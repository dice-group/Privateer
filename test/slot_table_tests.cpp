#include <gtest/gtest.h>

#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>
#include <privateer/word_wait.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace privateer;

namespace {

	slot_table make_table(size_t slots) {
		auto table = slot_table::create(slots);
		EXPECT_TRUE(table.has_value()) << to_string(table.error());
		return std::move(*table);
	}

	TEST(SlotTableTest, CreateInitializesEverySlotEmpty) {
		auto table = make_table(64);
		EXPECT_EQ(table.slot_count(), 64u);
		EXPECT_EQ(table.dirty_slots(), 0u);
		for (size_t i = 0; i < table.slot_count(); ++i) {
			EXPECT_EQ(table.load(i), slot_state::empty);
		}
	}

	TEST(SlotTableTest, CreateRejectsZeroSlots) {
		auto table = slot_table::create(0);
		ASSERT_FALSE(table.has_value());
		EXPECT_EQ(table.error().code, errc::invalid_argument);
	}

	TEST(SlotTableTest, UnlockedAllocationWorks) {
		auto table = slot_table::create(8, false);
		ASSERT_TRUE(table.has_value()) << to_string(table.error());
		EXPECT_EQ(table->load(0), slot_state::empty);
		EXPECT_FALSE(table->locking());
		EXPECT_EQ(table->locked_bytes(), 0u);
		EXPECT_TRUE(table->lock_to(8));  // a no-op with locking disabled
		EXPECT_EQ(table->locked_bytes(), 0u);
	}

	TEST(SlotTableTest, LocksThePagesTheUsedSlotsTouch) {
		size_t const slots_per_page = privateer::page_size() / sizeof(uint32_t);
		auto table = make_table(4 * slots_per_page);
		EXPECT_TRUE(table.locking());
		// create locks the header page only
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(0));
		EXPECT_LT(table.locked_bytes(), slot_table::locked_bytes_for(4 * slots_per_page));

		ASSERT_TRUE(table.lock_to(slots_per_page + 1));
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(slots_per_page + 1));

		// the locked prefix never shrinks
		ASSERT_TRUE(table.lock_to(1));
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(slots_per_page + 1));

		// a request beyond the slot count clamps to the whole array
		ASSERT_TRUE(table.lock_to(100 * slots_per_page));
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(4 * slots_per_page));
	}

	TEST(SlotTableTest, LockedBytesForIsPageGranularAndMonotone) {
		size_t const page = privateer::page_size();
		EXPECT_EQ(slot_table::locked_bytes_for(0), page);
		EXPECT_EQ(slot_table::locked_bytes_for(0) % page, 0u);
		EXPECT_LE(slot_table::locked_bytes_for(1), slot_table::locked_bytes_for(page));
	}

	TEST(SlotTableTest, StateNamesAreStable) {
		EXPECT_STREQ(to_string(slot_state::empty), "empty");
		EXPECT_STREQ(to_string(slot_state::clean), "clean");
		EXPECT_STREQ(to_string(slot_state::dirty), "dirty");
		EXPECT_STREQ(to_string(slot_state::dirty_empty), "dirty_empty");
		EXPECT_STREQ(to_string(slot_state::poisoned), "poisoned");
		EXPECT_STREQ(to_string(slot_state::materializing), "materializing");
		EXPECT_STREQ(to_string(slot_state::syncing), "syncing");
		EXPECT_STREQ(to_string(slot_state::freeing), "freeing");
	}

	TEST(SlotTableTest, TransientClassification) {
		EXPECT_TRUE(is_transient(slot_state::materializing));
		EXPECT_TRUE(is_transient(slot_state::syncing));
		EXPECT_TRUE(is_transient(slot_state::freeing));
		EXPECT_FALSE(is_transient(slot_state::empty));
		EXPECT_FALSE(is_transient(slot_state::clean));
		EXPECT_FALSE(is_transient(slot_state::dirty));
		EXPECT_FALSE(is_transient(slot_state::dirty_empty));
		EXPECT_FALSE(is_transient(slot_state::poisoned));
	}

	TEST(SlotTableTest, ClaimWinsOnlyFromTheExpectedState) {
		auto table = make_table(4);
		EXPECT_TRUE(table.try_claim(0, slot_state::empty, slot_state::materializing));
		EXPECT_EQ(table.load(0), slot_state::materializing);
		// the slot is claimed; a second claim from any expected state loses
		EXPECT_FALSE(table.try_claim(0, slot_state::empty, slot_state::materializing));
		EXPECT_FALSE(table.try_claim(0, slot_state::dirty, slot_state::syncing));
		EXPECT_EQ(table.load(0), slot_state::materializing);
	}

	TEST(SlotTableTest, PublishStoresTheTerminalState) {
		auto table = make_table(4);
		ASSERT_TRUE(table.try_claim(2, slot_state::empty, slot_state::materializing));
		table.publish(2, slot_state::dirty);
		EXPECT_EQ(table.load(2), slot_state::dirty);
		ASSERT_TRUE(table.try_claim(2, slot_state::dirty, slot_state::syncing));
		table.publish(2, slot_state::clean);
		EXPECT_EQ(table.load(2), slot_state::clean);
	}

	TEST(SlotTableTest, WaitChangedParksUntilPublish) {
		auto table = make_table(4);
		ASSERT_TRUE(table.try_claim(1, slot_state::empty, slot_state::materializing));
		slot_state seen = slot_state::materializing;
		std::thread waiter{[&] { seen = table.wait_changed(1, slot_state::materializing); }};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		table.publish(1, slot_state::dirty);
		waiter.join();
		EXPECT_EQ(seen, slot_state::dirty);
	}

	TEST(SlotTableTest, WaitChangedReturnsImmediatelyOnAStaleObservation) {
		auto table = make_table(4);
		ASSERT_TRUE(table.try_claim(1, slot_state::empty, slot_state::freeing));
		EXPECT_EQ(table.wait_changed(1, slot_state::empty), slot_state::freeing);
	}

	TEST(SlotTableTest, TimedWaitReturnsObservedOnTimeout) {
		auto table = make_table(4);
		// nothing publishes: the deadline passes with the word unchanged
		EXPECT_EQ(table.wait_changed_for(0, slot_state::empty, 5'000'000), slot_state::empty);
	}

	TEST(SlotTableTest, TimedWaitReturnsOnPublish) {
		auto table = make_table(4);
		ASSERT_TRUE(table.try_claim(1, slot_state::empty, slot_state::syncing));
		slot_state seen = slot_state::syncing;
		std::thread waiter{[&] {
			// a minute, but the publish below releases the wait promptly
			seen = table.wait_changed_for(1, slot_state::syncing, 60'000'000'000);
		}};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		table.publish(1, slot_state::poisoned);
		waiter.join();
		EXPECT_EQ(seen, slot_state::poisoned);
	}

	TEST(SlotTableTest, ClaimStormHasExactlyOneWinner) {
		auto table = make_table(1);
		constexpr int threads = 8;
		std::atomic<int> winners{0};
		std::atomic<bool> go{false};
		std::vector<std::thread> storm;
		for (int i = 0; i < threads; ++i) {
			storm.emplace_back([&] {
				while (!go.load(std::memory_order_acquire)) {
				}
				if (table.try_claim(0, slot_state::empty, slot_state::materializing)) {
					winners.fetch_add(1);
				}
			});
		}
		go.store(true, std::memory_order_release);
		for (auto &thread : storm) {
			thread.join();
		}
		EXPECT_EQ(winners.load(), 1);
		EXPECT_EQ(table.load(0), slot_state::materializing);
	}

	// The handler's first-touch loop over one slot: the claim winner
	// materializes and publishes dirty; losers wait out the transient and
	// re-examine. Every thread must end on a dirty slot with one count.
	TEST(SlotTableTest, FirstTouchStormEndsDirtyWithOneCount) {
		auto table = make_table(1);
		constexpr int threads = 8;
		std::atomic<bool> go{false};
		std::vector<std::thread> storm;
		for (int i = 0; i < threads; ++i) {
			storm.emplace_back([&] {
				while (!go.load(std::memory_order_acquire)) {
				}
				for (;;) {
					slot_state const state = table.load(0);
					if (state == slot_state::dirty) {
						return;
					}
					if (is_transient(state)) {
						(void) table.wait_changed(0, state);
						continue;
					}
					if (table.try_claim(0, slot_state::empty, slot_state::materializing)) {
						table.add_dirty();
						// the protection change would happen here
						table.publish(0, slot_state::dirty);
						return;
					}
				}
			});
		}
		go.store(true, std::memory_order_release);
		for (auto &thread : storm) {
			thread.join();
		}
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 1u);
	}

	TEST(SlotTableTest, DirtyAccountingBalances) {
		auto table = make_table(4);
		table.add_dirty();
		table.add_dirty();
		EXPECT_EQ(table.dirty_slots(), 2u);
		table.sub_dirty();
		EXPECT_EQ(table.dirty_slots(), 1u);
		table.sub_dirty();
		EXPECT_EQ(table.dirty_slots(), 0u);
	}

	TEST(SlotTableTest, EveryDecreaseBumpsTheGovernorWord) {
		auto table = make_table(4);
		table.add_dirty();
		table.add_dirty();
		uint32_t const before = table.governor_word().load(std::memory_order_acquire);
		table.sub_dirty();
		table.sub_dirty();
		EXPECT_EQ(table.governor_word().load(std::memory_order_acquire), before + 2);
	}

	TEST(SlotTableTest, DirtyDecreaseWakesTheGovernorWaiter) {
		auto table = make_table(4);
		table.add_dirty();
		uint32_t const observed = table.governor_word().load(std::memory_order_acquire);
		uint32_t seen = observed;
		std::thread waiter{[&] { seen = word_wait(table.governor_word(), observed); }};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		table.sub_dirty();
		waiter.join();
		EXPECT_EQ(seen, observed + 1);
	}

}  // namespace
