// Unit tests for the region registry: lookup hit and miss, overlap
// rejection, the in-flight counters, and remove draining a held lookup.

#include <gtest/gtest.h>

#include <privateer/region_registry.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace privateer;
using in_flight = region_registry::in_flight_kind;

namespace {

	TEST(RegionRegistry, LookupHitsInsideAndMissesOutside) {
		region_registry reg;
		region_record a;
		region_record b;
		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));
		ASSERT_TRUE(reg.add(b, 0x30000, 0x40000));

		EXPECT_EQ(reg.acquire(0x0FFFF, in_flight::handler), nullptr);
		EXPECT_EQ(reg.acquire(0x20000, in_flight::handler), nullptr);
		EXPECT_EQ(reg.acquire(0x2FFFF, in_flight::handler), nullptr);
		EXPECT_EQ(reg.acquire(0x40000, in_flight::handler), nullptr);

		auto *hit = reg.acquire(0x10000, in_flight::handler);
		ASSERT_EQ(hit, &a);
		EXPECT_EQ(a.handler_in_flight.load(), 1u);
		region_registry::release(*hit, in_flight::handler);
		EXPECT_EQ(a.handler_in_flight.load(), 0u);

		hit = reg.acquire(0x3FFFF, in_flight::free_region);
		ASSERT_EQ(hit, &b);
		EXPECT_EQ(b.free_in_flight.load(), 1u);
		EXPECT_EQ(b.handler_in_flight.load(), 0u);
		region_registry::release(*hit, in_flight::free_region);
		EXPECT_EQ(b.free_in_flight.load(), 0u);
	}

	TEST(RegionRegistry, EmptyRegistryMissesEverything) {
		region_registry reg;
		EXPECT_EQ(reg.acquire(0, in_flight::handler), nullptr);
		EXPECT_EQ(reg.acquire(UINTPTR_MAX, in_flight::handler), nullptr);
	}

	TEST(RegionRegistry, RejectsEmptyAndOverlappingRanges) {
		region_registry reg;
		region_record a;
		region_record b;

		auto empty = reg.add(a, 0x1000, 0x1000);
		ASSERT_FALSE(empty.has_value());
		EXPECT_EQ(empty.error().code, errc::invalid_argument);

		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));
		std::pair<uintptr_t, uintptr_t> const overlapping_ranges[] = {
				{0x10000, 0x20000}, {0x08000, 0x10001}, {0x1FFFF, 0x30000}, {0x12000, 0x13000}, {0x08000, 0x30000}};
		for (auto const &[start, end] : overlapping_ranges) {
			auto overlapping = reg.add(b, start, end);
			ASSERT_FALSE(overlapping.has_value()) << start << " " << end;
			EXPECT_EQ(overlapping.error().code, errc::invalid_argument);
		}

		// touching ranges do not overlap
		EXPECT_TRUE(reg.add(b, 0x20000, 0x30000));
	}

	TEST(RegionRegistry, RemoveMakesLookupMiss) {
		region_registry reg;
		region_record a;
		region_record b;
		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));
		ASSERT_TRUE(reg.add(b, 0x30000, 0x40000));

		reg.remove(a);
		EXPECT_EQ(reg.acquire(0x18000, in_flight::handler), nullptr);

		auto *hit = reg.acquire(0x38000, in_flight::handler);
		ASSERT_EQ(hit, &b);
		region_registry::release(*hit, in_flight::handler);

		reg.remove(a);  // removing an absent record is a no-op
		reg.remove(b);
		EXPECT_EQ(reg.acquire(0x38000, in_flight::handler), nullptr);
	}

	TEST(RegionRegistry, ManyRegionsAreAllFound) {
		region_registry reg;
		constexpr size_t count = 64;
		std::vector<region_record> records(count);
		// registration order is descending, lookup must not depend on it
		for (size_t i = count; i-- > 0;) {
			uintptr_t const start = 0x100000 + i * 0x10000;
			ASSERT_TRUE(reg.add(records[i], start, start + 0x8000));
		}
		for (size_t i = 0; i < count; ++i) {
			uintptr_t const start = 0x100000 + i * 0x10000;
			auto *hit = reg.acquire(start + 0x7FFF, in_flight::handler);
			ASSERT_EQ(hit, &records[i]) << i;
			region_registry::release(*hit, in_flight::handler);
			EXPECT_EQ(reg.acquire(start + 0x8000, in_flight::handler), nullptr) << i;
		}
	}

	TEST(RegionRegistry, RemoveWaitsForAHeldLookup) {
		region_registry reg;
		region_record a;
		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));

		auto *held = reg.acquire(0x10000, in_flight::handler);
		ASSERT_EQ(held, &a);

		std::atomic<bool> removed{false};
		std::thread remover{[&] {
			reg.remove(a);
			removed.store(true, std::memory_order_release);
		}};

		// remove must not finish while the lookup holds the record
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
		EXPECT_FALSE(removed.load(std::memory_order_acquire));

		region_registry::release(*held, in_flight::handler);
		remover.join();
		EXPECT_TRUE(removed.load(std::memory_order_acquire));
	}

}  // namespace
