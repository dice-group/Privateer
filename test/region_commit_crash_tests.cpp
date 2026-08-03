// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Crash tests for the region commit: a child process opens the datastore,
// dirties a slot through the write barrier, and is killed at each commit
// phase boundary. Properties checked on the survivor side: the datastore
// reopens, its content matches the last recipe that reached the rename, the
// open-time sweep leaves exactly the referenced blocks, and the survivor
// can commit again.

#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <csignal>
#include <cstddef>

using namespace privateer;
using privateer::testing::count_data_block_files;
using privateer::testing::count_segment_files;
using privateer::testing::subprocess_result;

namespace {

	// which completed phase kills the child (1 capture, 2 write-out,
	// 3 barrier, 4 rename, 5 reclaim)
	int g_kill_after_phase = 0;

	// parameter: the commit worker count, covering the single-threaded and
	// the fanned-out write-out
	struct RegionCommitCrashTest : ::testing::TestWithParam<size_t> {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		// slot 0 holds 'a' and keeps it; slot 1 changes from 'b' to 'c' in
		// the killed commit
		void SetUp() override {
			privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		}

		void run_killed_at(int phase, bool durable) {
			auto const res = PRIVATEER_SANDBOX {
				detail_fault_handler::uninstall_for_tests();  // the sandbox cleared the dispositions
				g_kill_after_phase = phase;
				detail_region::commit_phase_hook = [](int done) {
					if (done == g_kill_after_phase) {
						::raise(SIGKILL);
					}
				};
				auto reg = region::open(dir.path, {.commit_workers = GetParam()});
				if (!reg) {
					return 10;
				}
				static_cast<unsigned char volatile *>(reg->segment())[bs] = 'c';
				(void) reg->commit(durable);
				return 11;  // the kill phase was never reached
			};
			ASSERT_EQ(res, subprocess_result::killed);
		}

		// the reopen properties after the child died
		void check_state(char slot1_content, size_t files_before_reopen, size_t files_after_sweep) {
			EXPECT_EQ(count_data_block_files(dir.path), files_before_reopen);
			auto reg = region::open(dir.path);  // read-write: sweeps the garbage
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			auto *const bytes = static_cast<unsigned char volatile *>(reg->segment());
			EXPECT_EQ(bytes[0], 'a');
			EXPECT_EQ(bytes[bs], static_cast<unsigned char>(slot1_content));
			EXPECT_EQ(count_data_block_files(dir.path), files_after_sweep);
			// two slots are one segment, and the sweep took the segment file of
			// every recipe the rename did not publish
			EXPECT_EQ(count_segment_files(dir.path), 1u);
			EXPECT_TRUE(reg->commit(true));  // the survivor commits cleanly
		}
	};

	TEST_P(RegionCommitCrashTest, DurableKilledAfterCaptureChangesNothing) {
		run_killed_at(1, true);
		check_state('b', 2, 2);
	}

	TEST_P(RegionCommitCrashTest, DurableKilledAfterWriteOutKeepsTheOldRecipe) {
		run_killed_at(2, true);
		check_state('b', 3, 2);  // the block holding 'c' is swept garbage
	}

	TEST_P(RegionCommitCrashTest, DurableKilledAfterTheBarrierKeepsTheOldRecipe) {
		run_killed_at(3, true);
		check_state('b', 3, 2);  // durable but unreferenced: still swept
	}

	TEST_P(RegionCommitCrashTest, DurableKilledAfterTheRenameShowsTheNewContent) {
		run_killed_at(4, true);
		check_state('c', 3, 2);  // the retired 'b' block is not yet reclaimed
	}

	TEST_P(RegionCommitCrashTest, DurableKilledAfterReclaimLeavesNoGarbage) {
		run_killed_at(5, true);
		check_state('c', 2, 2);
	}

	TEST_P(RegionCommitCrashTest, NonDurableKilledAfterWriteOutKeepsTheOldRecipe) {
		run_killed_at(2, false);
		check_state('b', 3, 2);
	}

	TEST_P(RegionCommitCrashTest, NonDurableKilledAfterTheRenameShowsTheNewContent) {
		// a non-durable commit never unlinks, so the retired 'b' block
		// survives as sweepable garbage
		run_killed_at(4, false);
		check_state('c', 3, 2);
	}

	INSTANTIATE_TEST_SUITE_P(CommitWorkers, RegionCommitCrashTest,
							 ::testing::Values(size_t{1}, size_t{4}));

}  // namespace
