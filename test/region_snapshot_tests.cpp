// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

#include <unistd.h>

using namespace privateer;
using privateer::testing::count_block_files;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	// counts the staging link calls so a crash test can kill mid-staging
	std::atomic<int> g_links_until_kill{-1};

	struct RegionSnapshotTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();
		fs::path const src = dir.path / "src";
		fs::path const dst = dir.path / "dst";

		region open_src_ab() {
			privateer::testing::build_committed_store(src, bs, {'a', 'b'});
			auto reg = region::open(src);
			EXPECT_TRUE(reg.has_value()) << to_string(reg.error());
			return std::move(*reg);
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	TEST_F(RegionSnapshotTest, ASnapshotIsASelfContainedDatastore) {
		auto reg = open_src_ab();
		bytes(reg)[0] = 'c';  // dirty at snapshot time
		ASSERT_TRUE(reg.snapshot_to(dst));

		auto snap = region::open(dst);
		ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
		EXPECT_EQ(bytes(*snap)[0], 'c');  // the snapshot's commit ran first
		EXPECT_EQ(bytes(*snap)[bs], 'b');
		EXPECT_EQ(snap->size(), 2 * bs);
		EXPECT_EQ(snap->block_size(), bs);
	}

	TEST_F(RegionSnapshotTest, TheSnapshotCommitLandsInTheSourceToo) {
		{
			auto reg = open_src_ab();
			bytes(reg)[0] = 'c';
			ASSERT_TRUE(reg.snapshot_to(dst));
		}
		auto reopened = region::open(src);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 'c');
	}

	TEST_F(RegionSnapshotTest, SnapshotsShareBlocksThroughHardLinks) {
		auto reg = open_src_ab();
		ASSERT_TRUE(reg.snapshot_to(dst));
		EXPECT_EQ(count_block_files(dst), 2u);

		auto store = block_store::open(src);
		ASSERT_TRUE(store.has_value());
		std::vector<std::byte> const data(bs, std::byte{'a'});
		EXPECT_EQ(fs::hard_link_count(store->block_path(hash_block(hash_algorithm::xxh3_128, data))), 2u);
	}

	TEST_F(RegionSnapshotTest, LaterSourceCommitsDoNotDisturbTheSnapshot) {
		auto reg = open_src_ab();
		ASSERT_TRUE(reg.snapshot_to(dst));

		bytes(reg)[0] = 'z';
		ASSERT_TRUE(reg.commit(true));  // reclaims the source's name for 'a'
		ASSERT_EQ(count_block_files(src), 2u);

		auto snap = region::open(dst);  // the snapshot's link kept the inode alive
		ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
		EXPECT_EQ(bytes(*snap)[0], 'a');
		EXPECT_EQ(bytes(*snap)[bs], 'b');
	}

	TEST_F(RegionSnapshotTest, DeletingTheSourceKeepsTheSnapshot) {
		{
			auto reg = open_src_ab();
			ASSERT_TRUE(reg.snapshot_to(dst));
		}
		fs::remove_all(src);
		auto snap = region::open(dst);
		ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
		EXPECT_EQ(bytes(*snap)[0], 'a');
	}

	TEST_F(RegionSnapshotTest, SnapshotIntoAnOccupiedDirectoryFails) {
		auto reg = open_src_ab();
		privateer::testing::build_committed_store(dst, bs, {'x'});
		auto staged = reg.snapshot_to(dst);
		ASSERT_FALSE(staged.has_value());
		EXPECT_EQ(staged.error().code, errc::datastore_exists);
	}

	TEST_F(RegionSnapshotTest, TheCopyFallbackUnsharesTheBlocks) {
		auto reg = open_src_ab();
		detail_region::link_fn = [](char const *, char const *) {
			errno = EXDEV;
			return -1;
		};
		auto const staged = reg.snapshot_to(dst);
		detail_region::link_fn = ::link;
		ASSERT_TRUE(staged.has_value()) << to_string(staged.error());

		auto snap = region::open(dst);
		ASSERT_TRUE(snap.has_value());
		EXPECT_EQ(bytes(*snap)[0], 'a');
		auto store = block_store::open(dst);
		ASSERT_TRUE(store.has_value());
		std::vector<std::byte> const data(bs, std::byte{'a'});
		EXPECT_EQ(fs::hard_link_count(store->block_path(hash_block(hash_algorithm::xxh3_128, data))), 1u);
	}

	TEST_F(RegionSnapshotTest, AReadOnlyRegionSnapshots) {
		privateer::testing::build_committed_store(src, bs, {'a'});
		auto reg = region::open_read_only(src);
		ASSERT_TRUE(reg.has_value());
		ASSERT_TRUE(reg->snapshot_to(dst));
		auto snap = region::open(dst);
		ASSERT_TRUE(snap.has_value());
		EXPECT_EQ(bytes(*snap)[0], 'a');
	}

	TEST_F(RegionSnapshotTest, CopyStagesFromDisk) {
		privateer::testing::build_committed_store(src, bs, {'a', std::nullopt, 'c'});
		ASSERT_TRUE(region::copy(src, dst));

		auto snap = region::open(dst);
		ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
		EXPECT_EQ(bytes(*snap)[0], 'a');
		EXPECT_EQ(bytes(*snap)[bs], 0);
		EXPECT_EQ(bytes(*snap)[2 * bs], 'c');
		EXPECT_EQ(count_block_files(src), 2u);  // the source is untouched
	}

	TEST_F(RegionSnapshotTest, CopyValidatesTheSource) {
		privateer::testing::build_committed_store(src, bs, {'a'});
		auto store = block_store::open(src);
		ASSERT_TRUE(store.has_value());
		std::vector<std::byte> const data(bs, std::byte{'a'});
		fs::remove(store->block_path(hash_block(hash_algorithm::xxh3_128, data)));

		auto copied = region::copy(src, dst);
		ASSERT_FALSE(copied.has_value());
		EXPECT_EQ(copied.error().code, errc::datastore_inconsistent);
		EXPECT_FALSE(fs::exists(dst));  // validation fails before any staging
	}

	TEST_F(RegionSnapshotTest, AKillMidStagingLeavesNoValidDatastoreAndAnIntactSource) {
		privateer::testing::build_committed_store(src, bs, {'a', 'b'});
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();
			auto reg = region::open(src);
			if (!reg) {
				return 10;
			}
			static_cast<unsigned char volatile *>(reg->segment())[0] = 'c';
			g_links_until_kill.store(1);  // die on the second link
			detail_region::link_fn = [](char const *from, char const *to) {
				if (g_links_until_kill.fetch_sub(1) == 0) {
					::raise(SIGKILL);
				}
				return ::link(from, to);
			};
			(void) reg->snapshot_to(dst);
			return 11;  // the kill point was never reached
		};
		ASSERT_EQ(res, subprocess_result::killed);

		// the torn staging directory is never a valid datastore
		auto torn = region::open(dst);
		ASSERT_FALSE(torn.has_value());
		EXPECT_EQ(torn.error().code, errc::datastore_missing);  // no recipe was staged

		// the source is intact and carries the snapshot's durable commit
		auto reopened = region::open(src);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'c');
		EXPECT_EQ(bytes(*reopened)[bs], 'b');
	}

}  // namespace
