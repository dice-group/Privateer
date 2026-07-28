#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/recipe.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include <sys/resource.h>

using namespace privateer;
using privateer::testing::is_fault_signal;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	struct RegionTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();  // one-page blocks keep the tests small

		[[nodiscard]] region_options small_options() const {
			region_options options;
			options.block_size = bs;
			return options;
		}

		void build_store(std::vector<std::optional<char>> const &slots, uint64_t capacity_slots = 16) {
			privateer::testing::build_committed_store(dir.path, bs, slots, capacity_slots);
		}

		[[nodiscard]] static unsigned char read_byte(region const &reg, uint64_t offset) {
			return static_cast<unsigned char volatile *>(reg.segment())[offset];
		}
	};

	TEST_F(RegionTest, CreateMakesAnEmptyDatastore) {
		auto reg = region::create(dir.path, 4 * bs + 1, small_options());
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(reg->size(), 0u);
		EXPECT_EQ(reg->capacity(), 5 * bs);  // rounded up to whole slots
		EXPECT_EQ(reg->block_size(), bs);
		EXPECT_EQ(reg->algorithm(), hash_algorithm::xxh3_128);
		EXPECT_FALSE(reg->read_only());
		EXPECT_NE(reg->segment(), nullptr);
	}

	TEST_F(RegionTest, CreateRefusesAnExistingDatastore) {
		ASSERT_TRUE(region::create(dir.path, 4 * bs, small_options()));
		auto again = region::create(dir.path, 4 * bs, small_options());
		ASSERT_FALSE(again.has_value());
		EXPECT_EQ(again.error().code, errc::datastore_exists);
	}

	TEST_F(RegionTest, CreateRejectsBadConstants) {
		region_options unaligned;
		unaligned.block_size = bs + 1;
		EXPECT_EQ(region::create(dir.path, 4 * bs, unaligned).error().code, errc::invalid_argument);
		EXPECT_EQ(region::create(dir.path, 0, small_options()).error().code, errc::invalid_argument);
	}

	TEST_F(RegionTest, OpenMissingDatastoreFails) {
		auto reg = region::open(dir.path / "nothing");
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::datastore_missing);
	}

	TEST_F(RegionTest, OpenAdoptsTheRecipeHeader) {
		region_options create_options = small_options();
		create_options.algorithm = hash_algorithm::xxh3_128;
		ASSERT_TRUE(region::create(dir.path, 4 * bs, create_options));

		auto reg = region::open(dir.path);  // no options requested
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(reg->block_size(), bs);
		EXPECT_EQ(reg->algorithm(), hash_algorithm::xxh3_128);
	}

	TEST_F(RegionTest, RequestedOptionsMustMatchTheHeader) {
		ASSERT_TRUE(region::create(dir.path, 4 * bs, small_options()));

		region_options wrong_block = small_options();
		wrong_block.block_size = 2 * bs;
		EXPECT_EQ(region::open(dir.path, wrong_block).error().code, errc::option_mismatch);

		// a retired id: the only algorithm this build serves is the one the
		// header already names, so the mismatch needs an id from outside the
		// enumeration
		region_options wrong_algorithm = small_options();
		wrong_algorithm.algorithm = static_cast<hash_algorithm>(3);
		EXPECT_EQ(region::open(dir.path, wrong_algorithm).error().code, errc::option_mismatch);

		EXPECT_TRUE(region::open(dir.path, small_options()).has_value());
	}

	TEST_F(RegionTest, CommittedDataIsReadable) {
		build_store({'a', 'b'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(reg->size(), 2 * bs);
		EXPECT_EQ(read_byte(*reg, 0), 'a');
		EXPECT_EQ(read_byte(*reg, bs - 1), 'a');
		EXPECT_EQ(read_byte(*reg, bs), 'b');
		EXPECT_EQ(read_byte(*reg, 2 * bs - 1), 'b');
	}

	TEST_F(RegionTest, EmptySentinelSlotsReadZeros) {
		build_store({'a', std::nullopt, 'c'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(read_byte(*reg, 0), 'a');
		EXPECT_EQ(read_byte(*reg, bs), 0);
		EXPECT_EQ(read_byte(*reg, 2 * bs - 1), 0);
		EXPECT_EQ(read_byte(*reg, 2 * bs), 'c');
	}

	TEST_F(RegionTest, DeduplicatedSlotsShareOneBlock) {
		build_store({'a', 'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(read_byte(*reg, 0), 'a');
		EXPECT_EQ(read_byte(*reg, bs), 'a');
	}

	TEST_F(RegionTest, MissingReferencedBlockFailsOpen) {
		build_store({'a'});
		std::vector<std::byte> const data(bs, std::byte{'a'});
		auto store = block_store::open(dir.path);
		ASSERT_TRUE(store.has_value());
		fs::remove(store->block_path(hash_block(hash_algorithm::xxh3_128, data)));

		auto reg = region::open(dir.path);
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::datastore_inconsistent);
	}

	TEST_F(RegionTest, WrongSizeBlockFailsOpen) {
		build_store({'a'});
		std::vector<std::byte> const data(bs, std::byte{'a'});
		auto store = block_store::open(dir.path);
		ASSERT_TRUE(store.has_value());
		fs::resize_file(store->block_path(hash_block(hash_algorithm::xxh3_128, data)), bs - 1);

		auto reg = region::open(dir.path);
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::block_file_invalid);
	}

	TEST_F(RegionTest, OnlyReadWriteOpensSweep) {
		build_store({'a'});
		fs::path const stray = dir.path / "blocks" / "42" / "not-a-block";
		std::ofstream{stray} << "junk";
		ASSERT_TRUE(fs::exists(stray));

		ASSERT_TRUE(region::open_read_only(dir.path).has_value());
		EXPECT_TRUE(fs::exists(stray));  // read-only opens never mutate the datastore

		ASSERT_TRUE(region::open(dir.path).has_value());
		EXPECT_FALSE(fs::exists(stray));
	}

	TEST_F(RegionTest, ReadOnlyStrayWriteDies) {
		build_store({'a'});
		auto reg = region::open_read_only(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_TRUE(reg->read_only());
		EXPECT_EQ(read_byte(*reg, 0), 'a');
		auto const res = PRIVATEER_SANDBOX {
			static_cast<unsigned char volatile *>(reg->segment())[0] = 'x';
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST_F(RegionTest, ExtendGrowsInWholeSlots) {
		auto reg = region::create(dir.path, 8 * bs, small_options());
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());

		ASSERT_TRUE(reg->extend(1));
		EXPECT_EQ(reg->size(), bs);
		EXPECT_EQ(read_byte(*reg, 0), 0);

		ASSERT_TRUE(reg->extend(3 * bs + 5));
		EXPECT_EQ(reg->size(), 4 * bs);
		EXPECT_EQ(read_byte(*reg, 4 * bs - 1), 0);

		ASSERT_TRUE(reg->extend(2 * bs));  // at or below the current size: no-op
		EXPECT_EQ(reg->size(), 4 * bs);
	}

	TEST_F(RegionTest, ExtendBeyondCapacityFails) {
		auto reg = region::create(dir.path, 2 * bs, small_options());
		ASSERT_TRUE(reg.has_value());
		auto extended = reg->extend(3 * bs);
		ASSERT_FALSE(extended.has_value());
		EXPECT_EQ(extended.error().code, errc::capacity_exceeded);
		EXPECT_EQ(reg->size(), 0u);
	}

	TEST_F(RegionTest, ExtendOnAReadOnlyRegionFails) {
		build_store({'a'});
		auto reg = region::open_read_only(dir.path);
		ASSERT_TRUE(reg.has_value());
		auto extended = reg->extend(2 * bs);
		ASSERT_FALSE(extended.has_value());
		EXPECT_EQ(extended.error().code, errc::invalid_argument);
	}

	TEST_F(RegionTest, ReadOfTheUnextendedTailDies) {
		auto reg = region::create(dir.path, 4 * bs, small_options());
		ASSERT_TRUE(reg.has_value());
		ASSERT_TRUE(reg->extend(bs));
		auto const res = PRIVATEER_SANDBOX {
			return static_cast<int>(read_byte(*reg, bs));  // beyond size: still PROT_NONE
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST_F(RegionTest, ExtendIsNotPersistedWithoutACommit) {
		{
			auto reg = region::create(dir.path, 4 * bs, small_options());
			ASSERT_TRUE(reg.has_value());
			ASSERT_TRUE(reg->extend(2 * bs));
			EXPECT_EQ(reg->size(), 2 * bs);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 0u);  // the recipe still records the created size
	}

	TEST_F(RegionTest, TheSegmentHeaderIsMappedWritable) {
		region_options options = small_options();
		options.header_size = 100;
		auto reg = region::create(dir.path, 4 * bs, options);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(static_cast<std::byte *>(reg->segment()),
				  static_cast<std::byte *>(reg->segment_header()) + page_size());
		auto *header = static_cast<unsigned char volatile *>(reg->segment_header());
		header[0] = 1;
		header[page_size() - 1] = 2;
		EXPECT_EQ(header[0], 1);
		EXPECT_EQ(header[page_size() - 1], 2);
	}

	TEST_F(RegionTest, RegionsAreMovable) {
		build_store({'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		region moved = std::move(*reg);
		EXPECT_EQ(read_byte(moved, 0), 'a');
		EXPECT_EQ(moved.size(), bs);
	}

	TEST_F(RegionTest, DeepVerifyRejectsACorruptedBlockFile) {
		build_store({'a', 'b'});
		std::vector<std::byte> const data(bs, std::byte{'b'});
		auto store = block_store::open(dir.path);
		ASSERT_TRUE(store.has_value());
		fs::path const path = store->block_path(hash_block(hash_algorithm::xxh3_128, data));
		{
			std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
			file.seekp(7);
			file.put('x');
		}
		ASSERT_EQ(fs::file_size(path), bs);  // size validation cannot see the corruption

		region_options verify = small_options();
		verify.deep_verify = true;
		auto rejected = region::open(dir.path, verify);
		ASSERT_FALSE(rejected.has_value());
		EXPECT_EQ(rejected.error().code, errc::block_file_invalid);

		auto accepted = region::open(dir.path, small_options());
		ASSERT_TRUE(accepted.has_value()) << to_string(accepted.error());
	}

	TEST_F(RegionTest, DeepVerifyPassesOnAnIntactStore) {
		build_store({'a', std::nullopt, 'b', 'a'});
		region_options verify = small_options();
		verify.deep_verify = true;
		{
			auto reg = region::open(dir.path, verify);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			EXPECT_EQ(read_byte(*reg, 0), 'a');
			EXPECT_EQ(read_byte(*reg, bs), 0);
			EXPECT_EQ(read_byte(*reg, 2 * bs), 'b');
			EXPECT_EQ(read_byte(*reg, 3 * bs), 'a');
		}
		auto ro = region::open_read_only(dir.path, verify);
		ASSERT_TRUE(ro.has_value()) << to_string(ro.error());
		EXPECT_EQ(read_byte(*ro, 2 * bs), 'b');
	}

	TEST_F(RegionTest, LockedStateBytesTrackTheSizeNotTheCapacity) {
		uint64_t const slots_per_page = page_size() / sizeof(uint32_t);
		auto reg = region::create(dir.path, 2 * slots_per_page * bs, small_options());
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		auto &table = detail_region::table_of(*reg);
		EXPECT_TRUE(table.locking());
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(0));
		EXPECT_LT(table.locked_bytes(), slot_table::locked_bytes_for(2 * slots_per_page));

		ASSERT_TRUE(reg->extend(bs));
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(1));

		ASSERT_TRUE(reg->extend((slots_per_page + 1) * bs));
		EXPECT_EQ(table.locked_bytes(), slot_table::locked_bytes_for(slots_per_page + 1));
		EXPECT_GT(table.locked_bytes(), slot_table::locked_bytes_for(1));
	}

	TEST_F(RegionTest, OpenLocksTheStateArrayForTheCommittedSize) {
		uint64_t const slots_per_page = page_size() / sizeof(uint32_t);
		build_store({'a', 'b'}, 2 * slots_per_page);
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			EXPECT_EQ(detail_region::table_of(*reg).locked_bytes(), slot_table::locked_bytes_for(2));
		}
		auto ro = region::open_read_only(dir.path);
		ASSERT_TRUE(ro.has_value()) << to_string(ro.error());
		EXPECT_FALSE(detail_region::table_of(*ro).locking());  // read-only opens never lock
		EXPECT_EQ(detail_region::table_of(*ro).locked_bytes(), 0u);
	}

	TEST_F(RegionTest, ExtendFailsCleanlyUnderALoweredMemlockLimit) {
		uint64_t const slots_per_page = page_size() / sizeof(uint32_t);
		auto const res = PRIVATEER_SANDBOX {
			// fresh dispositions: the sandbox cleared the inherited handler
			privateer::detail_fault_handler::uninstall_for_tests();
			auto reg = region::create(dir.path, 2 * slots_per_page * bs, small_options());
			if (!reg) {
				return 10;
			}
			if (!reg->extend(bs)) {
				return 11;
			}
			rlimit const zero{0, 0};
			if (::setrlimit(RLIMIT_MEMLOCK, &zero) != 0) {
				return 12;
			}
			// crossing into a new state page needs a lock the limit refuses
			auto crossed = reg->extend((slots_per_page + 1) * bs);
			if (crossed.has_value()) {
				return 13;
			}
			if (crossed.error().code != errc::memlock_limit_too_low) {
				return 14;
			}
			if (reg->size() != bs) {
				return 15;  // the failed extend must not advance the size
			}
			// the region keeps working within the already locked pages
			if (!reg->extend(2 * bs)) {
				return 16;
			}
			static_cast<unsigned char volatile *>(reg->segment())[0] = 'x';
			if (!reg->commit(true)) {
				return 17;
			}
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

#ifdef __linux__
	TEST_F(RegionTest, TheVmaBudgetGatesExtend) {
		region_options options = small_options();
		options.vma_headroom = SIZE_MAX;  // leaves a budget of zero slots
		auto reg = region::create(dir.path, 4 * bs, options);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());  // zero mapped slots fit the budget
		auto extended = reg->extend(bs);
		ASSERT_FALSE(extended.has_value());
		EXPECT_EQ(extended.error().code, errc::vma_budget_exceeded);
	}

	TEST_F(RegionTest, TheVmaBudgetGatesOpen) {
		build_store({'a'});
		region_options options;
		options.vma_headroom = SIZE_MAX;
		auto reg = region::open(dir.path, options);
		ASSERT_FALSE(reg.has_value());
		EXPECT_EQ(reg.error().code, errc::vma_budget_exceeded);
	}
#endif

}  // namespace
