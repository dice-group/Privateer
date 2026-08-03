// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_TEST_STORE_BUILDER_HPP
#define PRIVATEER_TEST_STORE_BUILDER_HPP

// Builds a committed datastore through the storage layer, without a region:
// one recipe entry per element, a fill character for a block file or
// nullopt for the empty sentinel.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>

#include <cstddef>
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
