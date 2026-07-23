#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/temp_dir.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

using namespace privateer;
using privateer::testing::is_fault_signal;
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

		// builds a committed datastore through the storage layer: one entry
		// per element, a fill character for a block or nullopt for the empty
		// sentinel
		void build_store(std::vector<std::optional<char>> const &slots, uint64_t capacity_slots = 16) {
			auto store = block_store::create(dir.path);
			ASSERT_TRUE(store.has_value()) << to_string(store.error());
			recipe rec;
			rec.block_size = bs;
			rec.capacity = capacity_slots * bs;
			rec.size = slots.size() * bs;
			rec.algorithm = hash_algorithm::xxh3_128;
			std::vector<block_digest> names;
			for (auto const &fill : slots) {
				if (!fill) {
					rec.entries.emplace_back();
					continue;
				}
				std::vector<std::byte> const data(bs, static_cast<std::byte>(*fill));
				auto const name = hash_block(rec.algorithm, data);
				ASSERT_TRUE(store->publish(name, data));
				names.push_back(name);
				rec.entries.push_back(name);
			}
			ASSERT_TRUE(store->make_durable(names));
			ASSERT_TRUE(rec.commit(dir.path, true));
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
		create_options.algorithm = hash_algorithm::blake3;
		ASSERT_TRUE(region::create(dir.path, 4 * bs, create_options));

		auto reg = region::open(dir.path);  // no options requested
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		EXPECT_EQ(reg->block_size(), bs);
		EXPECT_EQ(reg->algorithm(), hash_algorithm::blake3);
	}

	TEST_F(RegionTest, RequestedOptionsMustMatchTheHeader) {
		ASSERT_TRUE(region::create(dir.path, 4 * bs, small_options()));

		region_options wrong_block = small_options();
		wrong_block.block_size = 2 * bs;
		EXPECT_EQ(region::open(dir.path, wrong_block).error().code, errc::option_mismatch);

		region_options wrong_algorithm = small_options();
		wrong_algorithm.algorithm = hash_algorithm::sha256;
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
