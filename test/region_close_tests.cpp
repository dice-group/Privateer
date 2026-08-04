// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Close against callers that are still inside the region: a writer parked on
// a slot claim nothing releases, and the API calls that work with the state
// close tears down.

#include <gtest/gtest.h>

#include <privateer/error.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/region_registry.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>

#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

using namespace privateer;
using namespace std::chrono_literals;

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

	struct RegionCloseTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	// A claim its owner never released stays on the slot for the region's
	// lifetime. A writer that faults such a slot waits on the slot's own
	// state word, and close changes no state: it wakes those waiters, and
	// the wait is timed, so the fault ends and close drains the handler
	// reference instead of waiting for a publish that never comes.
	TEST_F(RegionCloseTest, CloseReleasesAWriterParkedOnAStrandedClaim) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		auto const addr = reinterpret_cast<uintptr_t>(reg->segment());
		ASSERT_TRUE(table.try_claim(0, slot_state::clean, slot_state::syncing));

		// The fault path with the registry reference the process-wide
		// handler holds around it: that reference is what close drains
		// before it tears the region down.
		std::atomic<bool> entered{false};
		std::atomic<int> handled{-1};
		std::thread writer{[&] {
			auto *const rec =
					global_registry().acquire(addr, region_registry::in_flight_kind::handler);
			ASSERT_NE(rec, nullptr);
			entered.store(true, std::memory_order_release);
			bool const took = rec->on_fault(*rec, addr, SIGSEGV);
			region_registry::release(*rec, region_registry::in_flight_kind::handler);
			handled.store(took ? 1 : 0, std::memory_order_release);
		}};
		ASSERT_TRUE(eventually([&] { return entered.load(std::memory_order_acquire); }));
		std::this_thread::sleep_for(50ms);
		ASSERT_EQ(handled.load(std::memory_order_acquire), -1) << "the writer left the fault path";

		std::atomic<bool> closed{false};
		std::thread closer{[&] {
			{
				auto closing = std::move(*reg);
			}
			closed.store(true, std::memory_order_release);
		}};
		bool const done = eventually([&] { return closed.load(std::memory_order_acquire); }, 5s);
		EXPECT_TRUE(done) << "close did not finish with a claim stranded";
		if (!done) {
			table.publish(0, slot_state::clean);  // let the run end
		}
		closer.join();
		writer.join();
		// The fault forwards. A writer parked when close begins has not
		// quiesced, and the process-wide handler turns the forwarded fault
		// into the crash that says so.
		EXPECT_EQ(handled.load(std::memory_order_acquire), 0);
	}

}  // namespace
