// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Tests for the recipe: the frozen version 1 buffer, the version 2 commit
// and load over the block store, and corruption, version, and block
// validation on the load path. Corruption cases patch committed bytes and,
// where the damage must sit behind an intact checksum, recompute it.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace privateer;
namespace fs = std::filesystem;

namespace {

	constexpr uint64_t test_block_size = 4096;

	void store_le64(std::byte *out, uint64_t value) {
		for (int i = 0; i < 8; ++i) {
			out[i] = static_cast<std::byte>(value >> (8 * i));
		}
	}

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

	// A recipe over slots slots, every slot named after its index. picker
	// decides which slots stay the empty sentinel.
	recipe wide_recipe(size_t slots, std::function<bool(size_t)> const &empty = {}) {
		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = (slots + 8) * test_block_size;
		rec.size = slots * test_block_size;
		rec.algorithm = hash_algorithm::xxh3_128;
		rec.entries.resize(slots);
		for (size_t slot = 0; slot < slots; ++slot) {
			if (!empty || !empty(slot)) {
				rec.entries[slot] = digest_of("slot " + std::to_string(slot));
			}
		}
		return rec;
	}

	std::vector<std::byte> read_file(fs::path const &path) {
		std::ifstream in{path, std::ios::binary | std::ios::ate};
		auto const size = static_cast<size_t>(in.tellg());
		in.seekg(0);
		std::vector<std::byte> bytes(size);
		in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
		return bytes;
	}

	void write_file(fs::path const &path, std::span<std::byte const> bytes) {
		std::ofstream out{path, std::ios::binary | std::ios::trunc};
		out.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	// re-stamps both checksums of a version 1 buffer so damage before them
	// stays deliberate
	void fix_checksums(std::vector<std::byte> &bytes) {
		store_le64(bytes.data() + 48, checksum64({bytes.data(), 48}));
		size_t const entry_bytes = bytes.size() - 56 - 8;
		store_le64(bytes.data() + 56 + entry_bytes, checksum64({bytes.data() + 56, entry_bytes}));
	}

	// the same for a manifest: the header checksum and the record table checksum
	void fix_manifest_checksums(std::vector<std::byte> &bytes) {
		store_le64(bytes.data() + 48, checksum64({bytes.data(), 48}));
		size_t const table_bytes = bytes.size() - 56 - 8;
		store_le64(bytes.data() + bytes.size() - 8, checksum64({bytes.data() + 56, table_bytes}));
	}

	// the trailing checksum of a segment file, so damage in the payload is
	// caught by the content digest alone
	void fix_segment_checksum(std::vector<std::byte> &bytes) {
		store_le64(bytes.data() + bytes.size() - 8, checksum64({bytes.data(), bytes.size() - 8}));
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
		EXPECT_TRUE(loaded->segments.empty());  // version 1 has no segments
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
	// making existing stores unreadable. The digest values themselves are
	// pinned in the block hash tests against independent vectors, so this
	// asserts placement only.
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
		bytes[8] = std::byte{3};
		fix_checksums(bytes);
		EXPECT_EQ(recipe::deserialize(bytes).error().code, errc::recipe_unsupported);
	}

	// A version 2 buffer is a manifest, so it needs the store and the single
	// buffer reader refuses it.
	TEST(Recipe, DeserializeRefusesAManifest) {
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

	// Everything that needs the store: the version 2 commit publishes the
	// segment files into it, and load reads them back through it.
	struct RecipeStore : ::testing::Test {
		privateer::testing::temp_dir dir;
		std::optional<block_store> store;

		void SetUp() override {
			auto created = block_store::create(dir.path);
			ASSERT_TRUE(created.has_value()) << to_string(created.error());
			store.emplace(std::move(*created));
		}

		[[nodiscard]] fs::path manifest_path() const { return dir.path / recipe_file_name; }

		std::vector<segment_record> commit_ok(recipe const &rec, bool durable = true) {
			auto published = rec.commit(dir.path, *store, durable);
			EXPECT_TRUE(published.has_value()) << to_string(published.error());
			return published ? std::move(*published) : std::vector<segment_record>{};
		}

		[[nodiscard]] size_t block_files() const {
			size_t count = 0;
			for (auto const &shard : fs::directory_iterator{dir.path / "blocks"}) {
				for ([[maybe_unused]] auto const &entry : fs::directory_iterator{shard}) {
					++count;
				}
			}
			return count;
		}
	};

	// The version 1 read path through load: a legacy file in the manifest
	// slot, read whole, with no segment files anywhere.
	TEST_F(RecipeStore, AVersionOneFileLoadsThroughTheStore) {
		recipe const rec = sample_recipe();
		auto const bytes = rec.serialize();
		ASSERT_TRUE(bytes.has_value()) << to_string(bytes.error());
		write_file(manifest_path(), *bytes);

		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries, rec.entries);
		EXPECT_EQ(loaded->block_size, rec.block_size);
		EXPECT_EQ(loaded->capacity, rec.capacity);
		EXPECT_EQ(loaded->size, rec.size);
		EXPECT_TRUE(loaded->segments.empty());
		EXPECT_EQ(block_files(), 0u);
	}

	TEST_F(RecipeStore, CommitPublishesAtomicallyAndLoadReadsBack) {
		recipe const rec = sample_recipe();

		auto missing = recipe::load(dir.path, *store);
		ASSERT_FALSE(missing.has_value());
		EXPECT_EQ(missing.error().code, errc::datastore_missing);

		auto const records = commit_ok(rec);
		ASSERT_EQ(records.size(), 1u);
		EXPECT_EQ(records[0].encoding, segment_encoding::raw);
		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries, rec.entries);
		EXPECT_EQ(loaded->segments.size(), 1u);
		EXPECT_EQ(loaded->segments[0].digest, records[0].digest);

		recipe grown = rec;
		grown.size += test_block_size;
		grown.entries.push_back(digest_of("block four"));
		ASSERT_FALSE(commit_ok(grown, false).empty());
		loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value());
		EXPECT_EQ(loaded->entries, grown.entries);

		// the segment directory holds the manifest and the blocks tree, and no
		// staging litter
		std::vector<std::string> names;
		for (auto const &entry : fs::directory_iterator{dir.path}) {
			names.push_back(entry.path().filename().string());
		}
		std::ranges::sort(names);
		EXPECT_EQ(names, (std::vector<std::string>{"_recipe", "blocks"}));
	}

	TEST_F(RecipeStore, AnEmptyRecipeHasNoSegments) {
		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = 64 * test_block_size;
		EXPECT_TRUE(commit_ok(rec).empty());
		EXPECT_EQ(block_files(), 0u);
		EXPECT_EQ(fs::file_size(manifest_path()), 80u);  // header plus the table checksum

		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_TRUE(loaded->entries.empty());
		EXPECT_TRUE(loaded->segments.empty());
		EXPECT_EQ(loaded->block_size, test_block_size);
		EXPECT_EQ(loaded->capacity, 64 * test_block_size);
	}

	TEST_F(RecipeStore, SentinelsInsideARawSegmentSurvive) {
		recipe const rec = wide_recipe(11, [](size_t slot) { return slot % 3 == 1; });
		auto const records = commit_ok(rec);
		ASSERT_EQ(records.size(), 1u);
		EXPECT_EQ(records[0].encoding, segment_encoding::raw);
		EXPECT_EQ(records[0].byte_length, 24u + 11u * 16u + 8u);
		EXPECT_EQ(block_files(), 1u);

		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries, rec.entries);
		EXPECT_EQ(loaded->entries[1].size, 0u);
	}

	TEST_F(RecipeStore, ManySegmentsRoundTripWithAShortLastOne) {
		recipe const rec = wide_recipe(2 * recipe_segment_slots + 5);
		auto const records = commit_ok(rec);
		ASSERT_EQ(records.size(), 3u);
		EXPECT_EQ(records[0].byte_length, 24u + recipe_segment_slots * 16u + 8u);
		EXPECT_EQ(records[1].byte_length, records[0].byte_length);
		EXPECT_EQ(records[2].byte_length, 24u + 5u * 16u + 8u);
		EXPECT_EQ(block_files(), 3u);

		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries, rec.entries);
		ASSERT_EQ(loaded->segments.size(), 3u);
		EXPECT_EQ(loaded->segments[2].digest, records[2].digest);
	}

	TEST_F(RecipeStore, AnAllEmptySegmentPublishesNoFile) {
		recipe const rec = wide_recipe(3 * recipe_segment_slots, [](size_t slot) {
			return slot / recipe_segment_slots == 1;  // the middle segment
		});
		auto const records = commit_ok(rec);
		ASSERT_EQ(records.size(), 3u);
		EXPECT_EQ(records[0].encoding, segment_encoding::raw);
		EXPECT_EQ(records[1].encoding, segment_encoding::all_empty);
		EXPECT_EQ(records[1].byte_length, 0u);
		EXPECT_EQ(records[1].digest.size, 0u);
		EXPECT_EQ(records[2].encoding, segment_encoding::raw);
		EXPECT_EQ(block_files(), 2u);

		auto loaded = recipe::load(dir.path, *store);
		ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
		EXPECT_EQ(loaded->entries, rec.entries);
		EXPECT_EQ(loaded->segments[1].encoding, segment_encoding::all_empty);
	}

	// An unchanged segment dedups against the file the last commit published,
	// so a commit that changes one entry writes one segment file.
	TEST_F(RecipeStore, AnUnchangedSegmentKeepsItsFile) {
		recipe rec = wide_recipe(2 * recipe_segment_slots);
		auto const first = commit_ok(rec);
		ASSERT_EQ(first.size(), 2u);
		ASSERT_EQ(block_files(), 2u);

		rec.entries[0] = digest_of("something else");
		auto const second = commit_ok(rec);
		EXPECT_NE(second[0].digest, first[0].digest);
		EXPECT_EQ(second[1].digest, first[1].digest);
		EXPECT_EQ(block_files(), 3u);  // the tail segment was not rewritten
	}

	TEST_F(RecipeStore, ManifestDamageIsDetected) {
		commit_ok(sample_recipe());
		auto const pristine = read_file(manifest_path());
		ASSERT_EQ(pristine.size(), 80u + 24u);

		auto const check = [&](char const *what, errc expected,
							   std::function<void(std::vector<std::byte> &)> const &patch) {
			auto bytes = pristine;
			patch(bytes);
			write_file(manifest_path(), bytes);
			auto const loaded = recipe::load(dir.path, *store);
			ASSERT_FALSE(loaded.has_value()) << what;
			EXPECT_EQ(loaded.error().code, expected) << what;
		};

		check("bad magic", errc::recipe_corrupt, [](auto &b) { b[0] = std::byte{'X'}; });
		check("bad header checksum", errc::recipe_corrupt, [](auto &b) { b[20] ^= std::byte{0x01}; });
		check("version 3", errc::recipe_unsupported, [](auto &b) {
			b[8] = std::byte{3};
			fix_manifest_checksums(b);
		});
		check("unknown algorithm", errc::recipe_unsupported, [](auto &b) {
			b[12] = std::byte{200};
			fix_manifest_checksums(b);
		});
		check("wrong entry width", errc::recipe_corrupt, [](auto &b) {
			b[13] = std::byte{32};
			fix_manifest_checksums(b);
		});
		check("zero segment log2", errc::recipe_corrupt, [](auto &b) {
			b[14] = std::byte{0};
			fix_manifest_checksums(b);
		});
		check("segment log2 too large", errc::recipe_corrupt, [](auto &b) {
			b[14] = std::byte{25};
			fix_manifest_checksums(b);
		});
		check("inconsistent header fields", errc::recipe_corrupt, [](auto &b) {
			store_le64(b.data() + 40, 7);  // slot count no longer matches size
			fix_manifest_checksums(b);
		});
		check("segment count off by one", errc::recipe_corrupt, [](auto &b) {
			store_le64(b.data() + 56, 2);
			fix_manifest_checksums(b);
		});
		check("dictionary chunks", errc::recipe_unsupported, [](auto &b) {
			store_le64(b.data() + 64, 1);
			fix_manifest_checksums(b);
		});
		check("truncated header", errc::recipe_corrupt, [](auto &b) { b.resize(40); });
		check("truncated record table", errc::recipe_corrupt, [](auto &b) { b.resize(b.size() - 8); });
		check("bad table checksum", errc::recipe_corrupt,
			  [](auto &b) { b[b.size() - 1] ^= std::byte{0x01}; });
		check("unknown segment encoding", errc::recipe_unsupported, [](auto &b) {
			b[72] = std::byte{7};
			fix_manifest_checksums(b);
		});
		check("record flags set", errc::recipe_corrupt, [](auto &b) {
			b[73] = std::byte{1};
			fix_manifest_checksums(b);
		});
		check("an empty record naming a file", errc::recipe_corrupt, [](auto &b) {
			b[72] = std::byte{0};  // all_empty, but length and digest stay
			fix_manifest_checksums(b);
		});
		check("a raw record naming no file", errc::recipe_corrupt, [](auto &b) {
			std::fill(b.begin() + 80, b.begin() + 96, std::byte{0});
			fix_manifest_checksums(b);
		});

		// and the pristine manifest still loads
		write_file(manifest_path(), pristine);
		EXPECT_TRUE(recipe::load(dir.path, *store).has_value());
	}

	TEST_F(RecipeStore, SegmentFileDamageIsDetected) {
		auto const records = commit_ok(sample_recipe());
		ASSERT_EQ(records.size(), 1u);
		fs::path const path = store->block_path(records[0].digest);
		auto const pristine = read_file(path);

		fs::remove(path);
		auto const absent = recipe::load(dir.path, *store);
		ASSERT_FALSE(absent.has_value());
		EXPECT_EQ(absent.error().code, errc::datastore_inconsistent);

		auto const check = [&](char const *what, errc expected,
							   std::function<void(std::vector<std::byte> &)> const &patch) {
			auto bytes = pristine;
			patch(bytes);
			write_file(path, bytes);
			auto const loaded = recipe::load(dir.path, *store);
			ASSERT_FALSE(loaded.has_value()) << what;
			EXPECT_EQ(loaded.error().code, expected) << what;
		};

		check("length disagrees with the record", errc::recipe_corrupt,
			  [](auto &b) { b.push_back(std::byte{0}); });
		check("bad magic", errc::recipe_corrupt, [](auto &b) { b[0] = std::byte{'X'}; });
		check("wrong encoding id", errc::recipe_corrupt, [](auto &b) { b[4] = std::byte{0}; });
		check("wrong base slot", errc::recipe_corrupt, [](auto &b) { store_le64(b.data() + 8, 1); });
		check("wrong entry count", errc::recipe_corrupt, [](auto &b) { store_le64(b.data() + 16, 2); });
		check("bad trailer checksum", errc::recipe_corrupt,
			  [](auto &b) { b[b.size() - 1] ^= std::byte{0x01}; });
		// only the content digest catches this one: the trailer checksum is
		// restamped over the flipped entry byte
		check("content that does not hash to the record digest", errc::recipe_corrupt, [](auto &b) {
			b[24] ^= std::byte{0x01};
			fix_segment_checksum(b);
		});

		write_file(path, pristine);
		EXPECT_TRUE(recipe::load(dir.path, *store).has_value());
	}

	TEST_F(RecipeStore, ValidateBlocksChecksExistenceAndSize) {
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
