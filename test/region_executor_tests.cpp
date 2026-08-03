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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

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

	struct RegionExecutorTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		void TearDown() override {
			detail_region::commit_post_fails_fn = nullptr;
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

		// The blocker tasks below can outlive the test frame, so they own
		// their state, and every wait they make is bounded and released on
		// every exit path of the frame. A work-pool task that parks
		// unconditionally holds its thread for the rest of the process, and
		// the pool's join at static teardown then never returns: the process
		// hangs instead of reporting a failed test.
		struct blocker_state {
			std::atomic<size_t> started{0};
			std::atomic<size_t> finished{0};
			std::atomic<bool> released{false};
		};
		auto const blockers = std::make_shared<blocker_state>();
		struct release_guard {
			std::shared_ptr<blocker_state> state;
			~release_guard() { state->released.store(true, std::memory_order_release); }
		} const guard{blockers};

		// Occupy every work-pool thread, so the region task stays queued.
		size_t const workers = work_pool_size();
		for (size_t i = 0; i < workers; ++i) {
			asio::post(work_pool(), [blockers] {
				blockers->started.fetch_add(1, std::memory_order_acq_rel);
				auto const deadline = std::chrono::steady_clock::now() + 60s;
				while (!blockers->released.load(std::memory_order_acquire) &&
					   std::chrono::steady_clock::now() < deadline) {
					std::this_thread::sleep_for(1ms);
				}
				blockers->finished.fetch_add(1, std::memory_order_acq_rel);
			});
		}
		ASSERT_TRUE(eventually([&] { return blockers->started.load() == workers; }, 30s))
				<< "the work pool started " << blockers->started.load() << " of " << workers
				<< " posted tasks concurrently";

		detail_region::post_task(*reg, [] {});

		// Close joins the queued task: it cannot start before the blockers
		// leave, and close cannot finish before it is counted out.
		std::thread closer{[&] { reg.reset(); }};
		blockers->released.store(true, std::memory_order_release);
		closer.join();
		EXPECT_FALSE(reg.has_value());

		// No task this test posted still holds a work-pool thread.
		EXPECT_TRUE(eventually([&] { return blockers->finished.load() == workers; }, 30s));
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
