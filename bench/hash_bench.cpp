// Throughput of the block hash candidates at the engine's block sizes,
// plus a plain byte compare as the reference line for the dedup path.
// The argument is the block size in MiB.

#include <benchmark/benchmark.h>

#include <privateer/block_hash.hpp>

#include <cstring>
#include <random>
#include <vector>

namespace {

	std::vector<std::byte> random_block(size_t len) {
		std::vector<std::byte> block(len);
		std::mt19937_64 rng{42};
		for (size_t i = 0; i + 8 <= len; i += 8) {
			uint64_t const word = rng();
			std::memcpy(block.data() + i, &word, 8);
		}
		return block;
	}

	void hash_throughput(benchmark::State &state, privateer::hash_algorithm alg) {
		size_t const len = static_cast<size_t>(state.range(0)) * 1024 * 1024;
		auto const block = random_block(len);
		for (auto _ : state) {
			auto digest = privateer::hash_block(alg, block);
			benchmark::DoNotOptimize(digest);
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	void byte_compare_throughput(benchmark::State &state) {
		size_t const len = static_cast<size_t>(state.range(0)) * 1024 * 1024;
		auto const block = random_block(len);
		auto const copy = block;
		for (auto _ : state) {
			int const equal = std::memcmp(block.data(), copy.data(), len);
			benchmark::DoNotOptimize(equal);
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * len));
	}

	BENCHMARK_CAPTURE(hash_throughput, xxh3_128, privateer::hash_algorithm::xxh3_128)->Arg(2)->Arg(8)->Arg(32);
	BENCHMARK_CAPTURE(hash_throughput, blake3, privateer::hash_algorithm::blake3)->Arg(2)->Arg(8)->Arg(32);
	BENCHMARK_CAPTURE(hash_throughput, sha256, privateer::hash_algorithm::sha256)->Arg(2)->Arg(8)->Arg(32);
	BENCHMARK_CAPTURE(hash_throughput, rapidhash, privateer::hash_algorithm::rapidhash)->Arg(2)->Arg(8)->Arg(32);
	BENCHMARK(byte_compare_throughput)->Arg(2)->Arg(8)->Arg(32);

}  // namespace

BENCHMARK_MAIN();
