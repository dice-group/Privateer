// The A/B 2 microbenchmark: the per-block commit-path mechanics of
// content-addressed names against generation-addressed names (slot plus
// epoch, no hashing). Four arms per block size (2, 8, 32 MiB):
//
//   content_new    hash plus publish of unseen content: what an ordinary
//                  dirty block costs under content addressing
//   content_skip   hash of value-identical content whose name matches the
//                  recipe entry, nothing written: the skip that generation
//                  addressing gives up (it would rewrite the block)
//   content_dedup  hash plus publish against an existing identical file:
//                  the staged write, the refused link, the byte compare
//   generation     write under a counter name, no hash, always a fresh
//                  file: what a generation-addressed commit does for every
//                  dirty block, value-identical or not
//
// The skip and dedup RATES on real workloads are step 5 measurements; this
// bench prices the mechanics per block. The write arms unlink the produced
// file outside the timed region, so the store stays bounded. For
// decision-grade numbers run with TMPDIR on a container-local disk:
// virtio-fs mounts are slow and distort the write arms.

#include <benchmark/benchmark.h>

#include <privateer/block_hash.hpp>
#include <privateer/block_store.hpp>
#include <privateer/error.hpp>

#include "support/temp_dir.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace {

	using namespace privateer;

	size_t block_len_of(benchmark::State const &state) {
		return static_cast<size_t>(state.range(0)) * 1024 * 1024;
	}

	[[noreturn]] void die(char const *what, error const &err) {
		std::fprintf(stderr, "%s: %s\n", what, to_string(err).c_str());
		std::abort();
	}

	std::vector<std::byte> random_block(size_t len) {
		std::vector<std::byte> block(len);
		std::mt19937_64 rng{42};
		for (size_t i = 0; i + 8 <= len; i += 8) {
			uint64_t const word = rng();
			std::memcpy(block.data() + i, &word, 8);
		}
		return block;
	}

	// a generation name: slot and epoch packed into the digest bytes, the
	// same width as xxh3-128 names, no hashing
	block_digest generation_name(uint64_t slot, uint64_t epoch) {
		block_digest name;
		name.size = 16;
		std::memcpy(name.bytes.data(), &slot, sizeof(slot));
		std::memcpy(name.bytes.data() + sizeof(slot), &epoch, sizeof(epoch));
		return name;
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

	// hash plus publish of content unseen before; the epoch word makes
	// every iteration's content and name unique
	void content_new(benchmark::State &state) {
		size_t const len = block_len_of(state);
		bench_store bench;
		auto block = random_block(len);
		uint64_t epoch = 0;
		for (auto _ : state) {
			++epoch;
			std::memcpy(block.data(), &epoch, sizeof(epoch));
			auto const name = hash_block(hash_algorithm::xxh3_128, block);
			if (auto published = bench.store.publish(name, block); !published) {
				die("publish", published.error());
			}
			state.PauseTiming();
			std::filesystem::remove(bench.store.block_path(name));
			state.ResumeTiming();
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	// the value-identical skip: hash, compare against the recipe entry,
	// write nothing
	void content_skip(benchmark::State &state) {
		size_t const len = block_len_of(state);
		auto const block = random_block(len);
		auto const entry = hash_block(hash_algorithm::xxh3_128, block);
		for (auto _ : state) {
			auto const name = hash_block(hash_algorithm::xxh3_128, block);
			bool const skip = name == entry;
			benchmark::DoNotOptimize(skip);
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	// the dedup hit: hash plus publish where an identical file already
	// carries the name (the staged write, the refused link, the byte
	// compare); the staged temp file is discarded by publish itself
	void content_dedup(benchmark::State &state) {
		size_t const len = block_len_of(state);
		bench_store bench;
		auto const block = random_block(len);
		auto const entry = hash_block(hash_algorithm::xxh3_128, block);
		if (auto published = bench.store.publish(entry, block); !published) {
			die("publish", published.error());
		}
		for (auto _ : state) {
			auto const name = hash_block(hash_algorithm::xxh3_128, block);
			auto const published = bench.store.publish(name, block);
			if (!published) {
				die("publish", published.error());
			}
			benchmark::DoNotOptimize(*published);
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	// the generation-addressed block: no hash, a name that can never
	// exist, always a full write
	void generation(benchmark::State &state) {
		size_t const len = block_len_of(state);
		bench_store bench;
		auto block = random_block(len);
		uint64_t epoch = 0;
		for (auto _ : state) {
			++epoch;
			std::memcpy(block.data(), &epoch, sizeof(epoch));
			auto const name = generation_name(1, epoch);
			if (auto published = bench.store.publish(name, block); !published) {
				die("publish", published.error());
			}
			state.PauseTiming();
			std::filesystem::remove(bench.store.block_path(name));
			state.ResumeTiming();
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	BENCHMARK(content_new)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);
	BENCHMARK(content_skip)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);
	BENCHMARK(content_dedup)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);
	BENCHMARK(generation)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
