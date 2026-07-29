// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <thread>

#include <sys/mman.h>

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
using privateer::testing::count_block_files;

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

	struct RegionFreeTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		void TearDown() override {
			detail_region::mprotect_fn = ::mprotect;
			g_protect_fails.store(0);
		}

		// a region over a committed store holding 'a' and 'b'
		region open_ab() {
			privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
			auto reg = region::open(dir.path);
			EXPECT_TRUE(reg.has_value()) << to_string(reg.error());
			return std::move(*reg);
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	TEST_F(RegionFreeTest, FreedSlotsReadZeros) {
		auto reg = open_ab();
		ASSERT_TRUE(reg.free_region(0, bs));
		EXPECT_EQ(bytes(reg)[0], 0);
		EXPECT_EQ(bytes(reg)[bs - 1], 0);
		EXPECT_EQ(bytes(reg)[bs], 'b');
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::dirty_empty);
		EXPECT_EQ(detail_region::table_of(reg).load(1), slot_state::clean);
	}

	TEST_F(RegionFreeTest, FreeingADirtySlotBalancesTheCount) {
		auto reg = open_ab();
		bytes(reg)[0] = 'x';
		ASSERT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		ASSERT_TRUE(reg.free_region(0, bs));
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 0u);
		EXPECT_EQ(bytes(reg)[0], 0);
	}

	TEST_F(RegionFreeTest, SubSlotFreesChangeNothing) {
		auto reg = open_ab();
		ASSERT_TRUE(reg.free_region(bs / 2, bs / 2));  // no fully covered slot
		EXPECT_EQ(bytes(reg)[bs / 2], 'a');
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::clean);

		ASSERT_TRUE(reg.free_region(bs / 2, bs));  // spans two slots, covers neither fully
		EXPECT_EQ(bytes(reg)[0], 'a');
		EXPECT_EQ(bytes(reg)[bs], 'b');
	}

	TEST_F(RegionFreeTest, APartiallyCoveringRangeFreesOnlyTheFullSlots) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b', 'c'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		ASSERT_TRUE(reg->free_region(bs / 2, 2 * bs));  // fully covers slot 1 only
		EXPECT_EQ(bytes(*reg)[0], 'a');
		EXPECT_EQ(bytes(*reg)[bs], 0);
		EXPECT_EQ(bytes(*reg)[2 * bs], 'c');
	}

	TEST_F(RegionFreeTest, TheFreedNameIsReclaimedAtTheNextDurableCommit) {
		auto reg = open_ab();
		ASSERT_EQ(count_block_files(dir.path), 2u);
		ASSERT_TRUE(reg.free_region(0, bs));
		ASSERT_EQ(count_block_files(dir.path), 2u);  // nothing reclaimed before the commit
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(count_block_files(dir.path), 1u);
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::empty);
	}

	TEST_F(RegionFreeTest, TheFreedStatePersists) {
		{
			auto reg = open_ab();
			ASSERT_TRUE(reg.free_region(0, bs));
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 0);
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

	TEST_F(RegionFreeTest, FreeingAFreshEmptySlotStaysZero) {
		auto reg = region::create(dir.path, 4 * bs, [&] {
			region_options opts;
			opts.block_size = bs;
			return opts;
		}());
		ASSERT_TRUE(reg.has_value());
		ASSERT_TRUE(reg->extend(bs));
		ASSERT_TRUE(reg->free_region(0, bs));
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::dirty_empty);
		EXPECT_EQ(bytes(*reg)[0], 0);
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::empty);
	}

	TEST_F(RegionFreeTest, AWriteAfterAFreeMaterializesFreshZeros) {
		auto reg = open_ab();
		ASSERT_TRUE(reg.free_region(0, bs));
		bytes(reg)[5] = 'x';
		EXPECT_EQ(bytes(reg)[0], 0);  // the old 'a' bytes are gone
		EXPECT_EQ(bytes(reg)[5], 'x');
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::dirty);
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);

		ASSERT_TRUE(reg.commit(true));
		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 0);
		EXPECT_EQ(bytes(*reopened)[5], 'x');
	}

	TEST_F(RegionFreeTest, FreeOnAReadOnlyRegionFails) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open_read_only(dir.path);
		ASSERT_TRUE(reg.has_value());
		auto freed = reg->free_region(0, bs);
		ASSERT_FALSE(freed.has_value());
		EXPECT_EQ(freed.error().code, errc::invalid_argument);
		EXPECT_EQ(bytes(*reg)[0], 'a');
	}

	TEST_F(RegionFreeTest, FreeBeyondTheExtendedSizeIsANoOp) {
		auto reg = open_ab();
		ASSERT_TRUE(reg.free_region(2 * bs, bs));   // at the size boundary
		ASSERT_TRUE(reg.free_region(bs, 100 * bs));  // clamped to the size
		EXPECT_EQ(bytes(reg)[0], 'a');
		EXPECT_EQ(bytes(reg)[bs], 0);  // slot 1 was inside the clamped range
	}

	TEST_F(RegionFreeTest, FreeWaitsOutATransient) {
		auto reg = open_ab();
		auto &table = detail_region::table_of(reg);
		ASSERT_TRUE(table.try_claim(0, slot_state::clean, slot_state::syncing));

		std::atomic<bool> done{false};
		std::thread freer{[&] {
			EXPECT_TRUE(reg.free_region(0, bs));
			done.store(true, std::memory_order_release);
		}};
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		EXPECT_FALSE(done.load(std::memory_order_acquire));  // parked on the transient

		table.publish(0, slot_state::clean);
		freer.join();
		EXPECT_TRUE(done.load());
		EXPECT_EQ(table.load(0), slot_state::dirty_empty);
		EXPECT_EQ(bytes(reg)[0], 0);
	}

	TEST_F(RegionFreeTest, AFreeHealsAPoisonedSlot) {
		auto reg = open_ab();
		auto &table = detail_region::table_of(reg);
		g_protect_fails.store(1);  // the handler's change fails once
		detail_region::mprotect_fn = failing_protect;

		std::atomic<bool> done{false};
		std::thread writer{[&] {
			(void) arm_thread_fault_stack();
			bytes(reg)[0] = 'w';  // poisons slot 0 and parks in the handler
			done.store(true, std::memory_order_release);
		}};
		ASSERT_TRUE(eventually([&] { return table.load(0) == slot_state::poisoned; }));

		// The free claims the poisoned slot directly: the remap replaces the
		// mapping wholesale and does not need the protection change that
		// failed. The woken writer rematerializes the fresh zeros.
		ASSERT_TRUE(reg.free_region(0, bs));
		writer.join();

		EXPECT_TRUE(done.load());
		EXPECT_EQ(bytes(reg)[0], 'w');
		EXPECT_EQ(bytes(reg)[1], 0);  // the pre-free content was discarded
		EXPECT_EQ(bytes(reg)[bs], 'b');
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 1u);
		EXPECT_EQ(detail_region::poisoned_slots(reg), 0u);
		EXPECT_TRUE(reg.check_sanity());
	}

	TEST_F(RegionFreeTest, ACaptureSkipsAFreeingSlotAndKeepsThePreFreeName) {
		auto reg = open_ab();
		auto &table = detail_region::table_of(reg);
		// hold slot 0 mid-free across a whole commit
		ASSERT_TRUE(table.try_claim(0, slot_state::clean, slot_state::freeing));
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(count_block_files(dir.path), 2u);  // the pre-free name stays referenced

		// the free finishes; the next epoch retires the name
		table.publish(0, slot_state::dirty_empty);
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(count_block_files(dir.path), 1u);
		EXPECT_EQ(table.load(0), slot_state::empty);
	}

	// The GC pattern: the freer quiesces against the writer per round (the
	// engine tolerates the unsynchronized race too, but TSan cannot see the
	// remap ordering, so the racing variant runs on the other legs).
	TEST_F(RegionFreeTest, AlternatingWritesAndFreesConverge) {
		auto reg = open_ab();
		constexpr int rounds = 50;
		std::atomic<int> phase{0};
		std::thread writer{[&] {
			for (int round = 0; round < rounds; ++round) {
				while (phase.load(std::memory_order_acquire) != 2 * round) {
				}
				bytes(reg)[0] = static_cast<unsigned char>(round + 1);
				phase.store(2 * round + 1, std::memory_order_release);
			}
		}};
		for (int round = 0; round < rounds; ++round) {
			while (phase.load(std::memory_order_acquire) != 2 * round + 1) {
			}
			ASSERT_TRUE(reg.free_region(0, bs));
			phase.store(2 * round + 2, std::memory_order_release);
		}
		writer.join();
		EXPECT_EQ(bytes(reg)[0], 0);  // the last action was a free
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(count_block_files(dir.path), 1u);  // only 'b' remains referenced
	}

	TEST_F(RegionFreeTest, AnUnsynchronizedWriterRacingTheFreerIsSafe) {
#ifdef PRIVATEER_TEST_TSAN
		GTEST_SKIP() << "the writer races the remap on purpose; TSan cannot see the mmap ordering";
#endif
		auto reg = open_ab();
		std::atomic<bool> stop{false};
		std::thread writer{[&] {
			unsigned char value = 1;
			while (!stop.load(std::memory_order_acquire)) {
				bytes(reg)[0] = value++;
			}
		}};
		for (int i = 0; i < 200; ++i) {
			ASSERT_TRUE(reg.free_region(0, bs));
		}
		stop.store(true, std::memory_order_release);
		writer.join();

		// the slot is either freshly freed or re-materialized by the writer
		auto const state = detail_region::table_of(reg).load(0);
		EXPECT_TRUE(state == slot_state::dirty_empty || state == slot_state::dirty) << to_string(state);
		ASSERT_TRUE(reg.commit(true));
		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[bs], 'b');  // the untouched slot is intact
	}

}  // namespace
