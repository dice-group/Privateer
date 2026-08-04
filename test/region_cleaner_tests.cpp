// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// The background write-back: victim selection with cold-first order and
// re-dirty backoff, the write-back correctness under re-dirtying writers,
// the durability modes, and crash safety mid-clean.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/file_util.hpp>
#include <privateer/region.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#include <sys/stat.h>

// sanitizer detection: gcc defines __SANITIZE_*, clang answers __has_feature
#ifdef __SANITIZE_THREAD__
#define PRIVATEER_TEST_TSAN 1
#endif
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define PRIVATEER_TEST_TSAN 1
#endif
#endif

using namespace privateer;
using namespace std::chrono_literals;
using privateer::testing::count_data_block_files;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	bool eventually(std::function<bool()> const &condition, std::chrono::seconds timeout = 10s) {
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (condition()) {
				return true;
			}
			std::this_thread::sleep_for(1ms);
		}
		return condition();
	}

	// the injected cleaner clock; tests advance it by hand
	std::atomic<int64_t> g_now{0};

	// the inode of a file, 0 when it does not exist: what tells a rewrite
	// through a fresh file from a second sync of the same one
	ino_t inode_of(fs::path const &path) {
		struct stat st {};
		return ::stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
	}

	// the block file a slot holding fill in its first byte and zeros behind
	// it is named after
	fs::path block_of(fs::path const &segment_dir, uint64_t block_size, char fill) {
		std::vector<std::byte> data(block_size, std::byte{0});
		data[0] = static_cast<std::byte>(fill);
		auto store = block_store::open(segment_dir);
		EXPECT_TRUE(store.has_value());
		return store->block_path(hash_block(hash_algorithm::xxh3_128, data));
	}

	struct RegionCleanerTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();
		int64_t (*real_clock_)() = nullptr;

		void SetUp() override {
			g_now.store(0);
			real_clock_ = detail_region::clock_fn;
			detail_region::clock_fn = [] { return g_now.load(); };
			detail_file_util::sync_calls.store(0);
		}

		void TearDown() override {
			detail_region::clock_fn = real_clock_;
			detail_region::cleaner_slot_hook = nullptr;
			detail_region::cleaner_write_fails_fn = nullptr;
			detail_region::cleaner_alloc_fails_fn = nullptr;
			detail_region::cleaner_durability_fails_fn = nullptr;
		}

		// The interval keeps the timer from ever firing inside a test; the
		// batches are driven by hand. Backoff values are plain ticks of the
		// injected clock.
		[[nodiscard]] region_options options(cleaner_mode mode) const {
			region_options opts;
			opts.block_size = bs;
			opts.cleaner.mode = mode;
			opts.cleaner.interval = std::chrono::hours{1};
			opts.cleaner.batch_slots = 8;
			opts.cleaner.backoff_base = std::chrono::nanoseconds{100};
			opts.cleaner.backoff_cap = std::chrono::nanoseconds{800};
			return opts;
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	// A write-back changes the recipe entry of its slot, so the segment holding
	// that slot must be republished even though the commit captures nothing.
	TEST_F(RegionCleanerTest, AWriteBackMarksItsSegmentForTheNextCommit) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto const before = privateer::testing::manifest_records(dir.path);
		ASSERT_EQ(before.size(), 1u);

		bytes(*reg)[0] = 'a';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);

		ASSERT_TRUE(reg->commit(true));
		auto const after = privateer::testing::manifest_records(dir.path);
		ASSERT_EQ(after.size(), 1u);
		EXPECT_NE(after[0], before[0]);

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
	}

	TEST_F(RegionCleanerTest, ABatchWritesTheDirtySlotsBack) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';
		auto &table = detail_region::table_of(*reg);
		EXPECT_EQ(table.dirty_slots(), 1u);

		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(table.load(0), slot_state::clean);
		EXPECT_EQ(table.dirty_slots(), 0u);
		EXPECT_EQ(bytes(*reg)[0], 'a');  // the remap swapped identical bytes

		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'y');
	}

	TEST_F(RegionCleanerTest, ACleanedSlotThatRedirtiesIsRecapturedWithFinalContent) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);

		bytes(*reg)[0] = 'a';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		bytes(*reg)[0] = 'b';  // the write barrier re-dirties the cleaned slot
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 1u);

		ASSERT_TRUE(reg->commit(true));
		// the superseded 'a' version and the replaced 'x' are both reclaimed
		EXPECT_EQ(count_data_block_files(dir.path), 1u);

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'b');
	}

	TEST_F(RegionCleanerTest, ACleanedNeverRedirtiedSlotIsSkippedByTheNextCommit) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 2u);
		EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);

		// The commit captures nothing and publishes no new block; it only
		// syncs what the cleaner wrote, renames the recipe, and reclaims the
		// replaced blocks.
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(count_data_block_files(dir.path), 2u);

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

	TEST_F(RegionCleanerTest, TheWriteOutCountersCountWhatTheCleanerWrites) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';   // new content: a fresh block file
		bytes(*reg)[bs] = 'y';  // dirtied but unchanged: the write is skipped
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 2u);
		{
			auto const stats = reg->statistics();
			EXPECT_EQ(stats.slots_hashed, 2u);
			EXPECT_EQ(stats.slots_written, 1u);
			EXPECT_EQ(stats.slots_skipped, 1u);
			EXPECT_EQ(stats.slots_deduped, 0u);
			EXPECT_EQ(stats.slots_cleaned, 2u);
		}

		// Both slots are clean now, so the commit captures nothing and hashes
		// nothing: the cleaner already paid for this content.
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(reg->statistics().slots_hashed, 2u);
	}

	TEST_F(RegionCleanerTest, NonDurableCleaningLeavesTheSyncsToTheNextDurableCommit) {
		privateer::testing::build_committed_store(dir.path, bs, {std::nullopt, std::nullopt});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';

		detail_file_util::sync_calls.store(0);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(detail_file_util::sync_calls.load(), 0u);

		bytes(*reg)[bs] = 'b';
		detail_file_util::sync_calls.store(0);
		ASSERT_TRUE(reg->commit(true));
		// both block files and their shard entries, plus the recipe rename
		EXPECT_GE(detail_file_util::sync_calls.load(), 5u);
	}

	TEST_F(RegionCleanerTest, EagerDurableCleaningMakesTheNextDurableCommitPayOnlyRecentDirt) {
		privateer::testing::build_committed_store(dir.path, bs, {std::nullopt, std::nullopt});
		auto reg = region::open(dir.path, options(cleaner_mode::eager_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';

		// the batch pays the full durable-name contract for what it wrote:
		// the block file and its shard directory entry
		detail_file_util::sync_calls.store(0);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(detail_file_util::sync_calls.load(), 2u);

		// the durable commit pays only for the dirt the cleaner has not
		// reached: the fresh block and its shard entry, the recipe's segment
		// file and its shard entry, plus the manifest file and the segment
		// directory of the rename
		bytes(*reg)[bs] = 'b';
		detail_file_util::sync_calls.store(0);
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(detail_file_util::sync_calls.load(), 6u);

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

	TEST_F(RegionCleanerTest, TheRedirtyBackoffIsExponentialCappedAndClearedByAQuietPeriod) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto const batch = [&] { return detail_region::run_cleaner_batch(*reg, false); };

		// the first write-back of a slot carries no backoff
		bytes(*reg)[0] = 'a';
		EXPECT_EQ(batch(), 1u);

		// a re-dirty soon after the write-back waits out backoff_base
		bytes(*reg)[0] = 'b';
		g_now.store(10);  // observed here; eligible at 110
		EXPECT_EQ(batch(), 0u);
		g_now.store(109);
		EXPECT_EQ(batch(), 0u);
		g_now.store(110);
		EXPECT_EQ(batch(), 1u);

		// the next re-dirty doubles the backoff
		bytes(*reg)[0] = 'c';
		g_now.store(120);  // eligible at 320
		EXPECT_EQ(batch(), 0u);
		g_now.store(319);
		EXPECT_EQ(batch(), 0u);
		g_now.store(320);
		EXPECT_EQ(batch(), 1u);

		// 400, then the cap of 800
		bytes(*reg)[0] = 'd';
		g_now.store(330);  // eligible at 730
		EXPECT_EQ(batch(), 0u);
		g_now.store(730);
		EXPECT_EQ(batch(), 1u);
		bytes(*reg)[0] = 'e';
		g_now.store(740);  // eligible at 1540
		EXPECT_EQ(batch(), 0u);
		g_now.store(1540);
		EXPECT_EQ(batch(), 1u);

		// the doubling stops at the cap
		bytes(*reg)[0] = 'f';
		g_now.store(1550);  // eligible at 2350, not 3150
		EXPECT_EQ(batch(), 0u);
		g_now.store(2350);
		EXPECT_EQ(batch(), 1u);

		// a quiet period longer than the cap clears the backoff
		bytes(*reg)[0] = 'g';
		g_now.store(2350 + 900);
		EXPECT_EQ(batch(), 1u);
	}

	TEST_F(RegionCleanerTest, TheOverrideIgnoresTheBackoff) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());

		bytes(*reg)[0] = 'a';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		bytes(*reg)[0] = 'b';
		g_now.store(10);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);  // backed off
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, true), 1u);   // taken regardless
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::clean);
		EXPECT_EQ(bytes(*reg)[0], 'b');
	}

	TEST_F(RegionCleanerTest, TheColdestSlotIsWrittenBackFirst) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y', 'z'});
		auto opts = options(cleaner_mode::non_durable);
		opts.cleaner.batch_slots = 1;
		auto reg = region::open(dir.path, opts);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);

		// two slots turn dirty now, a third later; a one-slot batch must
		// drain the older dirt before it touches the young slot
		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		g_now.store(50);
		bytes(*reg)[2 * bs] = 'c';
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(table.load(0), slot_state::clean);
		EXPECT_EQ(table.load(1), slot_state::clean);
		EXPECT_EQ(table.load(2), slot_state::dirty);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_EQ(table.load(2), slot_state::clean);
	}

	TEST_F(RegionCleanerTest, ACrashMidCleanLeavesTheOnDiskRecipeUntouched) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto const opts = options(cleaner_mode::non_durable);

		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();  // the sandbox cleared the dispositions
			detail_region::cleaner_slot_hook = [](size_t) { (void) ::raise(SIGKILL); };
			auto reg = region::open(dir.path, opts);
			if (!reg) {
				return 10;
			}
			static_cast<unsigned char volatile *>(reg->segment())[0] = 'a';
			static_cast<unsigned char volatile *>(reg->segment())[bs] = 'b';
			(void) detail_region::run_cleaner_batch(*reg, false);
			return 11;  // the kill inside the batch was never reached
		};
		ASSERT_EQ(res, subprocess_result::killed);

		// the block file the killed batch published is ordinary sweep
		// garbage; the recipe still names the committed content
		auto reopened = region::open(dir.path, opts);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'x');
		EXPECT_EQ(bytes(*reopened)[bs], 'y');
		EXPECT_EQ(count_data_block_files(dir.path), 2u);
	}

	TEST_F(RegionCleanerTest, AFailedWriteBackUnwindsTheSlotAndLeavesTheStoreHealthy) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		bytes(*reg)[0] = 'a';

		detail_region::cleaner_write_fails_fn = [](size_t) { return true; };
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		detail_region::cleaner_write_fails_fn = nullptr;

		// the slot is dirty again, writable, and the region is healthy
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 1u);
		EXPECT_TRUE(reg->check_sanity());
		bytes(*reg)[0] = 'b';  // a native store into the restored mapping

		// sync(true) succeeds, so close earns the consistency mark
		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'b');
	}

	// A batch holds its slot claims across the store's publish, which
	// allocates. An allocation failure there throws out of the batch, and
	// every claim it holds has to go back: a slot left in syncing is
	// read-only for every writer and invisible to every later capture, so
	// its content would never reach a recipe.
	TEST_F(RegionCleanerTest, AnAllocationFailureInABatchPutsItsClaimsBack) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';
		ASSERT_EQ(table.dirty_slots(), 2u);

		// the second slot of the batch throws while the first one is claimed
		detail_region::cleaner_alloc_fails_fn = [](size_t slot) { return slot == 1; };
		EXPECT_THROW((void) detail_region::run_cleaner_batch(*reg, false), std::bad_alloc);
		detail_region::cleaner_alloc_fails_fn = nullptr;

		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.load(1), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 2u);
		EXPECT_TRUE(reg->check_sanity());

		// both slots are captured again, so neither is missing from the recipe
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(table.load(0), slot_state::clean);
		EXPECT_EQ(table.load(1), slot_state::clean);
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

	TEST_F(RegionCleanerTest, AFailedBatchBacksTheCleanerOff) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';

		detail_region::cleaner_write_fails_fn = [](size_t) { return true; };
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);  // fails at 0
		detail_region::cleaner_write_fails_fn = nullptr;

		// no attempt before backoff_base has passed, even though the
		// failure is gone
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		g_now.store(99);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		g_now.store(100);
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_TRUE(reg->check_sanity());
	}

	TEST_F(RegionCleanerTest, RepeatedFailuresDisableTheCleanerAndTheStoreStaysHealthy) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto opts = options(cleaner_mode::non_durable);
		opts.cleaner.failure_limit = 3;
		auto reg = region::open(dir.path, opts);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		bytes(*reg)[0] = 'a';

		// each failed batch doubles the backoff; the third disables
		detail_region::cleaner_write_fails_fn = [](size_t) { return true; };
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);  // backoff until 100
		EXPECT_FALSE(detail_region::cleaner_disabled(*reg));
		g_now.store(100);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);  // backoff until 300
		g_now.store(300);
		ASSERT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		detail_region::cleaner_write_fails_fn = nullptr;
		EXPECT_TRUE(detail_region::cleaner_disabled(*reg));

		// disabled means no more attempts, not an unhealthy store
		g_now.store(100000);
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		EXPECT_TRUE(reg->check_sanity());
		ASSERT_TRUE(reg->commit(true));  // commit-time write-back still lands the data

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
	}

	TEST_F(RegionCleanerTest, AFailedDurabilityBarrierUnwindsTheWholeBatch) {
		privateer::testing::build_committed_store(dir.path, bs, {std::nullopt, std::nullopt});
		auto reg = region::open(dir.path, options(cleaner_mode::eager_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';

		detail_region::cleaner_durability_fails_fn = [] { return true; };
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		detail_region::cleaner_durability_fails_fn = nullptr;

		// both slots unwound to dirty, and the files the batch created are
		// gone: nothing a later publish could dedup against
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.load(1), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 2u);
		EXPECT_TRUE(reg->check_sanity());
		EXPECT_EQ(count_data_block_files(dir.path), 0u);

		// past the failure backoff the retry writes both back durably
		g_now.store(100);
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 2u);
		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

	// The eager barrier covers every name the batch's entries carry, and a
	// slot whose content still carries the name its entry holds writes no
	// file. When that barrier fails, the file behind that name is the one
	// the failed sync left, so the next batch writes it again instead of
	// skipping the slot a second time.
	TEST_F(RegionCleanerTest, AFailedEagerBarrierRewritesTheNameASkippedSlotCarries) {
		auto reg = region::create(dir.path, 8 * bs, options(cleaner_mode::eager_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_TRUE(reg->extend(bs));
		bytes(*reg)[0] = 'a';
		ASSERT_TRUE(reg->commit(false));  // publishes the block, no barrier owed yet
		bytes(*reg)[0] = 'a';             // dirty again, same content, same name
		auto const block = block_of(dir.path, bs, 'a');
		auto const before = inode_of(block);
		ASSERT_NE(before, 0u);

		detail_block_store::barrier_sync_fails_fn = [](block_digest const &) { return true; };
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 0u);
		detail_block_store::barrier_sync_fails_fn = nullptr;
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::dirty);

		g_now.store(100);  // past the failure backoff
		EXPECT_EQ(detail_region::run_cleaner_batch(*reg, false), 1u);
		EXPECT_NE(inode_of(block), before) << "the block was not written again";

		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
	}

	TEST_F(RegionCleanerTest, TheTimerDrivenCleanerDrainsDirtySlots) {
		detail_region::clock_fn = real_clock_;  // real backoff timings
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y', 'z'});
		auto opts = options(cleaner_mode::non_durable);
		opts.cleaner.interval = 10ms;
		opts.cleaner.batch_slots = 2;
		auto reg = region::open(dir.path, opts);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);

		bytes(*reg)[0] = 'a';
		bytes(*reg)[bs] = 'b';
		bytes(*reg)[2 * bs] = 'c';
		ASSERT_TRUE(eventually([&] { return table.dirty_slots() == 0; }));

		// the chain re-arms itself: fresh dirt drains again
		bytes(*reg)[0] = 'd';
		ASSERT_TRUE(eventually([&] { return table.dirty_slots() == 0; }));

		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'd');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
		EXPECT_EQ(bytes(*reopened)[2 * bs], 'c');
	}

	TEST_F(RegionCleanerTest, InvalidCleanerOptionsFailTheOpen) {
		privateer::testing::build_committed_store(dir.path, bs, {'x'});

		auto no_interval = options(cleaner_mode::non_durable);
		no_interval.cleaner.interval = std::chrono::nanoseconds{0};
		auto opened = region::open(dir.path, no_interval);
		ASSERT_FALSE(opened.has_value());
		EXPECT_EQ(opened.error().code, errc::invalid_argument);

		auto no_batch = options(cleaner_mode::non_durable);
		no_batch.cleaner.batch_slots = 0;
		opened = region::open(dir.path, no_batch);
		ASSERT_FALSE(opened.has_value());
		EXPECT_EQ(opened.error().code, errc::invalid_argument);

		auto inverted = options(cleaner_mode::non_durable);
		inverted.cleaner.backoff_base = std::chrono::nanoseconds{900};
		opened = region::open(dir.path, inverted);
		ASSERT_FALSE(opened.has_value());
		EXPECT_EQ(opened.error().code, errc::invalid_argument);
	}

	TEST_F(RegionCleanerTest, AHandshakedWriterAndTheCleanerAlternateSafely) {
		privateer::testing::build_committed_store(dir.path, bs, {'x', 'y', 'z', 'w'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());

		constexpr int rounds = 16;
		std::atomic<int> round_ready{-1};
		std::atomic<int> round_done{-1};
		std::thread writer{[&] {
			for (int r = 0; r < rounds; ++r) {
				for (uint64_t slot = 0; slot < 4; ++slot) {
					bytes(*reg)[slot * bs] = static_cast<unsigned char>('A' + r);
				}
				round_ready.store(r, std::memory_order_release);
				while (round_done.load(std::memory_order_acquire) < r) {
					std::this_thread::yield();
				}
			}
		}};
		for (int r = 0; r < rounds; ++r) {
			while (round_ready.load(std::memory_order_acquire) < r) {
				std::this_thread::yield();
			}
			// the override drains every slot: each round re-dirties what the
			// last round cleaned, so plain eligibility would back off
			EXPECT_EQ(detail_region::run_cleaner_batch(*reg, true), 4u);
			round_done.store(r, std::memory_order_release);
		}
		writer.join();

		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		for (uint64_t slot = 0; slot < 4; ++slot) {
			EXPECT_EQ(bytes(*reopened)[slot * bs], 'A' + rounds - 1) << "slot " << slot;
		}
	}

	TEST_F(RegionCleanerTest, AnUnsynchronizedWriterRacingTheCleanerIsSafe) {
#ifdef PRIVATEER_TEST_TSAN
		GTEST_SKIP() << "the writer races the freeze on purpose; TSan cannot see the mprotect ordering";
#endif
		privateer::testing::build_committed_store(dir.path, bs, {'x'});
		auto reg = region::open(dir.path, options(cleaner_mode::non_durable));
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());

		// The writer hammers the slot while batches freeze and remap it
		// underneath. Every write lands: a store that hits a frozen page
		// faults, waits the write-back out, and re-dirties the slot; the
		// commit then recaptures the final content.
		std::atomic<bool> stop{false};
		std::atomic<uint64_t> iterations{0};
		std::atomic<unsigned char> last{0};
		std::thread writer{[&] {
			unsigned char v = 0;
			while (!stop.load(std::memory_order_acquire)) {
				++v;
				for (uint64_t i = 0; i < 8; ++i) {
					bytes(*reg)[i] = v;
				}
				iterations.fetch_add(1, std::memory_order_release);
			}
			last.store(v, std::memory_order_release);
		}};
		// Wait until the writer is live, then keep racing until twenty
		// write-backs happened mid-stream. Both sides always progress: the
		// writer re-dirties the slot right after every clean, and a fault
		// waits out at most one write-back of its own slot.
		while (iterations.load(std::memory_order_acquire) == 0) {
			std::this_thread::yield();
		}
		size_t cleaned = 0;
		while (cleaned < 20) {
			cleaned += detail_region::run_cleaner_batch(*reg, true);
		}
		stop.store(true, std::memory_order_release);
		writer.join();

		ASSERT_TRUE(reg->commit(true));
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		for (uint64_t i = 0; i < 8; ++i) {
			EXPECT_EQ(bytes(*reopened)[i], last.load()) << "byte " << i;
		}
	}

}  // namespace
