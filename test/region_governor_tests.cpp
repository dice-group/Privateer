// The memory governor: option validation, the dirty budget (soft-mark
// edge-triggered cleaning, the hard-mark writer wait with its timeout
// overshoot, close forwarding a parked writer), and the resident budget
// (smaps accounting, victim selection, PAGEOUT trim, the close handshake).

#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/resident.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/mman.h>

using namespace privateer;
using namespace std::chrono_literals;
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

	// the injected cleaner clock; the backoff never engages in these tests
	std::atomic<int64_t> g_now{0};

	struct RegionGovernorTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();
		int64_t (*real_clock_)() = nullptr;

		void SetUp() override {
			g_now.store(0);
			real_clock_ = detail_region::clock_fn;
			detail_region::clock_fn = [] { return g_now.load(); };
		}

		void TearDown() override {
			detail_region::clock_fn = real_clock_;
		}

		// Cleaner driven purely by governor wakes: the interval fallback
		// never fires inside a test, and no backoff engages. Watermarks in
		// blocks; hard 0 means writers never wait.
		[[nodiscard]] region_options options(uint64_t soft_blocks, uint64_t low_blocks,
											 uint64_t hard_blocks,
											 std::chrono::nanoseconds hard_timeout = 10s) const {
			region_options opts;
			opts.block_size = bs;
			opts.cleaner.mode = cleaner_mode::non_durable;
			opts.cleaner.interval = std::chrono::hours{1};
			opts.cleaner.batch_slots = 8;
			opts.cleaner.backoff_base = std::chrono::nanoseconds{0};
			opts.cleaner.backoff_cap = std::chrono::nanoseconds{0};
			opts.governor.dirty_soft = soft_blocks * bs;
			opts.governor.dirty_low = low_blocks * bs;
			opts.governor.dirty_hard = hard_blocks * bs;
			opts.governor.hard_timeout = hard_timeout;
			return opts;
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	// --- configuration validation ---

	TEST_F(RegionGovernorTest, TheDirtyBudgetRequiresTheCleaner) {
		auto opts = options(2, 0, 4);
		opts.cleaner.mode = cleaner_mode::off;
		auto reg = region::create(dir.path, 16 * bs, opts);
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::invalid_argument);
	}

	TEST_F(RegionGovernorTest, WatermarkOrderIsValidated) {
		{
			auto opts = options(2, 3, 4);  // low above soft
			auto reg = region::create(dir.path / "a", 16 * bs, opts);
			ASSERT_FALSE(reg.has_value());
			EXPECT_EQ(reg.error().code, errc::invalid_argument);
		}
		{
			auto opts = options(4, 0, 2);  // hard below soft
			auto reg = region::create(dir.path / "b", 16 * bs, opts);
			ASSERT_FALSE(reg.has_value());
			EXPECT_EQ(reg.error().code, errc::invalid_argument);
		}
		{
			auto opts = options(2, 0, 4, 0ns);  // hard mark without a timeout
			auto reg = region::create(dir.path / "c", 16 * bs, opts);
			ASSERT_FALSE(reg.has_value());
			EXPECT_EQ(reg.error().code, errc::invalid_argument);
		}
		{
			region_options opts;  // hard or low without a soft watermark
			opts.block_size = bs;
			opts.governor.dirty_hard = 4 * bs;
			auto reg = region::create(dir.path / "d", 16 * bs, opts);
			ASSERT_FALSE(reg.has_value());
			EXPECT_EQ(reg.error().code, errc::invalid_argument);
		}
	}

	TEST_F(RegionGovernorTest, ResidentBudgetIsValidated) {
		region_options opts;
		opts.block_size = bs;
		opts.governor.resident_soft = 4 * bs;
		opts.governor.resident_low = 8 * bs;  // low above soft
		auto reg = region::create(dir.path / "a", 16 * bs, opts);
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::invalid_argument);

		opts.governor.resident_low = 0;
#ifdef __linux__
		auto valid = region::create(dir.path / "b", 16 * bs, opts);
		EXPECT_TRUE(valid.has_value());
#else
		auto valid = region::create(dir.path / "b", 16 * bs, opts);
		ASSERT_FALSE(valid.has_value());  // the resident budget is Linux-only
		EXPECT_EQ(valid.error().code, errc::invalid_argument);
#endif
	}

	TEST_F(RegionGovernorTest, AGovernedReadOnlyOpenIgnoresTheBudgets) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open_read_only(dir.path, options(1, 0, 2));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(bytes(*reg)[0], 'x');
	}

	// --- the dirty budget ---

	TEST_F(RegionGovernorTest, TheSoftMarkCrossingWakesTheCleaner) {
		// The cleaner interval is one hour: only the handler's crossing wake
		// can activate the drain inside this test.
		auto reg = region::create(dir.path, 16 * bs, options(2, 0, 0));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(8 * bs));
		auto &table = detail_region::table_of(*reg);

		for (size_t slot = 0; slot < 3; ++slot) {
			bytes(*reg)[slot * bs] = 'a';
		}
		// three dirty blocks crossed the two-block soft mark; the cleaner
		// drains to the low target without any timer
		EXPECT_TRUE(eventually([&] { return table.dirty_slots() == 0; }));
		EXPECT_EQ(table.load(0), slot_state::clean);
	}

	TEST_F(RegionGovernorTest, ADirtyPublishAboveTheSoftMarkWakesAParkedCleaner) {
		// The lost-wakeup regression: the cleaner scans while the only
		// counted slot is still materializing (a writer mid-fault), finds
		// nothing in dirty state, and parks on the governor word. The next
		// dirty publish is its only wake, so the handler must wake at every
		// publish at or above the soft mark, not only at the exact
		// crossing. Slot 5 plays the frozen writer: claimed and counted by
		// hand, never published.
		region_options opts = options(0, 0, 0);
		opts.governor.dirty_soft = 1;  // one byte: every dirty slot is above it
		auto reg = region::create(dir.path, 16 * bs, opts);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(8 * bs));
		auto &table = detail_region::table_of(*reg);

		ASSERT_TRUE(table.try_claim(5, slot_state::empty, slot_state::materializing));
		(void) table.add_dirty();
		// let the cleaner scan the counted-but-not-dirty state and park
		std::this_thread::sleep_for(200ms);

		bytes(*reg)[6 * bs] = 'a';  // the publish must wake the parked cleaner
		EXPECT_TRUE(eventually([&] { return table.load(6) == slot_state::clean; }));

		// unfreeze the fake writer with the handler's publish-then-wake
		// protocol; the cleaner drains it like any dirt
		table.publish(5, slot_state::dirty);
		table.wake_governor();
		EXPECT_TRUE(eventually([&] { return table.dirty_slots() == 0; }));
	}

	TEST_F(RegionGovernorTest, TheDirtyBudgetHoldsUnderBulkLoad) {
		uint64_t const hard_blocks = 4;
		auto reg = region::create(dir.path, 64 * bs, options(2, 0, hard_blocks));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(64 * bs));
		auto &table = detail_region::table_of(*reg);

		// A single writer: the gate admits a write only while the result
		// stays at or below the hard mark, so the count never exceeds it
		// (the 10 s timeout is never reached while the cleaner drains).
		uint64_t max_dirty = 0;
		for (size_t slot = 0; slot < 64; ++slot) {
			bytes(*reg)[slot * bs] = static_cast<unsigned char>(slot);
			max_dirty = std::max(max_dirty, table.dirty_slots());
		}
		EXPECT_LE(max_dirty, hard_blocks);
		// residual dirt below the soft mark is legal; above it the cleaner
		// still owes a drain
		EXPECT_TRUE(eventually([&] { return table.dirty_slots() * bs <= 2 * bs; }));
		// The commit quiesces the write-back (batches hold the commit
		// mutex, and nothing stays dirty), so the reads below never race a
		// remap; TSan models a remap as a plain write.
		ASSERT_TRUE(reg->commit(true));
		// nothing was lost to the write-backs
		for (size_t slot = 0; slot < 64; ++slot) {
			EXPECT_EQ(bytes(*reg)[slot * bs], static_cast<unsigned char>(slot));
		}
	}

	TEST_F(RegionGovernorTest, TheHardMarkWaitIsBoundedByTheTimeout) {
		// Soft sits at the hard mark, so the cleaner is never activated and
		// nothing drains: the writer must be released by the timeout alone.
		auto const timeout = 300ms;
		auto reg = region::create(dir.path, 16 * bs, options(1, 0, 1, timeout));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(4 * bs));
		auto &table = detail_region::table_of(*reg);

		bytes(*reg)[0] = 'a';
		ASSERT_EQ(table.dirty_slots(), 1u);
		// park the only dirty slot in a transient claim: undrainable
		ASSERT_TRUE(table.try_claim(0, slot_state::dirty, slot_state::syncing));

		auto const start = std::chrono::steady_clock::now();
		bytes(*reg)[bs] = 'b';  // would take the total above the hard mark
		auto const elapsed = std::chrono::steady_clock::now() - start;
		EXPECT_GE(elapsed, timeout);
		EXPECT_LT(elapsed, 10 * timeout);
		// the write overshot to two dirty blocks; the woken cleaner may
		// have written the fresh one back by the time this samples
		EXPECT_GE(table.dirty_slots(), 1u);
		EXPECT_LE(table.dirty_slots(), 2u);

		table.publish(0, slot_state::dirty);  // release the parked claim
		// the commit quiesces the write-back before the content reads
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(bytes(*reg)[0], 'a');
		EXPECT_EQ(bytes(*reg)[bs], 'b');
	}

	TEST_F(RegionGovernorTest, ADrainReleasesTheHardMarkWaiterEarly) {
		// The timeout is far above the test budget: only the cleaner's
		// decrease can release the writer in time.
		auto reg = region::create(dir.path, 16 * bs, options(1, 0, 2, 60s));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(8 * bs));
		auto &table = detail_region::table_of(*reg);

		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';
		auto const start = std::chrono::steady_clock::now();
		bytes(*reg)[2 * bs] = 'c';  // blocks until the cleaner drains a slot
		auto const elapsed = std::chrono::steady_clock::now() - start;
		EXPECT_LT(elapsed, 30s);
		// the commit quiesces the write-back before the content read
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(bytes(*reg)[2 * bs], 'c');
	}

	TEST_F(RegionGovernorTest, ABlockedWriterHoldingApplicationLocksMakesProgress) {
		auto reg = region::create(dir.path, 64 * bs, options(2, 0, 4, 60s));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(32 * bs));

		// The drain runs on the executor and takes no application locks, so
		// a writer blocked at the hard mark may hold any of its own locks.
		std::mutex application_lock;
		std::atomic<bool> done{false};
		std::thread writer{[&] {
			(void) arm_thread_fault_stack();
			std::lock_guard const lock{application_lock};
			for (size_t slot = 0; slot < 32; ++slot) {
				bytes(*reg)[slot * bs] = static_cast<unsigned char>(slot);
			}
			done.store(true);
		}};
		EXPECT_TRUE(eventually([&] { return done.load(); }, 30s));
		writer.join();
	}

	TEST_F(RegionGovernorTest, APoisonedSlotFailsCommitsAndCloseDoesNotHang) {
		auto reg = region::create(dir.path, 16 * bs, options(2, 0, 4));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(4 * bs));
		auto &table = detail_region::table_of(*reg);

		// a dead slot, exactly as the handler's failed-mprotect path leaves
		// it: counter balanced, terminal poisoned published
		bytes(*reg)[bs] = 'x';
		table.publish(1, slot_state::poisoned);
		table.sub_dirty();

		auto committed = reg->commit(true);
		ASSERT_FALSE(committed.has_value());
		EXPECT_EQ(committed.error().code, errc::region_poisoned);
		auto freed = reg->free_region(bs, bs);
		ASSERT_FALSE(freed.has_value());
		EXPECT_EQ(freed.error().code, errc::region_poisoned);
		// close completes although the cleaner is live and a slot is dead;
		// the test ends by destroying the region
	}

	TEST_F(RegionGovernorTest, CloseForwardsAParkedHardMarkWaiterAsACrash) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();  // the sandbox cleared the dispositions
			// In the fork child no executor thread exists, so nothing ever
			// drains: the writer parks at the hard mark until close wakes
			// it, sees closing, and forwards the fault.
			auto reg = region::open(dir.path, options(1, 0, 1, std::chrono::hours{1}));
			if (!reg) {
				return 10;
			}
			// the segment pointer is taken before the writer starts: the
			// thread must not touch the region handle the close moves
			auto *const seg = static_cast<unsigned char volatile *>(reg->segment());
			seg[0] = 'a';
			std::atomic<bool> parked{false};
			std::thread writer{[&] {
				(void) arm_thread_fault_stack();
				parked.store(true);
				seg[bs] = 'b';
			}};
			writer.detach();
			while (!parked.load()) {
				std::this_thread::yield();
			}
			std::this_thread::sleep_for(200ms);  // let the writer reach the gate
			{
				auto closing = std::move(*reg);  // close while a writer is parked: the contract violation
			}
			std::this_thread::sleep_for(10s);    // the forwarded fault must kill us first
			return 11;
		};
		EXPECT_TRUE(is_fault_signal(res)) << static_cast<int>(res);
	}

	// --- the resident budget ---

	TEST(SmapsRollup, ParsesPssAndPrivateDirty) {
		auto const usage = parse_smaps_rollup(
				"00400000-7fff0000 ---p 00000000 00:00 0    [rollup]\n"
				"Rss:              123456 kB\n"
				"Pss:               42000 kB\n"
				"Pss_Anon:          10000 kB\n"
				"Shared_Clean:       1000 kB\n"
				"Private_Dirty:      8000 kB\n");
		ASSERT_TRUE(usage.has_value()) << to_string(usage.error());
		EXPECT_EQ(usage->pss, 42000u * 1024);
		EXPECT_EQ(usage->private_dirty, 8000u * 1024);
	}

	TEST(SmapsRollup, MissingLinesFail) {
		EXPECT_FALSE(parse_smaps_rollup("Rss: 1 kB\n").has_value());
		EXPECT_FALSE(parse_smaps_rollup("Pss: 1 kB\n").has_value());
		EXPECT_FALSE(parse_smaps_rollup("Pss: junk\nPrivate_Dirty: 1 kB\n").has_value());
	}

#ifdef __linux__

	TEST(SmapsRollup, ReadsTheLiveProcess) {
		auto const usage = read_resident_usage();
		ASSERT_TRUE(usage.has_value()) << to_string(usage.error());
		EXPECT_GT(usage->pss, 0u);
	}

	// the injected residency and trim seams
	std::atomic<uint64_t> g_resident{0};
	std::vector<void *> g_pageout_calls;

	struct RegionResidentTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();
		result<uint64_t> (*real_resident_)() = nullptr;
		int (*real_pageout_)(void *, size_t) = nullptr;

		void SetUp() override {
			g_resident.store(0);
			g_pageout_calls.clear();
			real_resident_ = detail_region::resident_bytes_fn;
			real_pageout_ = detail_region::pageout_fn;
		}

		void TearDown() override {
			detail_region::resident_bytes_fn = real_resident_;
			detail_region::pageout_fn = real_pageout_;
		}

		[[nodiscard]] region_options resident_options(uint64_t soft, uint64_t low) const {
			region_options opts;
			opts.block_size = bs;
			opts.governor.resident_soft = soft;
			opts.governor.resident_low = low;
			opts.governor.sweep_interval = std::chrono::hours{1};
			return opts;
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}

		// touch-read every byte of a slot so its pages are resident
		static void scan_slot(region &reg, size_t slot, uint64_t bs) {
			unsigned char sink = 0;
			for (uint64_t i = 0; i < bs; ++i) {
				sink += bytes(reg)[slot * bs + i];
			}
			(void) sink;
		}
	};

	TEST_F(RegionResidentTest, TheSweepTrimsCleanSlotsAndSparesDirtyOnes) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b', 'c', 'd'});
		auto reg = region::open(dir.path, resident_options(2 * bs, 0));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		for (size_t slot = 0; slot < 4; ++slot) {
			scan_slot(*reg, slot, bs);  // resident, still clean
		}
		bytes(*reg)[3 * bs] = 'D';  // slot 3 turns dirty: never a victim

		detail_region::resident_bytes_fn = [] { return result<uint64_t>{g_resident.load()}; };
		detail_region::pageout_fn = [](void *addr, size_t) {
			g_pageout_calls.push_back(addr);
			return 0;
		};

		// below the soft mark: the sweep does nothing
		g_resident.store(bs);
		EXPECT_EQ(detail_region::run_resident_sweep(), 0u);
		EXPECT_TRUE(g_pageout_calls.empty());

		// far above: every clean victim is asked out, the dirty slot never
		g_resident.store(1000 * bs);
		EXPECT_GT(detail_region::run_resident_sweep(), 0u);
		EXPECT_EQ(g_pageout_calls.size(), 3u);
		auto *const base = static_cast<std::byte *>(reg->segment());
		for (void *const addr : g_pageout_calls) {
			EXPECT_NE(addr, base + 3 * bs);
		}
	}

	TEST_F(RegionResidentTest, AClosedRegionIsNeverTouchedBySweeps) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		{
			auto reg = region::open(dir.path, resident_options(2 * bs, 0));
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			scan_slot(*reg, 0, bs);
		}  // close removes the entry under the sweep mutex
		detail_region::resident_bytes_fn = [] { return result<uint64_t>{g_resident.load()}; };
		detail_region::pageout_fn = [](void *addr, size_t) {
			g_pageout_calls.push_back(addr);
			return 0;
		};
		g_resident.store(1000 * bs);
		EXPECT_EQ(detail_region::run_resident_sweep(), 0u);
		EXPECT_TRUE(g_pageout_calls.empty());
	}

	// the counting, failing residency probe of the disable test
	std::atomic<int> g_resident_probe_calls{0};

	TEST_F(RegionResidentTest, AFailedResidencyReadDisablesOnlyTheResidentBudget) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		g_resident_probe_calls.store(0);
		detail_region::resident_bytes_fn = [] {
			g_resident_probe_calls.fetch_add(1);
			return result<uint64_t>{
					std::unexpected{error{errc::io_error, EACCES, "read smaps_rollup"}}};
		};
		auto reg = region::open(dir.path, resident_options(2 * bs, 0));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		scan_slot(*reg, 0, bs);

		// the failed read disables the budget; later passes never probe again
		EXPECT_EQ(detail_region::run_resident_sweep(), 0u);
		EXPECT_EQ(g_resident_probe_calls.load(), 1);
		EXPECT_EQ(detail_region::run_resident_sweep(), 0u);
		EXPECT_EQ(g_resident_probe_calls.load(), 1);

		// nothing else died: the region writes, commits, and closes clean
		EXPECT_TRUE(reg->check_sanity());
		bytes(*reg)[0] = 'A';
		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'A');
	}

	TEST_F(RegionResidentTest, RealPageoutEvictsCleanSlotsAndContentSurvives) {
		// The real MADV_PAGEOUT against a disk-backed datastore: the test
		// runs in the build tree's working directory, never on tmpfs, where
		// PAGEOUT would no-op (see the deployment notes). The residency
		// number is injected so the assertion does not depend on the
		// process-wide Pss; the trim itself is real.
		auto cwd_store = fs::current_path() / "privateer-governor-resident-test";
		fs::remove_all(cwd_store);
		privateer::testing::build_committed_store(cwd_store, bs, {'a', 'b', 'c', 'd'});
		{
			auto reg = region::open(cwd_store, resident_options(2 * bs, 0));
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			for (size_t slot = 0; slot < 4; ++slot) {
				scan_slot(*reg, slot, bs);
			}

			auto const resident_pages = [&](size_t slot) {
				std::vector<unsigned char> vec(bs / page_size());
				auto *const addr = static_cast<std::byte *>(reg->segment()) + slot * bs;
				if (::mincore(addr, bs, vec.data()) != 0) {
					return size_t{0};
				}
				size_t pages = 0;
				for (unsigned char const flags : vec) {
					pages += flags & 1;
				}
				return pages;
			};
			size_t const before = resident_pages(0);
			ASSERT_GT(before, 0u);

			detail_region::resident_bytes_fn = [] { return result<uint64_t>{g_resident.load()}; };
			g_resident.store(1000 * bs);  // force a full trim; PAGEOUT stays real
			EXPECT_GT(detail_region::run_resident_sweep(), 0u);
			// The kernel may serve one PAGEOUT partially under load, so the
			// poll keeps sweeping; any eviction proves the trim works.
			EXPECT_TRUE(eventually([&] {
				if (resident_pages(0) < before) {
					return true;
				}
				(void) detail_region::run_resident_sweep();
				return resident_pages(0) < before;
			})) << "PAGEOUT did not evict; tmpfs working directory?";

			// evicted pages fault back in from the block files unchanged
			EXPECT_EQ(bytes(*reg)[0], 'a');
			EXPECT_EQ(bytes(*reg)[bs], 'b');
			bytes(*reg)[0] = 'A';
			EXPECT_TRUE(reg->commit(true));
		}
		{
			auto reopened = region::open(cwd_store);
			ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
			EXPECT_EQ(bytes(*reopened)[0], 'A');
		}
		fs::remove_all(cwd_store);
	}

#endif  // __linux__

	// --- writers versus the governed cleaner, the stress leg ---

	TEST_F(RegionGovernorTest, ConcurrentWritersUnderTheBudgetLoseNothing) {
		size_t const writers = 4;
		size_t const slots_per_writer = 16;
		auto reg = region::create(dir.path, writers * slots_per_writer * bs,
								  options(2, 0, 4, 60s));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(writers * slots_per_writer * bs));
		auto &table = detail_region::table_of(*reg);

		std::vector<std::thread> threads;
		for (size_t w = 0; w < writers; ++w) {
			threads.emplace_back([&, w] {
				(void) arm_thread_fault_stack();
				for (size_t i = 0; i < slots_per_writer; ++i) {
					size_t const slot = w * slots_per_writer + i;
					bytes(*reg)[slot * bs] = static_cast<unsigned char>(slot);
				}
			});
		}
		for (auto &thread : threads) {
			thread.join();
		}
		// the ceiling is hard plus one overshoot block per writer
		EXPECT_LE(table.dirty_slots(), 4u + writers);
		// the commit quiesces the write-back before the content reads
		ASSERT_TRUE(reg->commit(true));
		for (size_t slot = 0; slot < writers * slots_per_writer; ++slot) {
			EXPECT_EQ(bytes(*reg)[slot * bs], static_cast<unsigned char>(slot));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		for (size_t slot = 0; slot < writers * slots_per_writer; ++slot) {
			EXPECT_EQ(bytes(*reopened)[slot * bs], static_cast<unsigned char>(slot));
		}
	}

}  // namespace
