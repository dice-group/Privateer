// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/file_util.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
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
using privateer::testing::count_segment_files;
using privateer::testing::manifest_records;
using privateer::testing::manifest_version;
namespace fs = std::filesystem;

namespace {

	bool eventually(std::function<bool()> const &condition, std::chrono::seconds timeout = 30s) {
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (condition()) {
				return true;
			}
			std::this_thread::sleep_for(1ms);
		}
		return condition();
	}

	// the protection-change seam: fails the next g_protect_fails write
	// upgrades, everything else passes through
	std::atomic<int> g_protect_fails{0};

	int failing_protect(void *addr, size_t len, int prot) {
		if ((prot & PROT_WRITE) != 0) {
			for (int budget = g_protect_fails.load(std::memory_order_acquire); budget != 0;) {
				if (budget < 0 ||
					g_protect_fails.compare_exchange_weak(budget, budget - 1,
														  std::memory_order_acq_rel)) {
					errno = ENOMEM;
					return -1;
				}
			}
		}
		return ::mprotect(addr, len, prot);
	}

	// the durability-barrier seam: fails every block file sync while it is on
	std::atomic<bool> g_barrier_fails{false};

	// the inode of every file in the store, by file name: what tells a
	// rewrite through a fresh file from a second sync of the same one
	std::map<std::string, ino_t> store_inodes(fs::path const &segment_dir) {
		std::map<std::string, ino_t> inodes;
		for (auto const &shard : fs::directory_iterator{segment_dir / "blocks"}) {
			for (auto const &entry : fs::directory_iterator{shard}) {
				struct stat st {};
				if (::stat(entry.path().c_str(), &st) == 0) {
					inodes.emplace(entry.path().filename().string(), st.st_ino);
				}
			}
		}
		return inodes;
	}

	struct RegionCommitTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		void TearDown() override {
			detail_region::mprotect_fn = ::mprotect;
			detail_block_store::barrier_sync_fails_fn = nullptr;
			g_protect_fails.store(0);
			g_barrier_fails.store(false);
		}

		// arms the barrier seam; it fails while g_barrier_fails is set
		static void arm_barrier_seam() {
			detail_block_store::barrier_sync_fails_fn = [](block_digest const &) {
				return g_barrier_fails.load(std::memory_order_relaxed);
			};
		}

		[[nodiscard]] region_options options() const {
			region_options opts;
			opts.block_size = bs;
			return opts;
		}

		region make_region(uint64_t extended_slots) {
			auto reg = region::create(dir.path, 8 * bs, options());
			EXPECT_TRUE(reg.has_value()) << to_string(reg.error());
			EXPECT_TRUE(reg->extend(extended_slots * bs));
			return std::move(*reg);
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	TEST_F(RegionCommitTest, ACommitPersistsDirtySlots) {
		{
			auto reg = make_region(2);
			bytes(reg)[0] = 'x';
			bytes(reg)[bs] = 'y';
			ASSERT_TRUE(reg.commit(true));
			auto &table = detail_region::table_of(reg);
			EXPECT_EQ(table.load(0), slot_state::clean);
			EXPECT_EQ(table.load(1), slot_state::clean);
			EXPECT_EQ(table.dirty_slots(), 0u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 2 * bs);
		EXPECT_EQ(bytes(*reopened)[0], 'x');
		EXPECT_EQ(bytes(*reopened)[bs], 'y');
	}

	TEST_F(RegionCommitTest, WritesStayReadableThroughTheCommit) {
		auto reg = make_region(1);
		bytes(reg)[7] = 'q';
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(bytes(reg)[7], 'q');  // the remap swaps identical bytes
		EXPECT_EQ(bytes(reg)[8], 0);
	}

	TEST_F(RegionCommitTest, ACommitOnAReadOnlyRegionIsANoOpSuccess) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open_read_only(dir.path);
		ASSERT_TRUE(reg.has_value());
		EXPECT_TRUE(reg->commit(true));
		EXPECT_TRUE(reg->commit(false));
		EXPECT_EQ(bytes(*reg)[0], 'a');
	}

	TEST_F(RegionCommitTest, AnEmptyDirtySetStillPersistsTheSize) {
		{
			auto reg = make_region(3);
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 3 * bs);
	}

	TEST_F(RegionCommitTest, ValueIdenticalWritesProduceNoNewBlock) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'a';  // dirtied, but the content is unchanged
		ASSERT_EQ(detail_region::table_of(*reg).dirty_slots(), 1u);
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(count_data_block_files(dir.path), 1u);
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::clean);
		EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);
	}

	TEST_F(RegionCommitTest, IdenticalSlotsDeduplicateIntoOneBlock) {
		{
			auto reg = make_region(2);
			bytes(reg)[0] = 'd';
			bytes(reg)[bs] = 'd';
			ASSERT_TRUE(reg.commit(true));
			EXPECT_EQ(count_data_block_files(dir.path), 1u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 'd');
		EXPECT_EQ(bytes(*reopened)[bs], 'd');
	}

	TEST_F(RegionCommitTest, TheWriteOutCountersSeparateFreshWritesDedupsAndSkips) {
		auto reg = make_region(3);
		bytes(reg)[0] = 'd';       // slot 0 and slot 1 end up with the same
		bytes(reg)[bs] = 'd';      // content, so the second one dedups
		bytes(reg)[2 * bs] = 'e';  // its own content, its own block file
		ASSERT_TRUE(reg.commit(true));
		{
			auto const stats = reg.statistics();
			EXPECT_EQ(stats.slots_hashed, 3u);
			EXPECT_EQ(stats.slots_written, 2u);
			EXPECT_EQ(stats.slots_deduped, 1u);
			EXPECT_EQ(stats.slots_skipped, 0u);
		}

		// Rewriting the same values dirties both slots again, so both are
		// hashed again; the content still carries the entry name, so no file
		// is written.
		bytes(reg)[0] = 'd';
		bytes(reg)[bs] = 'd';
		ASSERT_TRUE(reg.commit(true));
		auto const stats = reg.statistics();
		EXPECT_EQ(stats.slots_hashed, 5u);
		EXPECT_EQ(stats.slots_skipped, 2u);
		EXPECT_EQ(stats.slots_written, 2u);
		EXPECT_EQ(stats.slots_deduped, 1u);
	}

	TEST_F(RegionCommitTest, AnEmptiedSlotIsNotHashed) {
		auto reg = make_region(2);
		bytes(reg)[0] = 'a';
		bytes(reg)[bs] = 'b';
		ASSERT_TRUE(reg.commit(true));
		ASSERT_EQ(reg.statistics().slots_hashed, 2u);

		ASSERT_TRUE(reg.free_region(0, bs));
		ASSERT_TRUE(reg.commit(true));
		auto const stats = reg.statistics();
		EXPECT_EQ(stats.slots_hashed, 2u);  // an empty slot has no content to name
		EXPECT_EQ(stats.slots_written, 2u);
	}

	TEST_F(RegionCommitTest, ADurableCommitReclaimsTheRetiredBlock) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'b';
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(count_data_block_files(dir.path), 1u);  // the block holding 'a' is unlinked
		EXPECT_EQ(bytes(*reg)[0], 'b');
	}

	TEST_F(RegionCommitTest, ANonDurableCommitNeverUnlinks) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value());
			bytes(*reg)[0] = 'b';
			ASSERT_TRUE(reg->commit(false));
			// the retired block must survive: a lost rename has to be able to
			// resurface the old recipe with all its blocks intact
			EXPECT_EQ(count_data_block_files(dir.path), 2u);
		}
		auto reopened = region::open(dir.path);  // the sweep removes the retired block
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 'b');
		EXPECT_EQ(count_data_block_files(dir.path), 1u);
	}

	TEST_F(RegionCommitTest, ALaterDurableCommitReclaimsNonDurableRetirees) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'b';
		ASSERT_TRUE(reg->commit(false));
		ASSERT_EQ(count_data_block_files(dir.path), 2u);
		bytes(*reg)[0] = 'c';
		ASSERT_TRUE(reg->commit(true));
		// both retired names ('a' from the non-durable commit, 'b' from this
		// one) are unlinked once the rename is durable
		EXPECT_EQ(count_data_block_files(dir.path), 1u);
		EXPECT_EQ(bytes(*reg)[0], 'c');
	}

	TEST_F(RegionCommitTest, ADurableCommitSyncsTheNamesOfEarlierNonDurableCommits) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'x';
		ASSERT_TRUE(reg.commit(false));

		// Nothing is dirty, so the inherited name is all the barrier has to
		// do: its block file and its shard directory entry, the recipe's
		// segment file and its shard entry, plus the manifest file and the
		// segment directory of the rename.
		detail_file_util::sync_calls.store(0);
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(detail_file_util::sync_calls.load(), 6u);

		// Every name is durable now, and the recipe is unchanged, so its
		// segment file dedups onto a durable name: the next durable commit
		// pays the rename only.
		detail_file_util::sync_calls.store(0);
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(detail_file_util::sync_calls.load(), 2u);
	}

	TEST_F(RegionCommitTest, ANameRetiredBeforeItWasSyncedDoesNotFailTheBarrier) {
		{
			auto reg = make_region(1);
			bytes(reg)[0] = 'x';
			ASSERT_TRUE(reg.commit(false));  // the name of 'x' owes a sync
			bytes(reg)[0] = 'y';
			// nothing references the name of 'x' any more, so the barrier
			// leaves it out and the reclaim after the rename unlinks its file
			ASSERT_TRUE(reg.commit(true));
			EXPECT_EQ(count_data_block_files(dir.path), 1u);
			EXPECT_EQ(bytes(reg)[0], 'y');
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'y');
	}

	TEST_F(RegionCommitTest, ARetiredNameThatComesBackIsSyncedByTheNextBarrier) {
		{
			auto reg = make_region(1);
			bytes(reg)[0] = 'x';
			ASSERT_TRUE(reg.commit(false));
			bytes(reg)[0] = 'y';
			ASSERT_TRUE(reg.commit(true));  // the name of 'x' retires unsynced
			bytes(reg)[0] = 'x';            // and comes back

			// The returning name is written and synced like any other: its
			// block file and its shard entry, the recipe's segment file and
			// its shard entry, the manifest file and the segment directory of
			// the rename, and the shard entries of the reclaim that unlinks
			// the name of 'y' and the retired segment file.
			detail_file_util::sync_calls.store(0);
			ASSERT_TRUE(reg.commit(true));
			EXPECT_EQ(detail_file_util::sync_calls.load(), 8u);
			EXPECT_EQ(count_data_block_files(dir.path), 1u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'x');
	}

	TEST_F(RegionCommitTest, ADurableCommitSpreadsTheBarrierAndTheReclaim) {
		// Enough slots that the barrier and the reclaim pass cross the store's
		// spread floor, so both run over the commit workers instead of in
		// line. What must hold is unchanged: every block durable and named by
		// the recipe, and the retired ones unlinked.
		constexpr size_t slots = 6;
		auto opts = options();
		opts.commit_workers = 4;
		{
			auto reg = region::create(dir.path, 8 * bs, opts);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			ASSERT_TRUE(reg->extend(slots * bs));
			for (size_t slot = 0; slot < slots; ++slot) {
				bytes(*reg)[slot * bs] = static_cast<unsigned char>('a' + slot);
			}
			ASSERT_TRUE(reg->commit(true));
			EXPECT_EQ(count_data_block_files(dir.path), slots);

			// a second round retires all six names at once
			for (size_t slot = 0; slot < slots; ++slot) {
				bytes(*reg)[slot * bs] = static_cast<unsigned char>('A' + slot);
			}
			ASSERT_TRUE(reg->commit(true));
			EXPECT_EQ(count_data_block_files(dir.path), slots);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		for (size_t slot = 0; slot < slots; ++slot) {
			EXPECT_EQ(bytes(*reopened)[slot * bs], static_cast<unsigned char>('A' + slot));
		}
	}

	TEST_F(RegionCommitTest, TheDefaultWorkerCountStopsAtTheMeasuredPlateau) {
		auto reg = make_region(1);
		size_t const cores = std::max(1u, std::thread::hardware_concurrency());
		EXPECT_EQ(detail_region::commit_workers(reg), std::min<size_t>(cores, 16));

		auto opts = options();
		opts.commit_workers = 24;  // an explicit count is never capped
		privateer::testing::temp_dir other;
		auto explicit_workers = region::create(other.path, 8 * bs, opts);
		ASSERT_TRUE(explicit_workers.has_value()) << to_string(explicit_workers.error());
		EXPECT_EQ(detail_region::commit_workers(*explicit_workers), 24u);
	}

	TEST_F(RegionCommitTest, GrowthAcrossCommitsAccumulates) {
		{
			auto reg = make_region(1);
			bytes(reg)[0] = '1';
			ASSERT_TRUE(reg.commit(true));
			ASSERT_TRUE(reg.extend(3 * bs));
			bytes(reg)[2 * bs] = '3';
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(reopened->size(), 3 * bs);
		EXPECT_EQ(bytes(*reopened)[0], '1');
		EXPECT_EQ(bytes(*reopened)[bs], 0);
		EXPECT_EQ(bytes(*reopened)[2 * bs], '3');
	}

	TEST_F(RegionCommitTest, AFreedSegmentCollapsesToAllEmpty) {
		auto reg = make_region(2);
		bytes(reg)[0] = 'a';
		ASSERT_TRUE(reg.commit(true));
		ASSERT_EQ(count_segment_files(dir.path), 1u);
		ASSERT_EQ(count_data_block_files(dir.path), 1u);

		ASSERT_TRUE(reg.free_region(0, 2 * bs));
		ASSERT_TRUE(reg.commit(true));
		// every slot of the segment holds the sentinel now, so its record says
		// all_empty and the reclaim unlinked the file
		auto const records = manifest_records(dir.path);
		ASSERT_EQ(records.size(), 1u);
		EXPECT_EQ(records[0][0], 0u);  // the all_empty encoding
		EXPECT_EQ(count_segment_files(dir.path), 0u);
		EXPECT_EQ(count_data_block_files(dir.path), 0u);
	}

	TEST_F(RegionCommitTest, AVersionOneDatastoreRewritesEverySegmentOnItsFirstCommit) {
		privateer::testing::build_legacy_store(dir.path, bs, {'a', 'b'});
		ASSERT_EQ(manifest_version(dir.path), 1u);
		ASSERT_EQ(count_segment_files(dir.path), 0u);
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			ASSERT_TRUE(reg->commit(true));  // nothing is dirty, and every segment is
			EXPECT_EQ(manifest_version(dir.path), 2u);
			EXPECT_EQ(count_segment_files(dir.path), 1u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 2 * bs);
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
		EXPECT_EQ(count_data_block_files(dir.path), 2u);
	}

	// Regions that span more than one segment. A segment covers
	// recipe_segment_slots slots, so these stay cheap by materializing a
	// handful of slots and leaving the rest sparse.
	struct RegionSegmentTest : RegionCommitTest {
		uint64_t const seg = recipe_segment_slots;

		region make_wide_region(uint64_t extended_slots, uint64_t capacity_slots) {
			auto reg = region::create(dir.path, capacity_slots * bs, options());
			EXPECT_TRUE(reg.has_value()) << to_string(reg.error());
			EXPECT_TRUE(reg->extend(extended_slots * bs));
			return std::move(*reg);
		}
	};

	TEST_F(RegionSegmentTest, OnlyTheDirtySegmentsArePublished) {
		auto reg = make_wide_region(2 * seg + 1, 2 * seg + 8);
		bytes(reg)[0] = 'a';             // segment 0
		bytes(reg)[2 * seg * bs] = 'c';  // segment 2
		ASSERT_TRUE(reg.commit(true));
		auto const first = manifest_records(dir.path);
		ASSERT_EQ(first.size(), 3u);
		EXPECT_EQ(count_segment_files(dir.path), 2u);  // segment 1 is all_empty
		EXPECT_EQ(count_data_block_files(dir.path), 2u);

		bytes(reg)[0] = 'b';  // only the entries of segment 0 change
		ASSERT_TRUE(reg.commit(true));
		auto const second = manifest_records(dir.path);
		ASSERT_EQ(second.size(), 3u);
		EXPECT_NE(second[0], first[0]);
		EXPECT_EQ(second[1], first[1]);
		EXPECT_EQ(second[2], first[2]);
		EXPECT_EQ(count_segment_files(dir.path), 2u);
	}

	TEST_F(RegionSegmentTest, ExtendGrowsTheManifestWithoutFiles) {
		auto reg = make_wide_region(1, seg + 8);
		bytes(reg)[0] = 'a';
		ASSERT_TRUE(reg.commit(true));
		ASSERT_EQ(manifest_records(dir.path).size(), 1u);
		ASSERT_EQ(count_segment_files(dir.path), 1u);

		ASSERT_TRUE(reg.extend((seg + 1) * bs));
		ASSERT_TRUE(reg.commit(true));
		// The manifest carries the new segment, which is all_empty and has no
		// file. Segment 0 grew its entry count, so it republished and the
		// reclaim took its old file.
		EXPECT_EQ(manifest_records(dir.path).size(), 2u);
		EXPECT_EQ(count_segment_files(dir.path), 1u);
		EXPECT_EQ(count_data_block_files(dir.path), 1u);
	}

	// The missed-mark detector at scale: rounds of writes, frees, growth,
	// failed commits and cleaner batches across a segment boundary, against a
	// shadow of what every slot should hold. A dirty flag that is never set
	// shows up here as a slot whose reopened content is stale; in a test-hook
	// build the commit's own segment audit aborts before that.
	TEST_F(RegionSegmentTest, RandomizedRoundsSurviveAReopenIntact) {
		uint64_t const capacity_slots = seg + 64;
		uint64_t slots = seg - 5;  // the growth below crosses the segment boundary
		std::vector<unsigned char> shadow(capacity_slots, 0);
		{
			auto reg = make_wide_region(slots, capacity_slots);
			// the slots the rounds touch, a handful on each side of the boundary
			std::vector<uint64_t> const touched = {0,       1,       2,       seg - 4, seg - 3,
												   seg - 2, seg - 1, seg,     seg + 1};
			std::mt19937 rng{20260803};
			for (int round = 0; round < 30; ++round) {
				auto const value = static_cast<unsigned char>('A' + round % 26);
				for (int write = 0; write < 4; ++write) {
					uint64_t const slot = touched[rng() % touched.size()];
					if (slot >= slots) {
						continue;
					}
					bytes(reg)[slot * bs] = value;
					shadow[slot] = value;
				}
				uint64_t const first = touched[rng() % touched.size()];
				uint64_t const freed = 1 + rng() % 2;
				if (first < slots) {
					ASSERT_TRUE(reg.free_region(first * bs, freed * bs));
					for (uint64_t slot = first; slot < std::min(first + freed, slots); ++slot) {
						shadow[slot] = 0;
					}
				}
				if (slots < capacity_slots) {
					++slots;
					ASSERT_TRUE(reg.extend(slots * bs));
				}
				if (round % 3 == 0) {
					// A commit whose worker post fails: every captured slot goes
					// back to dirty and keeps its mark, so the commit below
					// persists it.
					detail_region::commit_post_fails_fn = [](size_t) { return true; };
					(void) reg.commit(true);
					detail_region::commit_post_fails_fn = nullptr;
				}
				if (round % 4 == 0) {
					(void) detail_region::run_cleaner_batch(reg, true);
				}
				ASSERT_TRUE(reg.commit(round % 2 == 0));
			}
			ASSERT_TRUE(reg.commit(true));
		}

		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		ASSERT_EQ(reopened->size(), slots * bs);
		size_t mismatches = 0;
		uint64_t first_bad = 0;
		for (uint64_t slot = 0; slot < slots; ++slot) {
			if (bytes(*reopened)[slot * bs] != shadow[slot]) {
				if (mismatches == 0) {
					first_bad = slot;
				}
				++mismatches;
			}
		}
		EXPECT_EQ(mismatches, 0u) << "first mismatching slot " << first_bad;
	}

	TEST_F(RegionCommitTest, ASimulatedEmptiedSlotCommitsTheSentinel) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		// stand-in for free_region: the slot resolves to dirty_empty
		auto &table = detail_region::table_of(*reg);
		ASSERT_TRUE(table.try_claim(0, slot_state::clean, slot_state::freeing));
		table.publish(0, slot_state::dirty_empty);

		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(table.load(0), slot_state::empty);
		EXPECT_EQ(count_data_block_files(dir.path), 0u);  // the emptied slot retired its name
	}

	TEST_F(RegionCommitTest, ACommitRecoversAPoisonedSlot) {
		{
			auto reg = make_region(1);
			auto &table = detail_region::table_of(reg);
			g_protect_fails.store(1);  // the handler's change fails once; the retry heals
			detail_region::mprotect_fn = failing_protect;

			std::atomic<bool> done{false};
			std::thread writer{[&] {
				(void) arm_thread_fault_stack();
				bytes(reg)[0] = 'w';  // poisons the slot and parks in the handler
				done.store(true, std::memory_order_release);
			}};
			ASSERT_TRUE(eventually([&] { return table.load(0) == slot_state::poisoned; }));

			// capture retries the protection change under the commit mutex,
			// heals the slot, and captures it; the parked store lands
			ASSERT_TRUE(reg.commit(true));
			writer.join();
			EXPECT_TRUE(done.load());
			EXPECT_TRUE(reg.check_sanity());
			EXPECT_EQ(detail_region::poisoned_slots(reg), 0u);
			EXPECT_EQ(bytes(reg)[0], 'w');

			// the store lands on either side of the capture's freeze; this
			// commit persists it whichever side it took
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'w');
	}

	TEST_F(RegionCommitTest, ACommitOverAnUnrecoveredPoisonedSlotKeepsTheEntry) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			auto &table = detail_region::table_of(*reg);
			bytes(*reg)[bs] = 'y';  // slot 1 dirties before the seam engages

			g_protect_fails.store(-1);  // every upgrade fails: recovery cannot heal yet
			detail_region::mprotect_fn = failing_protect;
			std::atomic<bool> done{false};
			std::thread writer{[&] {
				(void) arm_thread_fault_stack();
				bytes(*reg)[0] = 'w';  // poisons slot 0 and parks in the handler
				done.store(true, std::memory_order_release);
			}};
			ASSERT_TRUE(eventually([&] { return table.load(0) == slot_state::poisoned; }));

			// The slot's content is unchanged since its entry named it, so
			// capture keeps the entry and the commit stays valid without the
			// slot. No terminal flag: the slot is still recoverable.
			ASSERT_TRUE(reg->commit(true));
			EXPECT_TRUE(reg->check_sanity());
			EXPECT_EQ(table.load(0), slot_state::poisoned);

			// heal so the writer lands, then discard its uncommitted write
			g_protect_fails.store(0);
			(void) detail_region::run_cleaner_batch(*reg, false);
			writer.join();
			EXPECT_TRUE(done.load());
			EXPECT_EQ(bytes(*reg)[0], 'w');
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');  // the kept entry: the pre-poisoning content
		EXPECT_EQ(bytes(*reopened)[bs], 'y');  // the rest of the commit landed
	}

	TEST_F(RegionCommitTest, RewritesAfterACommitPersistAgain) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'x';
		ASSERT_TRUE(reg.commit(true));
		bytes(reg)[0] = 'y';  // re-dirties the now clean slot
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(bytes(reg)[0], 'y');
		EXPECT_EQ(count_data_block_files(dir.path), 1u);
	}

	// The downstream write pattern: the writer quiesces around every commit
	// (Tentris serializes its writer around checkpoints), so each commit is
	// a consistent cut and every cut persists what the writer wrote.
	TEST_F(RegionCommitTest, AQuiescedWriterSeesEveryCutPersist) {
		auto reg = make_region(2);
		constexpr int rounds = 10;
		std::atomic<int> phase{0};  // even: the writer's turn, odd: the committer's
		std::thread writer{[&] {
			for (int round = 0; round < rounds; ++round) {
				while (phase.load(std::memory_order_acquire) != 2 * round) {
				}
				bytes(reg)[0] = static_cast<unsigned char>(round + 1);
				bytes(reg)[bs + static_cast<uint64_t>(round)] = static_cast<unsigned char>(round + 1);
				phase.store(2 * round + 1, std::memory_order_release);
			}
		}};
		for (int round = 0; round < rounds; ++round) {
			while (phase.load(std::memory_order_acquire) != 2 * round + 1) {
			}
			ASSERT_TRUE(reg.commit(round % 2 == 0));
			phase.store(2 * round + 2, std::memory_order_release);
		}
		writer.join();
		ASSERT_TRUE(reg.commit(true));

		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], rounds);
		for (int round = 0; round < rounds; ++round) {
			EXPECT_EQ(bytes(*reopened)[bs + static_cast<uint64_t>(round)], round + 1);
		}
	}

	// A failed durability barrier drops the block's dirty pages and reports
	// the error only to the file the sync ran on. A retry through a fresh
	// descriptor over the same file therefore proves nothing about the
	// device: the content must be written again, through a fresh file.
	TEST_F(RegionCommitTest, ARetriedDurableCommitRewritesWhatTheBarrierFailedOn) {
		auto reg = make_region(2);
		bytes(reg)[0] = 'x';
		bytes(reg)[bs] = 'y';

		arm_barrier_seam();
		g_barrier_fails.store(true);
		auto const failed = reg.commit(true);
		ASSERT_FALSE(failed.has_value());
		EXPECT_EQ(failed.error().code, errc::io_error);
		g_barrier_fails.store(false);

		// the files the failed barrier left behind: one per slot, plus the
		// segment file of the recipe the commit did not publish
		auto const before = store_inodes(dir.path);
		ASSERT_EQ(before.size(), 3u);

		ASSERT_TRUE(reg.commit(true));
		auto const after = store_inodes(dir.path);
		ASSERT_EQ(after.size(), before.size());
		for (auto const &[name, inode] : before) {
			ASSERT_TRUE(after.contains(name)) << name;
			EXPECT_NE(after.at(name), inode) << "name " << name << " was not written again";
		}
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 0u);
		EXPECT_TRUE(reg.check_sanity());
	}

	// The slot the barrier failed on goes back to dirty, so the content it
	// carries reaches the retry's write-out even though nothing changed it.
	TEST_F(RegionCommitTest, ARetryRewritesEvenWhenTheContentAlreadyCarriesItsName) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'x';
		ASSERT_TRUE(reg.commit(false));  // published, no barrier owed yet
		bytes(reg)[0] = 'x';             // dirty again, same content, same name

		arm_barrier_seam();
		g_barrier_fails.store(true);
		ASSERT_FALSE(reg.commit(true).has_value());
		g_barrier_fails.store(false);
		auto const before = store_inodes(dir.path);

		ASSERT_TRUE(reg.commit(true));
		auto const after = store_inodes(dir.path);
		for (auto const &[name, inode] : before) {
			ASSERT_TRUE(after.contains(name)) << name;
			EXPECT_NE(after.at(name), inode) << "name " << name << " was not written again";
		}
	}

	TEST_F(RegionCommitTest, AFailedBarrierKeepsTheCommittedRecipeAndTheRetryPublishesIt) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		auto const committed = privateer::testing::manifest_records(dir.path);
		{
			auto reg = region::open(dir.path, options());
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			bytes(*reg)[bs] = 'c';

			arm_barrier_seam();
			g_barrier_fails.store(true);
			ASSERT_FALSE(reg->commit(true).has_value());
			g_barrier_fails.store(false);
			// nothing was renamed, so the datastore still describes 'a' and 'b'
			EXPECT_EQ(privateer::testing::manifest_records(dir.path), committed);

			ASSERT_TRUE(reg->commit(true));
			EXPECT_NE(privateer::testing::manifest_records(dir.path), committed);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');
		EXPECT_EQ(bytes(*reopened)[bs], 'c');
		// the reopen sweep took the block the failed barrier left unreferenced
		EXPECT_EQ(count_data_block_files(dir.path), 2u);
	}

	// The unsynchronized interleaving: a writer keeps dirtying slots while
	// commits run. Every commit is a cut of what the barrier captured (a cut
	// may tear mid-store, the documented consequence of writing concurrently
	// with sync), committed state is never corrupted, and the final commit
	// persists the final content. The racing plain stores are exactly what
	// TSan reports, so the test does not run there.
	TEST_F(RegionCommitTest, AnUnsynchronizedWriterNeverBreaksACommit) {
#ifdef PRIVATEER_TEST_TSAN
		GTEST_SKIP() << "the writer races the capture on purpose; the tear is the documented precondition";
#endif
		auto reg = make_region(4);
		std::atomic<bool> stop{false};
		std::thread writer{[&] {
			unsigned char value = 0;
			while (!stop.load(std::memory_order_acquire)) {
				for (uint64_t slot = 0; slot < 4; ++slot) {
					bytes(reg)[slot * bs + (value % bs)] = value;
				}
				++value;
			}
		}};
		for (int i = 0; i < 20; ++i) {
			ASSERT_TRUE(reg.commit(i % 2 == 0));
		}
		stop.store(true, std::memory_order_release);
		writer.join();
		ASSERT_TRUE(reg.commit(true));

		std::vector<unsigned char> final_content(4 * bs);
		for (size_t i = 0; i < final_content.size(); ++i) {
			final_content[i] = bytes(reg)[i];
		}
		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		for (size_t i = 0; i < final_content.size(); ++i) {
			ASSERT_EQ(bytes(*reopened)[i], final_content[i]) << "at offset " << i;
		}
	}

}  // namespace
