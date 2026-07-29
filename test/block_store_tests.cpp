// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Tests for the content-addressed block store: the shard skeleton,
// atomic publication with dedup and the hash-collision check, the
// durable-name set, reference bookkeeping feeding reclaim, and the
// open-time sweep.

#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/file_util.hpp>

#include "support/temp_dir.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace privateer;
namespace fs = std::filesystem;

namespace {

	constexpr hash_algorithm test_alg = hash_algorithm::xxh3_128;

	std::vector<std::byte> content(std::string const &text) {
		auto const *bytes = reinterpret_cast<std::byte const *>(text.data());
		return {bytes, bytes + text.size()};
	}

	block_digest name_of(std::vector<std::byte> const &data) {
		return hash_block(test_alg, data);
	}

	std::string read_file(fs::path const &path) {
		std::ifstream in{path, std::ios::binary};
		return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
	}

	size_t file_count(fs::path const &blocks_dir) {
		size_t count = 0;
		for (auto const &shard : fs::directory_iterator{blocks_dir}) {
			for ([[maybe_unused]] auto const &entry : fs::directory_iterator{shard}) {
				++count;
			}
		}
		return count;
	}

	struct store_fixture {
		privateer::testing::temp_dir dir;
		block_store store;

		store_fixture() : store{[&] {
			auto created = block_store::create(dir.path);
			if (!created) {
				throw std::runtime_error{to_string(created.error())};
			}
			return std::move(*created);
		}()} {}

		[[nodiscard]] fs::path blocks_dir() const { return dir.path / "blocks"; }
	};

	TEST(BlockStore, CreateBuildsTheShardSkeleton) {
		store_fixture f;
		size_t shards = 0;
		for (auto const &entry : fs::directory_iterator{f.blocks_dir()}) {
			EXPECT_TRUE(entry.is_directory());
			++shards;
		}
		EXPECT_EQ(shards, 256u);
	}

	TEST(BlockStore, CreateRefusesAnExistingStore) {
		store_fixture f;
		auto again = block_store::create(f.dir.path);
		ASSERT_FALSE(again.has_value());
		EXPECT_EQ(again.error().code, errc::datastore_exists);
	}

	TEST(BlockStore, OpenNeedsTheFullSkeleton) {
		privateer::testing::temp_dir dir;
		auto missing = block_store::open(dir.path);
		ASSERT_FALSE(missing.has_value());
		EXPECT_EQ(missing.error().code, errc::datastore_missing);

		ASSERT_TRUE(block_store::create(dir.path));
		ASSERT_TRUE(block_store::open(dir.path));

		fs::remove(dir.path / "blocks" / "7f");
		auto broken = block_store::open(dir.path);
		ASSERT_FALSE(broken.has_value());
		EXPECT_EQ(broken.error().code, errc::datastore_inconsistent);
	}

	TEST(BlockStore, PublishCreatesTheBlockFile) {
		store_fixture f;
		auto const data = content("some block content");
		auto const name = name_of(data);

		auto published = f.store.publish(name, data);
		ASSERT_TRUE(published.has_value()) << to_string(published.error());
		EXPECT_TRUE(*published);

		fs::path const path = f.store.block_path(name);
		EXPECT_EQ(path.parent_path().filename().string(), to_hex(name).substr(0, 2));
		EXPECT_EQ(read_file(path), "some block content");
	}

	TEST(BlockStore, PublishDedupsIdenticalContent) {
		store_fixture f;
		auto const data = content("identical content");
		auto const name = name_of(data);

		ASSERT_TRUE(f.store.publish(name, data));
		auto again = f.store.publish(name, data);
		ASSERT_TRUE(again.has_value()) << to_string(again.error());
		EXPECT_FALSE(*again);
		EXPECT_EQ(file_count(f.blocks_dir()), 1u);
	}

	TEST(BlockStore, PublishWritesNothingForADuplicate) {
		store_fixture f;
		auto const data = content(std::string(1 << 16, 'q'));
		auto const name = name_of(data);

		detail_file_util::staged_files.store(0);
		ASSERT_TRUE(f.store.publish(name, data));
		EXPECT_EQ(detail_file_util::staged_files.load(), 1u);

		// the name has a file, so the compare answers the publication
		detail_file_util::staged_files.store(0);
		auto again = f.store.publish(name, data);
		ASSERT_TRUE(again.has_value()) << to_string(again.error());
		EXPECT_FALSE(*again);
		EXPECT_EQ(detail_file_util::staged_files.load(), 0u);
		EXPECT_EQ(read_file(f.store.block_path(name)).size(), data.size());
	}

	TEST(BlockStore, PublishWritesNothingForACollision) {
		store_fixture f;
		auto const data = content("first content");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));

		detail_file_util::staged_files.store(0);
		auto collided = f.store.publish(name, content("other content"));
		ASSERT_FALSE(collided.has_value());
		EXPECT_EQ(collided.error().code, errc::hash_collision);
		EXPECT_EQ(detail_file_util::staged_files.load(), 0u);
	}

	TEST(BlockStore, PublishSurvivesConcurrentUnlinks) {
		store_fixture f;
		auto const data = content(std::string(1 << 14, 'u'));
		auto const name = name_of(data);
		fs::path const path = f.store.block_path(name);

		// A name that keeps disappearing under the publisher: every round is
		// either a compare against the file or a fresh write, and neither
		// leaves the store with a temp file or an error.
		std::atomic<bool> stop{false};
		std::thread unlinker{[&] {
			while (!stop.load()) {
				::unlink(path.c_str());
				std::this_thread::yield();
			}
		}};
		int created = 0;
		for (int round = 0; round < 500; ++round) {
			auto published = f.store.publish(name, data);
			if (!published) {
				stop.store(true);
				unlinker.join();
				FAIL() << to_string(published.error());
			}
			created += *published ? 1 : 0;
		}
		stop.store(true);
		unlinker.join();

		EXPECT_GT(created, 0);
		for (auto const &shard : fs::directory_iterator{f.blocks_dir()}) {
			for (auto const &entry : fs::directory_iterator{shard}) {
				EXPECT_FALSE(entry.path().filename().string().starts_with(temp_name_prefix));
			}
		}
	}

	TEST(BlockStore, PublishGivesUpOnANameThatCannotBeOpened) {
		store_fixture f;
		auto const data = content("content behind a broken name");
		auto const name = name_of(data);
		// A dangling symlink is a name whose entry refuses the link and whose
		// file cannot be opened, so no round of publish can resolve it.
		fs::create_symlink("nowhere", f.store.block_path(name));

		auto published = f.store.publish(name, data);
		ASSERT_FALSE(published.has_value());
		EXPECT_EQ(published.error().code, errc::io_error);
	}

	TEST(BlockStore, PublishDetectsAHashCollision) {
		store_fixture f;
		auto const data = content("first content");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));

		auto collided = f.store.publish(name, content("other content"));
		ASSERT_FALSE(collided.has_value());
		EXPECT_EQ(collided.error().code, errc::hash_collision);

		auto same_size = f.store.publish(name, content("othercontent!"));
		ASSERT_FALSE(same_size.has_value());
		EXPECT_EQ(same_size.error().code, errc::hash_collision);

		EXPECT_EQ(read_file(f.store.block_path(name)), "first content");
	}

	TEST(BlockStore, PublishRejectsEmptyInput) {
		store_fixture f;
		auto const data = content("x");
		EXPECT_EQ(f.store.publish(block_digest{}, data).error().code, errc::invalid_argument);
		EXPECT_EQ(f.store.publish(name_of(data), {}).error().code, errc::invalid_argument);
	}

	TEST(BlockStore, ConcurrentPublishOfOneNameYieldsOneFile) {
		store_fixture f;
		auto const data = content(std::string(1 << 16, 'z'));
		auto const name = name_of(data);

		constexpr int publishers = 8;
		std::vector<std::thread> threads;
		std::atomic<int> created{0};
		std::atomic<int> failed{0};
		for (int i = 0; i < publishers; ++i) {
			threads.emplace_back([&] {
				auto published = f.store.publish(name, data);
				if (!published.has_value()) {
					failed.fetch_add(1);
				} else if (*published) {
					created.fetch_add(1);
				}
			});
		}
		for (auto &t : threads) {
			t.join();
		}
		EXPECT_EQ(failed.load(), 0);
		EXPECT_EQ(created.load(), 1);
		EXPECT_EQ(file_count(f.blocks_dir()), 1u);
		EXPECT_EQ(read_file(f.store.block_path(name)).size(), data.size());
	}

	TEST(BlockStore, MakeDurableRecordsTheNames) {
		store_fixture f;
		auto const data = content("durable content");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));
		EXPECT_FALSE(f.store.is_durable(name));

		block_digest const names[] = {name};
		ASSERT_TRUE(f.store.make_durable(names));
		EXPECT_TRUE(f.store.is_durable(name));
		ASSERT_TRUE(f.store.make_durable(names));
	}

	TEST(BlockStore, MakeDurableFailsOnAMissingBlock) {
		store_fixture f;
		block_digest const names[] = {name_of(content("never published"))};
		auto barrier = f.store.make_durable(names);
		ASSERT_FALSE(barrier.has_value());
		EXPECT_EQ(barrier.error().code, errc::io_error);
		EXPECT_FALSE(f.store.is_durable(names[0]));
	}

	// A fan-out that runs every index on its own thread, so the store's own
	// bookkeeping is the only thing left on the calling thread, and counts the
	// indices it was given.
	struct counting_fan_out {
		std::atomic<size_t> indices{0};
		std::atomic<size_t> calls{0};

		block_store::sync_fan_out get() {
			return [this](size_t count, std::function<void(size_t)> const &body) {
				calls.fetch_add(1);
				indices.fetch_add(count);
				std::vector<std::thread> threads;
				threads.reserve(count);
				for (size_t index = 0; index < count; ++index) {
					threads.emplace_back([&body, index] { body(index); });
				}
				for (auto &thread : threads) {
					thread.join();
				}
			};
		}
	};

	// eight distinct blocks, published but not durable
	std::vector<block_digest> publish_eight(store_fixture &f) {
		std::vector<block_digest> names;
		for (int i = 0; i < 8; ++i) {
			auto const data = content("spread block " + std::to_string(i));
			auto const name = name_of(data);
			EXPECT_TRUE(f.store.publish(name, data).has_value());
			names.push_back(name);
		}
		return names;
	}

	size_t shard_count_of(std::vector<block_digest> const &names) {
		std::set<uint8_t> shards;
		for (auto const &name : names) {
			shards.insert(static_cast<uint8_t>(name.bytes[0]));
		}
		return shards.size();
	}

	TEST(BlockStore, MakeDurableSpreadsOverTheFanOut) {
		store_fixture f;
		auto const names = publish_eight(f);
		counting_fan_out fan_out;
		detail_file_util::sync_calls.store(0);

		auto barrier = f.store.make_durable(names, fan_out.get());
		ASSERT_TRUE(barrier.has_value()) << to_string(barrier.error());
		for (auto const &name : names) {
			EXPECT_TRUE(f.store.is_durable(name));
		}
		// one index per block file and one per shard that got an entry, and
		// exactly that many syncs
		size_t const expected = names.size() + shard_count_of(names);
		EXPECT_EQ(fan_out.calls.load(), 1u);
		EXPECT_EQ(fan_out.indices.load(), expected);
		EXPECT_EQ(detail_file_util::sync_calls.load(), expected);
	}

	TEST(BlockStore, MakeDurableSyncsADuplicateNameOnce) {
		store_fixture f;
		auto const data = content("named once");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));
		// a dedup hit puts one name in several recipe entries
		std::vector<block_digest> const names(6, name);
		counting_fan_out fan_out;

		ASSERT_TRUE(f.store.make_durable(names, fan_out.get()));
		EXPECT_TRUE(f.store.is_durable(name));
		// one file, one shard, and no spread for a batch that small
		EXPECT_EQ(fan_out.calls.load(), 0u);
	}

	TEST(BlockStore, MakeDurableThroughTheFanOutRecordsNothingOnAFailure) {
		store_fixture f;
		auto names = publish_eight(f);
		names.push_back(name_of(content("never published")));
		counting_fan_out fan_out;

		auto barrier = f.store.make_durable(names, fan_out.get());
		ASSERT_FALSE(barrier.has_value());
		EXPECT_EQ(barrier.error().code, errc::io_error);
		for (auto const &name : names) {
			EXPECT_FALSE(f.store.is_durable(name));
		}
	}

	TEST(BlockStore, ReclaimSpreadsByShard) {
		store_fixture f;
		auto const names = publish_eight(f);
		for (auto const &name : names) {
			f.store.add_reference(name);
			f.store.drop_reference(name);
		}
		counting_fan_out fan_out;

		f.store.reclaim(fan_out.get());
		for (auto const &name : names) {
			EXPECT_FALSE(fs::exists(f.store.block_path(name)));
			EXPECT_FALSE(f.store.is_durable(name));
		}
		EXPECT_EQ(file_count(f.blocks_dir()), 0u);
		// one index per shard, and the candidates are gone, so a second pass
		// has nothing to spread
		EXPECT_EQ(fan_out.indices.load(), shard_count_of(names));
		f.store.reclaim(fan_out.get());
		EXPECT_EQ(fan_out.calls.load(), 1u);
	}

	TEST(BlockStore, SeedDurableSkipsTheSyncs) {
		store_fixture f;
		auto const name = name_of(content("seeded"));
		f.store.seed_durable(name);
		EXPECT_TRUE(f.store.is_durable(name));
	}

	TEST(BlockStore, ReferencesGateReclaim) {
		store_fixture f;
		auto const data = content("reclaimable");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));

		f.store.add_reference(name);
		f.store.add_reference(name);
		f.store.drop_reference(name);
		f.store.reclaim();
		EXPECT_TRUE(fs::exists(f.store.block_path(name)));
		EXPECT_TRUE(f.store.referenced(name));

		f.store.drop_reference(name);
		EXPECT_FALSE(f.store.referenced(name));
		f.store.reclaim();
		EXPECT_FALSE(fs::exists(f.store.block_path(name)));
	}

	TEST(BlockStore, ReAddedReferenceWithdrawsTheCandidate) {
		store_fixture f;
		auto const data = content("kept alive");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));

		f.store.add_reference(name);
		f.store.drop_reference(name);
		f.store.add_reference(name);
		f.store.reclaim();
		EXPECT_TRUE(fs::exists(f.store.block_path(name)));
	}

	TEST(BlockStore, ReclaimDropsTheDurableEntry) {
		store_fixture f;
		auto const data = content("durable then gone");
		auto const name = name_of(data);
		ASSERT_TRUE(f.store.publish(name, data));
		block_digest const names[] = {name};
		ASSERT_TRUE(f.store.make_durable(names));

		f.store.add_reference(name);
		f.store.drop_reference(name);
		f.store.reclaim();
		EXPECT_FALSE(fs::exists(f.store.block_path(name)));
		EXPECT_FALSE(f.store.is_durable(name));
	}

	TEST(BlockStore, SweepRemovesExactlyTheGarbage) {
		store_fixture f;
		auto const kept_a = content("referenced block a");
		auto const kept_b = content("referenced block b");
		auto const garbage = content("unreferenced block");
		ASSERT_TRUE(f.store.publish(name_of(kept_a), kept_a));
		ASSERT_TRUE(f.store.publish(name_of(kept_b), kept_b));
		ASSERT_TRUE(f.store.publish(name_of(garbage), garbage));

		fs::path const temp_leftover = f.blocks_dir() / "00" / (std::string{temp_name_prefix} + "abc123");
		std::ofstream{temp_leftover} << "temp litter";
		fs::path const stray = f.blocks_dir() / "42" / "not-a-block";
		std::ofstream{stray} << "stray litter";

		block_digest const referenced[] = {name_of(kept_a), name_of(kept_b)};
		auto removed = f.store.sweep(referenced);
		ASSERT_TRUE(removed.has_value()) << to_string(removed.error());
		EXPECT_EQ(*removed, 3u);

		EXPECT_TRUE(fs::exists(f.store.block_path(name_of(kept_a))));
		EXPECT_TRUE(fs::exists(f.store.block_path(name_of(kept_b))));
		EXPECT_FALSE(fs::exists(f.store.block_path(name_of(garbage))));
		EXPECT_FALSE(fs::exists(temp_leftover));
		EXPECT_FALSE(fs::exists(stray));
		EXPECT_EQ(file_count(f.blocks_dir()), 2u);
	}

}  // namespace
