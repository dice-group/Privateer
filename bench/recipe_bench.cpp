// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// What one commit pays to persist the recipe: the version 2 metadata path
// against the whole-file write of the frozen version 1 format, over the slot
// count and the size of the changeset. Three arms:
//
//   v1_whole_file      recipe::serialize plus one staged write and rename of
//                      the whole entry table, which is what version 1 pays
//                      for any change, one slot or all of them
//   v2_dirty_segments  the version 2 shape: fresh entries in the dirty
//                      slots, then encode, hash and publish of every segment
//                      those slots fall into, then the manifest
//   v2_manifest_only   publish_manifest alone, the fixed floor of every
//                      commit: one record per segment and no segment file
//
// The dirty arms sweep the changeset size and its shape. scattered spreads
// the dirty slots evenly over the whole range, so every segment gets one,
// which is the worst case; clustered is one contiguous run, which is what a
// region written in place produces. Each arm reports segments_touched and
// bytes_written per iteration, so the cost of a commit can be read against
// the work its changeset caused.
//
// No arm syncs. An fsync costs what the device charges, which would hide the
// metadata work behind it; barrier_bench prices the barrier itself.
//
// The blocks the entries name never exist: these arms write recipe metadata
// only and nothing here reads a recipe back. The segment files an iteration
// publishes are unlinked outside the timed region, so the store stays
// bounded. Run with TMPDIR on a container-local disk, since virtio-fs
// distorts every write arm.

#include <benchmark/benchmark.h>

#include <privateer/block_hash.hpp>
#include <privateer/block_store.hpp>
#include <privateer/error.hpp>
#include <privateer/file_util.hpp>
#include <privateer/recipe.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

	using namespace privateer;

	constexpr uint64_t bench_block_size = 4096;

	[[noreturn]] void die(char const *what, error const &err) {
		std::fprintf(stderr, "%s: %s\n", what, to_string(err).c_str());
		std::abort();
	}

	// Distinct digests from a counter. The counter runs over the whole
	// process, so no entry ever repeats one an earlier iteration wrote, no
	// segment file repeats content the store already holds, and every publish
	// is a real write instead of a dedup hit.
	struct digest_source {
		uint64_t next = 1;

		block_digest operator()() {
			block_digest digest;
			digest.size = 16;
			uint64_t const value = next++;
			std::memcpy(digest.bytes.data(), &value, sizeof(value));
			return digest;
		}
	};

	digest_source digests;

	// A recipe over slot_count slots, one distinct digest per slot, so no
	// segment is all empty and every segment carries a file.
	recipe build_recipe(uint64_t slot_count) {
		recipe rec;
		rec.block_size = bench_block_size;
		rec.size = slot_count * bench_block_size;
		rec.capacity = rec.size;
		rec.algorithm = hash_algorithm::xxh3_128;
		rec.entries.resize(static_cast<size_t>(slot_count));
		for (auto &entry : rec.entries) {
			entry = digests();
		}
		return rec;
	}

	struct bench_store {
		privateer::testing::temp_dir dir;
		block_store store;

		bench_store() : store{make(dir.path)} {}

	private:
		static block_store make(std::filesystem::path const &path) {
			auto created = block_store::create(path);
			if (!created) {
				die("block_store::create", created.error());
			}
			return std::move(*created);
		}
	};

	// how the dirty slots of one iteration sit in the slot range
	enum struct dirty_pattern {
		scattered,  // evenly spaced over the whole range
		clustered,  // one contiguous run
	};

	char const *to_string(dirty_pattern pattern) noexcept {
		return pattern == dirty_pattern::scattered ? "scattered" : "clustered";
	}

	// The slots one iteration dirties, in ascending order. The clustered run
	// sits in the middle of the range and is shifted by half its length, so it
	// crosses a segment boundary instead of resting inside one segment by
	// accident.
	std::vector<uint64_t> dirty_slots(uint64_t slot_count, uint64_t dirty, dirty_pattern pattern) {
		std::vector<uint64_t> slots;
		slots.reserve(static_cast<size_t>(dirty));
		if (pattern == dirty_pattern::scattered) {
			uint64_t const stride = std::max<uint64_t>(1, slot_count / dirty);
			for (uint64_t i = 0; i < dirty; ++i) {
				slots.push_back(std::min(slot_count - 1, i * stride));
			}
		} else {
			uint64_t const start = slot_count / 2 - dirty / 2;
			for (uint64_t i = 0; i < dirty; ++i) {
				slots.push_back(start + i);
			}
		}
		return slots;
	}

	// The segments the dirty slots fall into, each one once. The slots are
	// ascending, so a compare against the last index is enough.
	std::vector<size_t> touched_segments(std::span<uint64_t const> slots) {
		std::vector<size_t> touched;
		for (uint64_t const slot : slots) {
			size_t const index = static_cast<size_t>(slot / recipe_segment_slots);
			if (touched.empty() || touched.back() != index) {
				touched.push_back(index);
			}
		}
		return touched;
	}

	// One segment of a commit: frame its entries, name them by their hash,
	// publish the file, and install the record the manifest needs. Returns the
	// bytes of the segment file.
	uint64_t publish_segment(recipe &rec, block_store const &store, size_t index,
							 std::vector<block_digest> &published) {
		auto encoded = encode_segment(rec, index);
		if (!encoded) {
			die("encode_segment", encoded.error());
		}
		if (!*encoded) {
			rec.segments[index] = segment_record{};
			return 0;
		}
		auto const name = hash_block(rec.algorithm, **encoded);
		if (auto ok = store.publish(name, **encoded); !ok) {
			die("publish", ok.error());
		}
		rec.segments[index] = segment_record{segment_encoding::raw,
											 static_cast<uint32_t>((*encoded)->size()), name};
		published.push_back(name);
		return (*encoded)->size();
	}

	// bytes of the manifest the last commit wrote
	uint64_t manifest_bytes(std::filesystem::path const &segment_dir) {
		return std::filesystem::file_size(segment_dir / recipe_file_name);
	}

	// The frozen version 1 commit: the whole entry table in one buffer, staged
	// and renamed over the file of the last commit.
	void v1_whole_file(benchmark::State &state, uint64_t slot_count) {
		privateer::testing::temp_dir dir;
		auto const rec = build_recipe(slot_count);
		uint64_t bytes_written = 0;
		for (auto _ : state) {
			auto encoded = rec.serialize();
			if (!encoded) {
				die("serialize", encoded.error());
			}
			auto staged = staged_file::create_in(dir.path);
			if (!staged) {
				die("staged_file::create_in", staged.error());
			}
			if (auto written = staged->write(*encoded); !written) {
				die("write", written.error());
			}
			if (auto published = staged->publish(recipe_file_name, publish_mode::replace); !published) {
				die("publish", published.error());
			}
			bytes_written = encoded->size();
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * bytes_written));
		state.counters["bytes_written"] = static_cast<double>(bytes_written);
		// The file carries every entry, so one changed slot rewrites the range
		// of every segment.
		state.counters["segments_touched"] = static_cast<double>(segment_count(slot_count));
	}

	// The version 2 commit over a changeset: the segments the dirty slots fall
	// into, then the manifest.
	void v2_dirty_segments(benchmark::State &state, uint64_t slot_count, uint64_t dirty, dirty_pattern pattern) {
		bench_store bench;
		recipe rec = build_recipe(slot_count);
		// Every segment published once, so an iteration prices the rewrite of
		// what it dirties and not the first publication of the whole recipe.
		auto initial = rec.commit(bench.dir.path, bench.store, false);
		if (!initial) {
			die("recipe::commit", initial.error());
		}
		rec.segments = std::move(*initial);
		uint64_t const manifest_len = manifest_bytes(bench.dir.path);

		auto const slots = dirty_slots(slot_count, dirty, pattern);
		auto const touched = touched_segments(slots);
		std::vector<block_digest> published;
		uint64_t bytes_written = 0;
		for (auto _ : state) {
			published.clear();
			for (uint64_t const slot : slots) {
				rec.entries[static_cast<size_t>(slot)] = digests();
			}
			for (size_t const index : touched) {
				bytes_written += publish_segment(rec, bench.store, index, published);
			}
			if (auto written = publish_manifest(rec, bench.dir.path, false); !written) {
				die("publish_manifest", written.error());
			}
			bytes_written += manifest_len;
			state.PauseTiming();
			for (auto const &name : published) {
				std::filesystem::remove(bench.store.block_path(name));
			}
			state.ResumeTiming();
		}
		state.SetBytesProcessed(static_cast<int64_t>(bytes_written));
		state.counters["bytes_written"] =
				benchmark::Counter(static_cast<double>(bytes_written), benchmark::Counter::kAvgIterations);
		state.counters["segments_touched"] = static_cast<double>(touched.size());
	}

	// The floor of every commit: the manifest alone, with no segment changed.
	void v2_manifest_only(benchmark::State &state, uint64_t slot_count) {
		bench_store bench;
		recipe rec = build_recipe(slot_count);
		auto initial = rec.commit(bench.dir.path, bench.store, false);
		if (!initial) {
			die("recipe::commit", initial.error());
		}
		rec.segments = std::move(*initial);
		uint64_t const manifest_len = manifest_bytes(bench.dir.path);

		for (auto _ : state) {
			if (auto written = publish_manifest(rec, bench.dir.path, false); !written) {
				die("publish_manifest", written.error());
			}
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * manifest_len));
		state.counters["bytes_written"] = static_cast<double>(manifest_len);
		state.counters["segments_touched"] = 0;
	}

	// --- registration ---

	// Real time, because the arms wait for the file system and the reported
	// rates divide by the reported time.
	void configure(benchmark::Benchmark *arm) {
		arm->UseRealTime()->Unit(benchmark::kMicrosecond);
	}

	void register_arms() {
		// 65536 slots is 256 MiB at 4 KiB blocks and four segments; the two
		// large counts are what a 1 TiB and a 10 TiB region need at 2 MiB
		// blocks, 32 and 320 segments.
		constexpr std::array<uint64_t, 3> slot_counts{65536, 524288, 5242880};

		for (uint64_t const slot_count : slot_counts) {
			configure(benchmark::RegisterBenchmark(
					"v1_whole_file/slots:" + std::to_string(slot_count),
					[slot_count](benchmark::State &state) { v1_whole_file(state, slot_count); }));
			configure(benchmark::RegisterBenchmark(
					"v2_manifest_only/slots:" + std::to_string(slot_count),
					[slot_count](benchmark::State &state) { v2_manifest_only(state, slot_count); }));
		}

		for (uint64_t const slot_count : std::array<uint64_t, 2>{524288, 5242880}) {
			for (uint64_t const dirty : std::array<uint64_t, 3>{1, 128, 10000}) {
				for (dirty_pattern const pattern : {dirty_pattern::scattered, dirty_pattern::clustered}) {
					auto const label = "v2_dirty_segments/slots:" + std::to_string(slot_count) + "/dirty:" +
									   std::to_string(dirty) + "/" + to_string(pattern);
					configure(benchmark::RegisterBenchmark(label,
														   [slot_count, dirty, pattern](benchmark::State &state) {
															   v2_dirty_segments(state, slot_count, dirty, pattern);
														   }));
				}
			}
		}
	}

}  // namespace

int main(int argc, char **argv) {
	register_arms();
	benchmark::Initialize(&argc, argv);
	if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
		return 1;
	}
	benchmark::RunSpecifiedBenchmarks();
	benchmark::Shutdown();
	return 0;
}
