// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// The executor around the region: the parallel commit write-out, the
// region-owned task and timer lifecycle, and the close-time drain.

#include <gtest/gtest.h>

#include <privateer/executor.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

using namespace privateer;
using namespace std::chrono_literals;
using privateer::testing::count_data_block_files;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	// Patient by default: these waits assert that something happens at all,
	// not how fast. A sanitizer build on a small oversubscribed runner needs
	// the room, and the wait returns as soon as the condition holds.
	bool eventually(std::function<bool()> const &condition, std::chrono::seconds timeout = 60s) {
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (condition()) {
				return true;
			}
			std::this_thread::sleep_for(1ms);
		}
		return condition();
	}

	// The commit phase hook is a plain function pointer, so the gate it
	// parks in lives here. A commit that finished phase 1 holds the commit
	// mutex, has captured every slot that was dirty, and has not posted its
	// write-out workers yet.
	std::atomic<size_t> g_after_capture{0};
	std::atomic<bool> g_release_capture{false};

	void hold_the_commits_after_capture(int completed_phase) {
		if (completed_phase != 1) {
			return;
		}
		g_after_capture.fetch_add(1, std::memory_order_acq_rel);
		auto const deadline = std::chrono::steady_clock::now() + 60s;
		while (!g_release_capture.load(std::memory_order_acquire) &&
			   std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(1ms);
		}
	}

	// Holds work-pool threads, so a test decides how much of the pool is
	// left. The tasks own their state, because they can outlive the frame
	// that posted them, and every wait they make is bounded and released on
	// every exit path of that frame: a work-pool task that parks
	// unconditionally holds its thread for the rest of the process, and the
	// pool's join at static teardown then never returns.
	struct pool_blockers {
		struct shared {
			std::atomic<size_t> started{0};
			std::atomic<size_t> finished{0};
			std::atomic<bool> released{false};
		};
		std::shared_ptr<shared> state = std::make_shared<shared>();
		size_t count;

		explicit pool_blockers(size_t threads) : count{threads} {
			for (size_t i = 0; i < threads; ++i) {
				asio::post(work_pool(), [held = state] {
					held->started.fetch_add(1, std::memory_order_acq_rel);
					auto const deadline = std::chrono::steady_clock::now() + 60s;
					while (!held->released.load(std::memory_order_acquire) &&
						   std::chrono::steady_clock::now() < deadline) {
						std::this_thread::sleep_for(1ms);
					}
					held->finished.fetch_add(1, std::memory_order_acq_rel);
				});
			}
		}

		pool_blockers(pool_blockers const &) = delete;
		pool_blockers &operator=(pool_blockers const &) = delete;
		~pool_blockers() { release(); }

		void release() const noexcept { state->released.store(true, std::memory_order_release); }

		[[nodiscard]] size_t running() const {
			return state->started.load(std::memory_order_acquire);
		}
		[[nodiscard]] bool all_running() const { return running() == count; }
		[[nodiscard]] bool all_finished() const {
			return state->finished.load(std::memory_order_acquire) == count;
		}
	};

	struct RegionExecutorTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		void TearDown() override {
			detail_region::commit_post_fails_fn = nullptr;
			detail_region::commit_phase_hook = nullptr;
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}

		// Six slots covering every write-out kind: a new block, a dedup
		// twin, a value-identical rewrite, a freed slot, an untouched slot,
		// and a materialized empty sentinel.
		void dirty_and_commit(fs::path const &segment_dir, size_t workers) {
			privateer::testing::build_committed_store(segment_dir, bs,
													  {'x', 'x', 'y', 'z', 'w', std::nullopt});
			auto reg = region::open(segment_dir, {.commit_workers = workers});
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			bytes(*reg)[0] = 'a';
			bytes(*reg)[bs] = 'a';
			bytes(*reg)[2 * bs] = 'y';
			ASSERT_TRUE(reg->free_region(3 * bs, bs));
			bytes(*reg)[5 * bs] = 'q';
			ASSERT_TRUE(reg->commit(true));
			EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);
		}

		// One region of the full-pool case: the commit on a thread of its
		// own, and the cleaner batch one work-pool task runs against it.
		struct batch_run {
			std::atomic<bool> started{false};
			std::atomic<bool> done{false};
			std::atomic<size_t> cleaned{0};
		};
		struct pool_case {
			std::optional<region> reg;
			std::shared_ptr<batch_run> batch = std::make_shared<batch_run>();
			std::atomic<bool> commit_returned{false};
			std::atomic<bool> commit_ok{false};
			std::thread committer;
		};

		// Runs the commit of every region into a cleaner batch that owns the
		// last work-pool thread. Each region holds a commit past its capture,
		// where it owns the commit mutex, gets four more slots dirtied that
		// the capture did not take, and then gets one batch posted on the
		// pool. The pool has exactly one thread left per region, so a
		// commit's write-out worker runs only once a batch gives its thread
		// back.
		void commits_meet_cleaner_batches_on_a_full_pool(size_t regions) {
			ASSERT_GE(work_pool_size(), regions);
			pool_blockers const blockers{work_pool_size() - regions};
			ASSERT_TRUE(eventually([&] { return blockers.all_running(); }, 30s))
					<< "the work pool ran " << blockers.running() << " of " << blockers.count
					<< " posted tasks concurrently";

			std::vector<std::unique_ptr<pool_case>> cases;
			// Frees everything a commit can be waiting on, joins the
			// committers and closes the regions, on every exit path of this
			// frame. The order is what makes a failing run end: a commit
			// waiting for a write-out worker returns only once a pool thread
			// frees up.
			struct unwind {
				pool_blockers const &blockers;
				std::vector<std::unique_ptr<pool_case>> &cases;
				~unwind() {
					g_release_capture.store(true, std::memory_order_release);
					blockers.release();
					for (auto const &one : cases) {
						if (one->committer.joinable()) {
							one->committer.join();
						}
					}
					cases.clear();  // closing a region joins its posted batch
				}
			} const guard{blockers, cases};

			g_after_capture.store(0, std::memory_order_release);
			g_release_capture.store(false, std::memory_order_release);
			detail_region::commit_phase_hook = hold_the_commits_after_capture;

			for (size_t index = 0; index < regions; ++index) {
				fs::path const segment_dir = dir.path / std::to_string(index);
				privateer::testing::build_committed_store(
						segment_dir, bs, {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'});
				auto opened = region::open(segment_dir, {.commit_workers = 2});
				ASSERT_TRUE(opened.has_value()) << to_string(opened.error());
				auto one = std::make_unique<pool_case>();
				one->reg.emplace(std::move(*opened));
				// what the commit captures: four slots, so it posts a worker
				for (size_t slot = 0; slot < 4; ++slot) {
					bytes(*one->reg)[slot * bs] = static_cast<unsigned char>('A' + slot);
				}
				cases.push_back(std::move(one));
			}

			for (auto const &one : cases) {
				one->committer = std::thread{[state = one.get()] {
					auto const committed = state->reg->commit(true);
					state->commit_ok.store(committed.has_value(), std::memory_order_release);
					state->commit_returned.store(true, std::memory_order_release);
				}};
			}
			ASSERT_TRUE(eventually([&] { return g_after_capture.load() == regions; }, 30s))
					<< g_after_capture.load() << " of " << regions << " commits reached the capture";

			// What the batch is for: dirt the running commit did not capture
			// and will not write back.
			for (auto const &one : cases) {
				for (size_t slot = 4; slot < 8; ++slot) {
					bytes(*one->reg)[slot * bs] = static_cast<unsigned char>('A' + slot);
				}
				// the capture holds its four claims without releasing them, so
				// the dirty count carries both halves
				ASSERT_EQ(detail_region::table_of(*one->reg).dirty_slots(), 8u);
			}

			// The cleaner's own chain runs a batch exactly this way: a
			// region-owned task on the work pool.
			for (auto const &one : cases) {
				detail_region::post_task(*one->reg, [&target = *one->reg, batch = one->batch] {
					batch->started.store(true, std::memory_order_release);
					batch->cleaned.store(detail_region::run_cleaner_batch(target, false),
										 std::memory_order_release);
					batch->done.store(true, std::memory_order_release);
				});
			}
			auto const all = [&cases](auto &&reached) {
				return std::all_of(cases.begin(), cases.end(), reached);
			};
			ASSERT_TRUE(eventually(
					[&] {
						return all([](auto const &one) { return one->batch->started.load(); });
					},
					30s))
					<< "a cleaner batch never got a work-pool thread";

			// The commits leave the capture and post their write-out workers,
			// which have only the threads the batches are on to run on.
			g_release_capture.store(true, std::memory_order_release);

			// A detector, not a timeout: a commit with a thread for its
			// worker returns in milliseconds, and one whose worker is queued
			// behind a batch waiting for the commit mutex never returns.
			ASSERT_TRUE(eventually(
					[&] {
						return all([](auto const &one) { return one->commit_returned.load(); });
					},
					20s))
					<< "a commit did not return: its write-out worker is queued behind a cleaner "
					   "batch that waits for the commit mutex the commit holds";
			EXPECT_TRUE(eventually(
					[&] {
						return all([](auto const &one) { return one->batch->done.load(); });
					},
					20s));
			detail_region::commit_phase_hook = nullptr;

			for (auto const &one : cases) {
				EXPECT_TRUE(one->commit_ok.load());
				// The batch met the running commit and stepped aside: it
				// wrote nothing back, so its four victims are still dirty,
				// and the batch after it takes them.
				EXPECT_EQ(one->batch->cleaned.load(), 0u);
				auto &table = detail_region::table_of(*one->reg);
				EXPECT_EQ(table.dirty_slots(), 4u);
				EXPECT_EQ(detail_region::run_cleaner_batch(*one->reg, false), 4u);
				EXPECT_EQ(table.dirty_slots(), 0u);
				EXPECT_TRUE(one->reg->commit(true));
			}

			// No task this case posted still holds a work-pool thread.
			blockers.release();
			EXPECT_TRUE(eventually([&] { return blockers.all_finished(); }, 30s));
		}

		// what the regions of the case above hold after their last commit:
		// the commit's four slots and the cleaner's four
		void expect_persisted(size_t regions) {
			for (size_t index = 0; index < regions; ++index) {
				auto reopened = region::open(dir.path / std::to_string(index));
				ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
				for (size_t slot = 0; slot < 8; ++slot) {
					EXPECT_EQ(bytes(*reopened)[slot * bs], static_cast<unsigned char>('A' + slot))
							<< "region " << index << " slot " << slot;
				}
			}
		}
	};

	TEST_F(RegionExecutorTest, ParallelCommitMatchesTheSingleThreadedCommit) {
		dirty_and_commit(dir.path / "one", 1);
		dirty_and_commit(dir.path / "many", 8);

		// referenced blocks: the shared 'a', 'y', 'w', 'q'; 'x' and the
		// freed 'z' are reclaimed by the durable commit
		EXPECT_EQ(count_data_block_files(dir.path / "one"), 4u);
		EXPECT_EQ(count_data_block_files(dir.path / "many"), 4u);

		auto one = region::open(dir.path / "one");
		auto many = region::open(dir.path / "many");
		ASSERT_TRUE(one.has_value()) << to_string(one.error());
		ASSERT_TRUE(many.has_value()) << to_string(many.error());
		ASSERT_EQ(one->size(), many->size());
		for (uint64_t i = 0; i < one->size(); i += bs) {
			EXPECT_EQ(bytes(*one)[i], bytes(*many)[i]) << "slot " << i / bs;
		}
		EXPECT_EQ(bytes(*one)[3 * bs], 0);  // the freed slot persisted as empty
	}

	TEST_F(RegionExecutorTest, ManyWorkersOnFewSlotsCommitCleanly) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b', 'c'});
		auto reg = region::open(dir.path, {.commit_workers = 32});
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'd';
		bytes(*reg)[2 * bs] = 'e';
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);
		ASSERT_TRUE(reg->commit(true));  // an empty capture posts no workers
		bytes(*reg)[bs] = 'f';
		ASSERT_TRUE(reg->commit(false));

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'd');
		EXPECT_EQ(bytes(*reopened)[bs], 'f');
		EXPECT_EQ(bytes(*reopened)[2 * bs], 'e');
	}

	TEST_F(RegionExecutorTest, CloseWithQueuedTasksDrainsWithoutHang) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto opened = region::open(dir.path);
		ASSERT_TRUE(opened.has_value()) << to_string(opened.error());
		std::optional<region> reg{std::move(*opened)};

		// Occupy every work-pool thread, so the region task stays queued.
		pool_blockers const blockers{work_pool_size()};
		ASSERT_TRUE(eventually([&] { return blockers.all_running(); }, 30s))
				<< "the work pool started " << blockers.running() << " of " << blockers.count
				<< " posted tasks concurrently";

		detail_region::post_task(*reg, [] {});

		// Close joins the queued task: it cannot start before the blockers
		// leave, and close cannot finish before it is counted out.
		std::thread closer{[&] { reg.reset(); }};
		blockers.release();
		closer.join();
		EXPECT_FALSE(reg.has_value());

		// No task this test posted still holds a work-pool thread.
		EXPECT_TRUE(eventually([&] { return blockers.all_finished(); }, 30s));
	}

	// A cleaner batch runs on a work-pool thread and a commit posts its
	// write-out workers to the same pool and waits for them. A batch that
	// waited for the commit mutex would hold its thread while the commit
	// that owns the mutex waits for a worker queued behind that thread, so
	// on a pool with no thread to spare neither side would ever move again.
	// The pool with no thread to spare is a one-vCPU host, a pool that
	// started one thread after a failed thread creation, or any pool whose
	// threads are busy.
	TEST_F(RegionExecutorTest, ACommitOnAFullPoolFinishesPastACleanerBatch) {
		commits_meet_cleaner_batches_on_a_full_pool(1);
		expect_persisted(1);
	}

	// The same cycle spread over regions: two of them, each with a batch on
	// a work-pool thread of its own and a commit of its own.
	TEST_F(RegionExecutorTest, ManyRegionsOnAFullPoolFinishTheirCommits) {
		if (work_pool_size() < 2) {
			GTEST_SKIP() << "the work pool has one thread; the single-region case covers it";
		}
		commits_meet_cleaner_batches_on_a_full_pool(2);
		expect_persisted(2);
	}

	// The write-out fan-out posts one task per worker, and every posted
	// worker reads the committing thread's frame. A post that fails must not
	// leave a captured slot claimed, must not leave the join counter waiting
	// for a worker that never runs, and must not let the frame die under the
	// workers that do run.
	TEST_F(RegionExecutorTest, AFailedWorkerPostFailsTheCommitAndCapturesNothing) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b', 'c', 'd'});
		auto reg = region::open(dir.path, {.commit_workers = 4});
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		for (size_t slot = 0; slot < 4; ++slot) {
			bytes(*reg)[slot * bs] = static_cast<unsigned char>('A' + slot);
		}
		ASSERT_EQ(table.dirty_slots(), 4u);

		detail_region::commit_post_fails_fn = [](size_t worker) { return worker == 2; };
		auto const committed = reg->commit(true);
		detail_region::commit_post_fails_fn = nullptr;
		ASSERT_FALSE(committed.has_value());
		EXPECT_EQ(committed.error().code, errc::io_error);

		for (size_t slot = 0; slot < 4; ++slot) {
			slot_state const state = table.load(slot);
			EXPECT_FALSE(is_transient(state)) << "slot " << slot << " stayed " << to_string(state);
		}
		EXPECT_TRUE(reg->check_sanity());  // a failed post does not kill the region
		ASSERT_TRUE(reg->commit(true));    // and the retry commits every slot

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		for (size_t slot = 0; slot < 4; ++slot) {
			EXPECT_EQ(bytes(*reopened)[slot * bs], static_cast<unsigned char>('A' + slot));
		}
	}

	// A host with no thread budget left must get an answer from the executor.
	// asio's own thread_pool joins the threads it created when a later
	// creation fails, without stopping their scheduler, so that join never
	// returns and the failure becomes a deadlock in a function-local static.
	TEST(ExecutorThreadBudget, AStartWithoutThreadBudgetReportsInsteadOfHanging) {
		auto const res = PRIVATEER_SANDBOX {
			::alarm(60);  // a deadlock here must not sit until ctest's ceiling
			rlimit limit{};
			if (::getrlimit(RLIMIT_NPROC, &limit) != 0) {
				return 10;
			}
			limit.rlim_cur = 2;  // fewer than any pool of this size needs
			if (::setrlimit(RLIMIT_NPROC, &limit) != 0) {
				return 11;
			}
			// Either a degraded pool that runs the task, or no pool at all.
			// Both are answers; a hang is the defect this test exists for.
			return detail_executor::start_pool_and_run_task(16) == 0 ? 20 : 0;
		};
		EXPECT_TRUE(res == subprocess_result::exit_success || res == subprocess_result::exit_failure)
				<< "the executor start never reported back, signal " << static_cast<int>(res);
	}

	TEST_F(RegionExecutorTest, CloseCancelsAPendingTimer) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto opened = region::open(dir.path);
		ASSERT_TRUE(opened.has_value()) << to_string(opened.error());
		std::optional<region> reg{std::move(*opened)};

		std::atomic<int> fired{0};
		std::atomic<int> aborted_count{0};
		detail_region::start_timer(*reg, 10s, [&](bool aborted) {
			if (aborted) {
				aborted_count.fetch_add(1);
			}
			fired.fetch_add(1);
		});

		auto const before = std::chrono::steady_clock::now();
		reg.reset();  // close joins the handler, so it has run when reset returns
		auto const elapsed = std::chrono::steady_clock::now() - before;
		EXPECT_EQ(fired.load(), 1);
		EXPECT_EQ(aborted_count.load(), 1);
		EXPECT_LT(elapsed, 5s);  // the cancel completed the wait, not the expiry
	}

	TEST_F(RegionExecutorTest, ATimerFiresOnTheTimerPool) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto opened = region::open(dir.path);
		ASSERT_TRUE(opened.has_value()) << to_string(opened.error());

		std::atomic<int> fired{0};
		std::atomic<int> aborted_count{0};
		detail_region::start_timer(*opened, 1ms, [&](bool aborted) {
			if (aborted) {
				aborted_count.fetch_add(1);
			}
			fired.fetch_add(1);
		});
		ASSERT_TRUE(eventually([&] { return fired.load() == 1; }));
		EXPECT_EQ(aborted_count.load(), 0);
	}

	TEST_F(RegionExecutorTest, AThrowingTaskBodySetsTheErrorFlag) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());

		detail_region::post_task(*reg, [] { throw std::runtime_error{"task body failure"}; });
		ASSERT_TRUE(eventually([&] { return !reg->check_sanity(); }));

		auto const committed = reg->commit(true);
		ASSERT_FALSE(committed.has_value());
		EXPECT_EQ(committed.error().code, errc::datastore_inconsistent);
	}

	TEST_F(RegionExecutorTest, AForkChildCommitsWithoutTheParentsPools) {
		// a commit through the executor starts the pools in the parent;
		// their threads do not survive the fork below
		dirty_and_commit(dir.path / "warm", 2);

		fs::path const child_dir = dir.path / "child";
		privateer::testing::build_committed_store(child_dir, bs, {'a', 'b'});
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();  // the sandbox cleared the dispositions
			auto reg = region::open(child_dir);           // default worker count, degraded in the child
			if (!reg) {
				return 10;
			}
			static_cast<unsigned char volatile *>(reg->segment())[0] = 'c';
			if (!reg->commit(true)) {
				return 11;
			}
			return 0;
		};
		ASSERT_EQ(res, subprocess_result::exit_success);

		auto reopened = region::open(child_dir);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'c');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

}  // namespace
