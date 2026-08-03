// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Crash tests for the storage-level commit sequence over the block store
// and the recipe: publish the changed blocks, run the durability barrier,
// publish the recipe (its segment files, then the manifest rename), reclaim
// the retired names. A child process runs the sequence and is killed at
// each phase boundary, for a durable and a non-durable commit. Properties
// checked on the survivor side: the recipe loads and matches the last
// commit that reached the rename, every block it references exists intact,
// and the open-time sweep removes exactly the garbage, never a block or a
// segment file the recipe references.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/recipe.hpp>

#include "support/sandbox.hpp"
#include "support/temp_dir.hpp"

#include <csignal>
#include <cstddef>
#include <filesystem>
#include <vector>

#include <boost/unordered/unordered_flat_set.hpp>

using namespace privateer;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	constexpr uint64_t test_block_size = 4096;

	std::vector<std::byte> block_content(char fill) {
		return std::vector<std::byte>(test_block_size, static_cast<std::byte>(fill));
	}

	recipe make_recipe(std::vector<block_digest> entries) {
		recipe rec;
		rec.block_size = test_block_size;
		rec.capacity = 64 * test_block_size;
		rec.size = entries.size() * test_block_size;
		rec.algorithm = hash_algorithm::xxh3_128;
		rec.entries = std::move(entries);
		return rec;
	}

	// The phases of the second, killable commit. Slot 1 changes from block
	// b to block c; slot 0 keeps block a.
	enum struct phase : int {
		publish = 1,        // write-out of the changed block
		barrier = 2,        // durability barrier (durable commits only)
		recipe_rename = 3,  // the recipe commit: its segment files, then the manifest rename
		reclaim = 4,        // unlink of the retired names (durable commits only)
	};

	struct CommitCrashTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		std::vector<std::byte> const block_a = block_content('a');
		std::vector<std::byte> const block_b = block_content('b');
		std::vector<std::byte> const block_c = block_content('c');
		block_digest const name_a = hash_block(hash_algorithm::xxh3_128, block_a);
		block_digest const name_b = hash_block(hash_algorithm::xxh3_128, block_b);
		block_digest const name_c = hash_block(hash_algorithm::xxh3_128, block_c);

		// the first, durable commit: slots [a, b]
		void SetUp() override {
			auto store = block_store::create(dir.path);
			ASSERT_TRUE(store.has_value()) << to_string(store.error());
			ASSERT_TRUE(store->publish(name_a, block_a));
			ASSERT_TRUE(store->publish(name_b, block_b));
			block_digest const names[] = {name_a, name_b};
			ASSERT_TRUE(store->make_durable(names));
			ASSERT_TRUE(make_recipe({name_a, name_b}).commit(dir.path, *store, true).has_value());
		}

		// Runs the second commit through all phases up to last, like a
		// reopened process would: open the store and seed the durable set
		// and the references from the committed recipe first, blocks and
		// segment files alike. Returns 0 when last was reached, a distinct
		// code on any failure before it.
		int run_second_commit(phase last, bool durable) {
			auto store = block_store::open(dir.path);
			if (!store) {
				return 10;
			}
			auto loaded = recipe::load(dir.path, *store);
			if (!loaded) {
				return 14;
			}
			for (auto const &entry : loaded->entries) {
				store->seed_durable(entry);
				store->add_reference(entry);
			}
			for (auto const &segment : loaded->segments) {
				store->seed_durable(segment.digest);
				store->add_reference(segment.digest);
			}

			if (!store->publish(name_c, block_c)) {
				return 11;
			}
			if (last == phase::publish) {
				return 0;
			}
			if (durable) {
				block_digest const referenced[] = {name_a, name_c};
				if (!store->make_durable(referenced)) {
					return 12;
				}
			}
			if (last == phase::barrier) {
				return 0;
			}
			auto published = make_recipe({name_a, name_c}).commit(dir.path, *store, durable);
			if (!published) {
				return 13;
			}
			if (last == phase::recipe_rename) {
				return 0;
			}
			if (durable) {
				store->add_reference(name_c);
				for (auto const &segment : *published) {
					store->add_reference(segment.digest);
				}
				store->drop_reference(name_b);
				for (auto const &segment : loaded->segments) {
					store->drop_reference(segment.digest);
				}
				store->reclaim();
			}
			return 0;
		}

		[[nodiscard]] size_t file_count() const {
			size_t count = 0;
			for (auto const &shard : fs::directory_iterator{dir.path / "blocks"}) {
				for ([[maybe_unused]] auto const &entry : fs::directory_iterator{shard}) {
					++count;
				}
			}
			return count;
		}

		// the reopen properties after the child died
		void check_state(std::vector<block_digest> const &expected_entries, size_t expected_swept) {
			auto store = block_store::open(dir.path);
			ASSERT_TRUE(store.has_value()) << to_string(store.error());
			auto loaded = recipe::load(dir.path, *store);
			ASSERT_TRUE(loaded.has_value()) << to_string(loaded.error());
			ASSERT_EQ(loaded->entries, expected_entries);
			ASSERT_TRUE(validate_blocks(*loaded, *store));

			boost::unordered_flat_set<block_digest, block_digest_hash> unique;
			std::vector<block_digest> referenced;
			for (auto const &entry : loaded->entries) {
				if (entry.size != 0 && unique.insert(entry).second) {
					referenced.push_back(entry);
				}
			}
			size_t segment_files = 0;
			for (auto const &segment : loaded->segments) {
				if (segment.digest.size != 0) {
					referenced.push_back(segment.digest);
					++segment_files;
				}
			}

			auto const removed = store->sweep(referenced);
			ASSERT_TRUE(removed.has_value()) << to_string(removed.error());
			EXPECT_EQ(*removed, expected_swept);
			EXPECT_TRUE(validate_blocks(*loaded, *store));
			// the recipe loads again, so the sweep kept its segment files
			EXPECT_TRUE(recipe::load(dir.path, *store).has_value());
			EXPECT_EQ(file_count(), unique.size() + segment_files);
		}

		void run_killed_at(phase last, bool durable) {
			auto const res = PRIVATEER_SANDBOX {
				if (int const code = run_second_commit(last, durable); code != 0) {
					return code;
				}
				::raise(SIGKILL);
				return 0;
			};
			ASSERT_EQ(res, subprocess_result::killed);
		}
	};

	TEST_F(CommitCrashTest, DurableKilledAfterPublishKeepsTheOldRecipe) {
		run_killed_at(phase::publish, true);
		check_state({name_a, name_b}, 1);  // the swept garbage is block c
	}

	TEST_F(CommitCrashTest, DurableKilledAfterTheBarrierKeepsTheOldRecipe) {
		run_killed_at(phase::barrier, true);
		check_state({name_a, name_b}, 1);  // block c is durable but unreferenced
	}

	TEST_F(CommitCrashTest, DurableKilledAfterTheRecipeRenameShowsTheNewRecipe) {
		run_killed_at(phase::recipe_rename, true);
		// block b and the segment file of the old recipe are retired but not
		// yet reclaimed
		check_state({name_a, name_c}, 2);
	}

	TEST_F(CommitCrashTest, DurableKilledAfterReclaimLeavesNoGarbage) {
		run_killed_at(phase::reclaim, true);
		check_state({name_a, name_c}, 0);
	}

	TEST_F(CommitCrashTest, NonDurableKilledAfterPublishKeepsTheOldRecipe) {
		run_killed_at(phase::publish, false);
		check_state({name_a, name_b}, 1);
	}

	TEST_F(CommitCrashTest, NonDurableKilledAfterTheRecipeRenameShowsTheNewRecipe) {
		// a non-durable commit never unlinks, so the retired block b and the
		// old recipe's segment file survive as sweepable garbage; the
		// published files are all present because a process kill loses no
		// page cache
		run_killed_at(phase::recipe_rename, false);
		check_state({name_a, name_c}, 2);
	}

	TEST_F(CommitCrashTest, TheFullSequenceCompletesCleanly) {
		ASSERT_EQ(run_second_commit(phase::reclaim, true), 0);
		check_state({name_a, name_c}, 0);
	}

}  // namespace
