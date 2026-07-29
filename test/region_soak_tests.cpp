// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// The combined long-running soak: many readers, one writer, a GC thread
// freeing slot ranges, and a committer running commit and snapshot cycles,
// with the cleaner and the governor enabled the whole time. The suite
// asserts the engine's correctness properties under sustained concurrency:
// no write is lost, every committed cut is per-slot consistent (never torn
// inside a slot), and no actor wedges (a fault loop or a lost wakeup hangs
// the test into its ctest timeout).
//
// Content protocol. The writer fills slots front to back with one 64-bit
// stamp per fill; a stamp encodes the slot and a process-unique sequence
// number. Every mutation completes under a per-slot writer lock, so a slot
// that no mutation touches always matches its expected image. Commit
// capture can cut a fill mid slot; the cut is a prefix of the newer fill
// over the older content, so a valid snapshot slot scans as stamp runs with
// strictly decreasing sequence numbers, with zeros allowed only as the
// tail. Anything else in a snapshot is a torn commit.
//
// TSan runs a restricted write pattern. TSan cannot see mprotect ordering:
// a store that does not fault has no happens-before edge to the cleaner's
// or capture's later freeze-and-read, and TSan models the write-back's
// MAP_FIXED remap as a plain write into the slot. A store that DOES fault
// is covered: its instrumentation records it before the handler's release
// publish on the state word, which every later claim of the slot acquires.
// Under TSan the writer therefore writes exactly one faulting word per
// fill, only into slots whose last mutation is two finished commits old
// (definitely clean again, so the store faults), and readers use the same
// two-commit grace period (a slot committed that long ago is stably clean,
// so no remap races the reads). All other builds run full-slot fills into
// any slot with no grace period; that unsynchronized interleaving is half
// the point of the soak.
//
// Runtime: PRIVATEER_SOAK_SECONDS (default 30) per test. The suite carries
// the ctest label long_running and does not run in CI.

#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// sanitizer detection: gcc defines __SANITIZE_*, clang answers __has_feature
#ifdef __SANITIZE_THREAD__
#define PRIVATEER_TEST_TSAN 1
#endif
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define PRIVATEER_TEST_TSAN 1
#endif
#endif

using namespace privateer;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

	constexpr size_t capacity_slots = 128;
	constexpr size_t initial_slots = 64;
	constexpr size_t extend_step_slots = 8;
	constexpr size_t reader_count = 4;

	std::chrono::seconds soak_duration() {
		if (char const *env = std::getenv("PRIVATEER_SOAK_SECONDS")) {
			return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(std::strtol(env, nullptr, 10))};
		}
		return 30s;
	}

	// stamp layout: slot + 1 in the high 16 bits, sequence below; never zero
	constexpr uint64_t make_stamp(size_t slot, uint64_t seq) {
		return (static_cast<uint64_t>(slot + 1) << 48) | (seq & ((1ull << 48) - 1));
	}
	constexpr uint64_t stamp_slot(uint64_t stamp) {
		return (stamp >> 48) - 1;
	}
	constexpr uint64_t stamp_seq(uint64_t stamp) {
		return stamp & ((1ull << 48) - 1);
	}

	// Validates one committed slot image against the content protocol: stamp
	// runs of this slot with strictly decreasing sequence numbers (a newer
	// fill cut over older content), zeros only as the tail. Returns a
	// diagnostic, empty when valid.
	std::string check_slot_image(uint64_t const *words, size_t nwords, size_t slot) {
		uint64_t prev_stamp = 0;
		bool zeros_seen = false;
		for (size_t w = 0; w < nwords; ++w) {
			uint64_t const word = words[w];
			if (word == 0) {
				zeros_seen = true;
				continue;
			}
			if (zeros_seen) {
				return "stamp after zeros at word " + std::to_string(w);
			}
			if (word != prev_stamp) {
				if (stamp_slot(word) != slot) {
					return "foreign stamp at word " + std::to_string(w) + ": slot " +
						   std::to_string(stamp_slot(word));
				}
				if (prev_stamp != 0 && stamp_seq(word) >= stamp_seq(prev_stamp)) {
					return "sequence rises at word " + std::to_string(w);
				}
				prev_stamp = word;
			}
		}
		return {};
	}

	struct RegionSoakTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();
		size_t const words_per_slot = bs / sizeof(uint64_t);
#ifdef PRIVATEER_TEST_TSAN
		size_t const fill_words = 1;
#else
		size_t const fill_words = words_per_slot;
#endif

		std::optional<region> reg;

		// test-side application locking: one writer lock per slot; readers
		// share it, every mutation holds it exclusively
		std::vector<std::shared_mutex> slot_locks{capacity_slots};

		// stamp of the last completed fill per slot, 0 for never written or
		// freed; guarded by the slot lock
		std::vector<uint64_t> expected = std::vector<uint64_t>(capacity_slots, 0);

		// commits_finished value at each slot's last mutation, guarded by
		// the slot lock; -2 makes untouched slots eligible from the start
		std::vector<int64_t> last_mutation_commit = std::vector<int64_t>(capacity_slots, -2);
		std::atomic<int64_t> commits_finished{0};

		std::atomic<uint64_t> next_seq{1};
		std::atomic<size_t> extended_slots{initial_slots};
		std::atomic<bool> stop_mutators{false};
		std::atomic<bool> stop_committer{false};
		std::atomic<bool> stop_readers{false};

		// activity counters; the test asserts every actor did real work
		std::atomic<uint64_t> fills{0}, frees{0}, commits{0}, snapshots{0}, reads{0}, extends{0};

		void SetUp() override {
			region_options opts;
			opts.block_size = bs;
			opts.cleaner.mode = cleaner_mode::eager_durable;
			opts.cleaner.interval = 20ms;
			opts.cleaner.batch_slots = 8;
			opts.cleaner.backoff_base = 5ms;
			opts.cleaner.backoff_cap = 200ms;
			opts.governor.dirty_soft = 16 * bs;
			opts.governor.dirty_low = 8 * bs;
			opts.governor.dirty_hard = 48 * bs;
			opts.governor.hard_timeout = 5s;
#ifdef __linux__
			opts.governor.resident_soft = 64ull * 1024 * 1024;
			opts.governor.resident_low = 48ull * 1024 * 1024;
			opts.governor.sweep_interval = 100ms;
#endif
			auto created = region::create(dir.path / "store", capacity_slots * bs, opts);
			ASSERT_TRUE(created.has_value()) << to_string(created.error());
			reg.emplace(std::move(*created));
			ASSERT_TRUE(reg->extend(initial_slots * bs));
		}

		[[nodiscard]] uint64_t volatile *slot_words(size_t slot) {
			return reinterpret_cast<uint64_t volatile *>(
					static_cast<unsigned char volatile *>(reg->segment()) + slot * bs);
		}

		// Two finished commits since the slot's last mutation: the first may
		// have been in flight and missed it, the second started afterwards
		// and captured it, so the slot is stably clean until its next
		// mutation. The caller holds the slot lock.
		[[nodiscard]] bool slot_committed(size_t slot) const {
			return last_mutation_commit[slot] + 2 <= commits_finished.load(std::memory_order_acquire);
		}

		void fill_slot_locked(size_t slot) {
			uint64_t const stamp = make_stamp(slot, next_seq.fetch_add(1, std::memory_order_relaxed));
			auto volatile *words = slot_words(slot);
			for (size_t w = 0; w < fill_words; ++w) {
				words[w] = stamp;
			}
			expected[slot] = stamp;
			last_mutation_commit[slot] = commits_finished.load(std::memory_order_acquire);
		}

		void writer_body() {
			ASSERT_TRUE(arm_thread_fault_stack());
			std::mt19937_64 rng{1};
			uint64_t fill_count = 0;
			while (!stop_mutators.load(std::memory_order_acquire)) {
				// occasional growth, until capacity
				if (++fill_count % 256 == 0) {
					size_t const now_slots = extended_slots.load(std::memory_order_acquire);
					if (now_slots < capacity_slots) {
						ASSERT_TRUE(reg->extend((now_slots + extend_step_slots) * bs));
						extended_slots.store(now_slots + extend_step_slots, std::memory_order_release);
						extends.fetch_add(1, std::memory_order_relaxed);
					}
				}
				size_t const slot = rng() % extended_slots.load(std::memory_order_acquire);
				{
					std::unique_lock lock{slot_locks[slot]};
#ifdef PRIVATEER_TEST_TSAN
					// only a faulting store carries the happens-before edge
					// the cleaner's freeze-and-read needs (header comment)
					if (!slot_committed(slot)) {
						continue;
					}
#endif
					fill_slot_locked(slot);
				}
				fills.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void gc_body() {
			std::mt19937_64 rng{2};
			while (!stop_mutators.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(20ms);
				size_t const limit = extended_slots.load(std::memory_order_acquire);
				size_t const count = 1 + rng() % 4;
				size_t const first = rng() % limit;
				size_t const last = std::min(first + count, limit);
				{
					std::vector<std::unique_lock<std::shared_mutex>> locks;
					for (size_t slot = first; slot < last; ++slot) {
						locks.emplace_back(slot_locks[slot]);
					}
					ASSERT_TRUE(reg->free_region(first * bs, (last - first) * bs));
					int64_t const finished = commits_finished.load(std::memory_order_acquire);
					for (size_t slot = first; slot < last; ++slot) {
						expected[slot] = 0;
						last_mutation_commit[slot] = finished;
					}
				}
				frees.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void validate_snapshot(fs::path const &snap_dir) {
			auto snap = region::open_read_only(snap_dir);
			ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
			size_t const snap_slots = snap->size() / bs;
			auto const *base = static_cast<unsigned char const *>(snap->segment());
			for (size_t slot = 0; slot < snap_slots; ++slot) {
				auto const *words = reinterpret_cast<uint64_t const *>(base + slot * bs);
				auto const diagnostic = check_slot_image(words, words_per_slot, slot);
				ASSERT_TRUE(diagnostic.empty())
						<< "torn commit in snapshot slot " << slot << ": " << diagnostic;
			}
		}

		void committer_body() {
			uint64_t round = 0;
			while (!stop_committer.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(50ms);
				ASSERT_TRUE(reg->commit(round % 2 == 0));
				commits_finished.fetch_add(1, std::memory_order_acq_rel);
				commits.fetch_add(1, std::memory_order_relaxed);
				if (++round % 4 == 0) {
					fs::path const snap_dir = dir.path / ("snap-" + std::to_string(round));
					ASSERT_TRUE(reg->snapshot_to(snap_dir));
					commits_finished.fetch_add(1, std::memory_order_acq_rel);  // inline durable commit
					validate_snapshot(snap_dir);
					fs::remove_all(snap_dir);
					snapshots.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		void reader_body(size_t reader_index) {
			std::mt19937_64 rng{100 + reader_index};
			while (!stop_readers.load(std::memory_order_acquire)) {
				size_t const slot = rng() % extended_slots.load(std::memory_order_acquire);
				std::shared_lock lock{slot_locks[slot]};
#ifdef PRIVATEER_TEST_TSAN
				// a slot inside the grace period may still see a write-back
				// remap, which TSan models as a plain write (header comment)
				if (!slot_committed(slot)) {
					lock.unlock();
					std::this_thread::sleep_for(100us);
					continue;
				}
#endif
				uint64_t const want = expected[slot];
				auto volatile const *words = slot_words(slot);
				for (size_t w = 0; w < words_per_slot; ++w) {
					uint64_t const got = words[w];
					ASSERT_EQ(got, w < fill_words ? want : 0u)
							<< "lost or foreign write in slot " << slot << " word " << w;
				}
				reads.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void verify_final_state(region &final_region) {
			size_t const limit = extended_slots.load(std::memory_order_acquire);
			ASSERT_EQ(final_region.size(), limit * bs);
			auto volatile const *base = static_cast<unsigned char volatile const *>(final_region.segment());
			for (size_t slot = 0; slot < limit; ++slot) {
				uint64_t const want = expected[slot];
				auto volatile const *words = reinterpret_cast<uint64_t volatile const *>(base + slot * bs);
				for (size_t w = 0; w < words_per_slot; ++w) {
					ASSERT_EQ(words[w], w < fill_words ? want : 0u)
							<< "final content mismatch in slot " << slot << " word " << w;
				}
			}
		}
	};

	TEST_F(RegionSoakTest, TheFullActorMixStaysConsistent) {
		std::vector<std::thread> actors;
		actors.emplace_back([&] { writer_body(); });
		actors.emplace_back([&] { gc_body(); });
		actors.emplace_back([&] { committer_body(); });
		for (size_t r = 0; r < reader_count; ++r) {
			actors.emplace_back([&, r] { reader_body(r); });
		}

		std::this_thread::sleep_for(soak_duration());

		// mutators first: close must never see a writer parked at the hard
		// mark, and the final expected[] needs quiesced content
		stop_mutators.store(true, std::memory_order_release);
		actors[0].join();
		actors[1].join();
		stop_committer.store(true, std::memory_order_release);
		actors[2].join();
		stop_readers.store(true, std::memory_order_release);
		for (size_t r = 0; r < reader_count; ++r) {
			actors[3 + r].join();
		}

		// every actor did real work
		EXPECT_GT(fills.load(), 0u);
		EXPECT_GT(frees.load(), 0u);
		EXPECT_GT(commits.load(), 0u);
		EXPECT_GT(snapshots.load(), 0u);
		EXPECT_GT(reads.load(), 0u);
		EXPECT_GT(extends.load(), 0u);

		// lost writes: the quiesced live region, a final snapshot, and the
		// reopened store all carry exactly the expected content
		ASSERT_TRUE(reg->commit(true));
		verify_final_state(*reg);

		fs::path const final_snap = dir.path / "snap-final";
		ASSERT_TRUE(reg->snapshot_to(final_snap));
		{
			auto snap = region::open_read_only(final_snap);
			ASSERT_TRUE(snap.has_value()) << to_string(snap.error());
			verify_final_state(*snap);
		}
		fs::remove_all(final_snap);

		ASSERT_TRUE(reg->check_sanity());
		auto reopened = region::open_read_only(dir.path / "store");
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		verify_final_state(*reopened);
	}

}  // namespace
