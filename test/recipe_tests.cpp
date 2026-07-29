// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Tests for the recipe: serialization round trip, field validation, the
// atomic commit through the file, and corruption, version, and block
// validation on the load path. Corruption cases patch serialized bytes
// and, where the damage must sit behind an intact checksum, recompute it.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace privateer;
namespace fs = std::filesystem;

namespace {

	constexpr uint64_t test_block_size = 4096;

	block_digest digest_of(std::string const &text) {
		auto const *bytes = reinterpret_cast<std::byte const *>(text.data());
		return hash_block(hash_algorithm::xxh3_128, {bytes, text.size()});
	}

	recipe sample_recipe() {
		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = 64 * test_block_size;
		rec.size = 3 * test_block_size;
		rec.algorithm = hash_algorithm::xxh3_128;
		rec.entries = {digest_of("block one"), block_digest{}, digest_of("block three")};
		return rec;
	}

	// re-stamps both checksums so damage before them stays deliberate
	void fix_checksums(std::vector<std::byte> &bytes) {
		auto const store_le64 = [](std::byte *out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out[i] = static_cast<std::byte>(value >> (8 * i));
			}
		};
		store_le64(bytes.data() + 48, checksum64({bytes.data(), 48}));
		size_t const entry_bytes = bytes.size() - 56 - 8;
		store_le64(bytes.data() + 56 + entry_bytes, checksum64({bytes.data() + 56, entry_bytes}));
	}

	TEST(Recipe, RoundTripPreservesEverything) {
		recipe const rec = sample_recipe();
		auto bytes = rec.serialize();
		ASSERT_TRUE(bytes.has_value()) << to_string(bytes.error());

		auto loaded = recipe::deserialize(*bytes);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->block_size, rec.block_size);
		EXPECT_EQ(loaded->capacity, rec.capacity);
		EXPECT_EQ(loaded->size, rec.size);
		EXPECT_EQ(loaded->algorithm, rec.algorithm);
		ASSERT_EQ(loaded->entries.size(), rec.entries.size());
		EXPECT_EQ(loaded->entries[0], rec.entries[0]);
		EXPECT_EQ(loaded->entries[1].size, 0);
		EXPECT_EQ(loaded->entries[2], rec.entries[2]);
	}

	TEST(Recipe, EmptyRegionRoundTrips) {
		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = 64 * test_block_size;
		auto bytes = rec.serialize();
		ASSERT_TRUE(bytes.has_value());
		auto loaded = recipe::deserialize(*bytes);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_TRUE(loaded->entries.empty());
	}

	TEST(Recipe, SerializeRejectsInconsistentFields) {
		recipe rec = sample_recipe();
		rec.block_size = 0;
		EXPECT_EQ(rec.serialize().error().code, errc::invalid_argument);

		rec = sample_recipe();
		rec.size += 1;
		EXPECT_EQ(rec.serialize().error().code, errc::invalid_argument);

		rec = sample_recipe();
		rec.capacity = rec.size - test_block_size;
		EXPECT_EQ(rec.serialize().error().code, errc::invalid_argument);

		rec = sample_recipe();
		rec.entries.pop_back();
		EXPECT_EQ(rec.serialize().error().code, errc::invalid_argument);

		rec = sample_recipe();
		rec.entries[0] = hash_block(hash_algorithm::xxh3_128, {});
		rec.entries[0].size = 8;  // narrower than xxh3_128's 16 bytes
		EXPECT_EQ(rec.serialize().error().code, errc::invalid_argument);
	}

	// The write path refuses an algorithm this build cannot compute, the same
	// way the read path does, so no commit reaches hash_block without an
	// implementation for the id in the header.
	TEST(Recipe, SerializeRefusesAnAlgorithmTheBuildDoesNotKnow) {
		recipe rec = sample_recipe();
		rec.algorithm = static_cast<hash_algorithm>(3);  // retired sha256 id
		EXPECT_EQ(rec.serialize().error().code, errc::recipe_unsupported);
	}

	// Format version 1 is frozen. This pins every field at its offset and the
	// entry stride, so a change to the layout fails here instead of silently
	// making existing stores unreadable. An intended change takes format
	// version 2. The digest values themselves are pinned in the block hash
	// tests against independent vectors, so this asserts placement only.
	TEST(Recipe, FormatVersion1IsFrozen) {
		auto const bytes = sample_recipe().serialize();
		ASSERT_TRUE(bytes.has_value()) << to_string(bytes.error());

		constexpr size_t header = 56;
		constexpr size_t entry = 16;  // the xxh3-128 digest width
		ASSERT_EQ(bytes->size(), header + 3 * entry + sizeof(uint64_t));

		auto const at = [&](size_t offset) { return static_cast<unsigned>((*bytes)[offset]); };
		auto const le64_at = [&](size_t offset) {
			uint64_t value = 0;
			for (int i = 7; i >= 0; --i) {
				value = (value << 8) | at(offset + static_cast<size_t>(i));
			}
			return value;
		};

		EXPECT_EQ(std::string(reinterpret_cast<char const *>(bytes->data()), 8), "PVRECIPE");
		EXPECT_EQ(at(8), 1u);   // format version, little endian uint32
		EXPECT_EQ(at(9), 0u);
		EXPECT_EQ(at(10), 0u);
		EXPECT_EQ(at(11), 0u);
		EXPECT_EQ(at(12), 1u);   // hash algorithm id, xxh3_128
		EXPECT_EQ(at(13), entry);  // entry width
		EXPECT_EQ(at(14), 0u);   // reserved
		EXPECT_EQ(at(15), 0u);
		EXPECT_EQ(le64_at(16), test_block_size);
		EXPECT_EQ(le64_at(24), 64 * test_block_size);
		EXPECT_EQ(le64_at(32), 3 * test_block_size);
		EXPECT_EQ(le64_at(40), 3u);  // slot count

		// entries sit at the header, one digest wide each, sentinel all zero
		auto const entry_at = [&](size_t index) {
			return std::span<std::byte const>{bytes->data() + header + index * entry, entry};
		};
		auto const one = digest_of("block one");
		auto const three = digest_of("block three");
		EXPECT_TRUE(std::equal(entry_at(0).begin(), entry_at(0).end(), one.bytes.begin()));
		EXPECT_TRUE(std::ranges::all_of(entry_at(1), [](std::byte b) { return b == std::byte{0}; }));
		EXPECT_TRUE(std::equal(entry_at(2).begin(), entry_at(2).end(), three.bytes.begin()));

		// and it reads back to the same recipe
		auto const back = recipe::deserialize(*bytes);
		ASSERT_TRUE(back.has_value()) << to_string(back.error());
		EXPECT_EQ(back->block_size, test_block_size);
		EXPECT_EQ(back->algorithm, hash_algorithm::xxh3_128);
		EXPECT_EQ(back->entries.size(), 3u);
		EXPECT_EQ(back->entries[0], one);
		EXPECT_EQ(back->entries[1].size, 0u);
		EXPECT_EQ(back->entries[2], three);
	}

	// The width lives in the header, so a recipe written before it existed
	// carries a zero there and must fail rather than parse at 32 bytes.
	TEST(Recipe, AZeroEntryWidthIsRejected) {
		auto bytes = sample_recipe().serialize();
		ASSERT_TRUE(bytes.has_value());
		(*bytes)[13] = std::byte{0};
		fix_checksums(*bytes);
		EXPECT_EQ(recipe::deserialize(*bytes).error().code, errc::recipe_corrupt);
	}

	TEST(Recipe, AnEntryWidthThatDisagreesWithTheAlgorithmIsRejected) {
		auto bytes = sample_recipe().serialize();
		ASSERT_TRUE(bytes.has_value());
		(*bytes)[13] = std::byte{32};  // what the pre-freeze layout used
		fix_checksums(*bytes);
		EXPECT_EQ(recipe::deserialize(*bytes).error().code, errc::recipe_corrupt);
	}

	TEST(Recipe, CommitPublishesAtomicallyAndLoadReadsBack) {
		privateer::testing::temp_dir dir;
		recipe const rec = sample_recipe();

		auto missing = recipe::load(dir.path);
		ASSERT_FALSE(missing.has_value());
		EXPECT_EQ(missing.error().code, errc::datastore_missing);

		ASSERT_TRUE(rec.commit(dir.path, true));
		auto loaded = recipe::load(dir.path);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries.size(), 3u);

		recipe grown = rec;
		grown.size += test_block_size;
		grown.entries.push_back(digest_of("block four"));
		ASSERT_TRUE(grown.commit(dir.path, false));
		loaded = recipe::load(dir.path);
		ASSERT_TRUE(loaded.has_value());
		EXPECT_EQ(loaded->entries.size(), 4u);

		size_t files = 0;
		for ([[maybe_unused]] auto const &entry : fs::directory_iterator{dir.path}) {
			++files;
		}
		EXPECT_EQ(files, 1u);  // only _recipe, no staging litter
	}

	TEST(Recipe, DamageIsDetected) {
		auto const pristine = *sample_recipe().serialize();

		auto bytes = pristine;
		bytes[0] = std::byte{'X'};
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);

		bytes = pristine;
		bytes[20] ^= std::byte{0x01};  // block_size, behind the header checksum
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);

		bytes = pristine;
		bytes[56] ^= std::byte{0x01};  // first entry byte, behind the entries checksum
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);

		bytes = pristine;
		bytes.resize(bytes.size() - 1);
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);

		bytes = pristine;
		bytes.push_back(std::byte{0});
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);

		EXPECT_EQ(recipe::deserialize({pristine.data(), 10}).error().code, errc::recipe_corrupt);
	}

	TEST(Recipe, InconsistentHeaderFieldsAreCorrupt) {
		auto bytes = *sample_recipe().serialize();
		bytes[40] = std::byte{7};  // slot count no longer matches size
		fix_checksums(bytes);
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_corrupt);
	}

	TEST(Recipe, NewerFormatVersionIsUnsupported) {
		auto bytes = *sample_recipe().serialize();
		bytes[8] = std::byte{2};
		fix_checksums(bytes);
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_unsupported);
	}

	TEST(Recipe, UnknownHashAlgorithmIsUnsupported) {
		auto bytes = *sample_recipe().serialize();
		bytes[12] = std::byte{200};
		fix_checksums(bytes);
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_unsupported);
	}

	TEST(Recipe, ValidateBlocksChecksExistenceAndSize) {
		privateer::testing::temp_dir dir;
		auto store = block_store::create(dir.path);
		ASSERT_TRUE(store.has_value());

		std::vector<std::byte> const block(test_block_size, std::byte{0x5A});
		auto const name = hash_block(hash_algorithm::xxh3_128, block);
		ASSERT_TRUE(store->publish(name, block));

		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = 64 * test_block_size;
		rec.size = 2 * test_block_size;
		rec.entries = {name, block_digest{}};
		EXPECT_TRUE(validate_blocks(rec, *store));

		rec.size = 3 * test_block_size;
		rec.entries.push_back(digest_of("never published"));
		auto absent = validate_blocks(rec, *store);
		ASSERT_FALSE(absent.has_value());
		EXPECT_EQ(absent.error().code, errc::datastore_inconsistent);

		std::vector<std::byte> const short_block(test_block_size / 2, std::byte{0x5A});
		auto const short_name = digest_of("short block");
		ASSERT_TRUE(store->publish(short_name, short_block));
		rec.entries[2] = short_name;
		auto wrong_size = validate_blocks(rec, *store);
		ASSERT_FALSE(wrong_size.has_value());
		EXPECT_EQ(wrong_size.error().code, errc::block_file_invalid);
	}

}  // namespace
