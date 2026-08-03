// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_TEST_STORE_BUILDER_HPP
#define PRIVATEER_TEST_STORE_BUILDER_HPP

// Builds a committed datastore through the storage layer, without a region:
// one recipe entry per element, a fill character for a block file or
// nullopt for the empty sentinel. Also the readers over the raw bytes of a
// committed datastore: the file counts of the store and the header and
// segment records of the manifest.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <vector>

namespace privateer::testing {

	inline void build_committed_store(std::filesystem::path const &segment_dir, uint64_t block_size,
									  std::vector<std::optional<char>> const &slots,
									  uint64_t capacity_slots = 16) {
		std::filesystem::create_directories(segment_dir);
		auto store = block_store::create(segment_dir);
		ASSERT_TRUE(store.has_value()) << to_string(store.error());
		recipe rec;
		rec.block_size = block_size;
		rec.capacity = capacity_slots * block_size;
		rec.size = slots.size() * block_size;
		rec.algorithm = hash_algorithm::xxh3_128;
		std::vector<block_digest> names;
		for (auto const &fill : slots) {
			if (!fill) {
				rec.entries.emplace_back();
				continue;
			}
			std::vector<std::byte> const data(block_size, static_cast<std::byte>(*fill));
			auto const name = hash_block(rec.algorithm, data);
			ASSERT_TRUE(store->publish(name, data));
			names.push_back(name);
			rec.entries.push_back(name);
		}
		auto barrier = store->make_durable(names);
		ASSERT_TRUE(barrier.has_value()) << to_string(barrier.error());
		ASSERT_TRUE(rec.commit(segment_dir, *store, true).has_value());
	}

	// The same store with a frozen version 1 recipe in the manifest slot: the
	// whole entry table in one file and no segment files anywhere. The read
	// path a datastore an older build wrote takes.
	inline void build_legacy_store(std::filesystem::path const &segment_dir, uint64_t block_size,
								   std::vector<std::optional<char>> const &slots,
								   uint64_t capacity_slots = 16) {
		std::filesystem::create_directories(segment_dir);
		auto store = block_store::create(segment_dir);
		ASSERT_TRUE(store.has_value()) << to_string(store.error());
		recipe rec;
		rec.block_size = block_size;
		rec.capacity = capacity_slots * block_size;
		rec.size = slots.size() * block_size;
		rec.algorithm = hash_algorithm::xxh3_128;
		std::vector<block_digest> names;
		for (auto const &fill : slots) {
			if (!fill) {
				rec.entries.emplace_back();
				continue;
			}
			std::vector<std::byte> const data(block_size, static_cast<std::byte>(*fill));
			auto const name = hash_block(rec.algorithm, data);
			ASSERT_TRUE(store->publish(name, data));
			names.push_back(name);
			rec.entries.push_back(name);
		}
		auto barrier = store->make_durable(names);
		ASSERT_TRUE(barrier.has_value()) << to_string(barrier.error());
		auto const bytes = rec.serialize();
		ASSERT_TRUE(bytes.has_value()) << to_string(bytes.error());
		std::ofstream out{segment_dir / recipe_file_name, std::ios::binary | std::ios::trunc};
		out.write(reinterpret_cast<char const *>(bytes->data()),
				  static_cast<std::streamsize>(bytes->size()));
		out.close();
		ASSERT_TRUE(out.good());
	}

	// whole content of the manifest file
	[[nodiscard]] inline std::vector<unsigned char> read_manifest(
			std::filesystem::path const &segment_dir) {
		std::ifstream in{segment_dir / recipe_file_name, std::ios::binary | std::ios::ate};
		auto const size = static_cast<size_t>(in.tellg());
		in.seekg(0);
		std::vector<unsigned char> bytes(size);
		in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
		return bytes;
	}

	// The manifest layout the tests read: a 72 byte header, then one 24 byte
	// segment record per segment, then the table checksum.
	inline constexpr size_t manifest_header_bytes = 72;
	inline constexpr size_t manifest_record_bytes = 24;

	// the format version of the manifest header
	[[nodiscard]] inline uint32_t manifest_version(std::filesystem::path const &segment_dir) {
		auto const bytes = read_manifest(segment_dir);
		uint32_t version = 0;
		for (int i = 0; i < 4; ++i) {
			version |= static_cast<uint32_t>(bytes.at(8 + static_cast<size_t>(i))) << (8 * i);
		}
		return version;
	}

	// the segment records of the manifest, in slot order
	[[nodiscard]] inline std::vector<std::vector<unsigned char>> manifest_records(
			std::filesystem::path const &segment_dir) {
		auto const bytes = read_manifest(segment_dir);
		std::vector<std::vector<unsigned char>> records;
		if (bytes.size() < manifest_header_bytes + sizeof(uint64_t)) {
			return records;
		}
		size_t const count = (bytes.size() - manifest_header_bytes - sizeof(uint64_t)) /
							 manifest_record_bytes;
		for (size_t index = 0; index < count; ++index) {
			auto const *const at = bytes.data() + manifest_header_bytes + index * manifest_record_bytes;
			records.emplace_back(at, at + manifest_record_bytes);
		}
		return records;
	}

	// number of files under <segment_dir>/blocks, across all shards
	[[nodiscard]] inline size_t count_block_files(std::filesystem::path const &segment_dir) {
		size_t count = 0;
		for (auto const &shard : std::filesystem::directory_iterator{segment_dir / "blocks"}) {
			for ([[maybe_unused]] auto const &entry : std::filesystem::directory_iterator{shard}) {
				++count;
			}
		}
		return count;
	}

	// True for a recipe segment file, told from a data block by its magic.
	// The store holds both under the same naming scheme.
	[[nodiscard]] inline bool is_segment_file(std::filesystem::path const &path) {
		std::ifstream in{path, std::ios::binary};
		char magic[4] = {};
		in.read(magic, sizeof(magic));
		return in.gcount() == sizeof(magic) && std::string_view{magic, sizeof(magic)} == "PVSG";
	}

	// the recipe segment files among the store's files
	[[nodiscard]] inline size_t count_segment_files(std::filesystem::path const &segment_dir) {
		size_t count = 0;
		for (auto const &shard : std::filesystem::directory_iterator{segment_dir / "blocks"}) {
			for (auto const &entry : std::filesystem::directory_iterator{shard}) {
				count += is_segment_file(entry.path()) ? 1 : 0;
			}
		}
		return count;
	}

	// the data blocks among the store's files: everything that is no recipe segment
	[[nodiscard]] inline size_t count_data_block_files(std::filesystem::path const &segment_dir) {
		return count_block_files(segment_dir) - count_segment_files(segment_dir);
	}

}  // namespace privateer::testing

#endif  // PRIVATEER_TEST_STORE_BUILDER_HPP
