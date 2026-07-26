// Engine microbenchmarks on a real region: barrier fault latency, commit
// throughput and worker scaling, writer stall during a commit, and reader
// disturbance during commit capture. Every benchmark sweeps the A/B 3
// block-size candidates (2, 8, 32 MiB); the commit benchmark additionally
// sweeps the worker count (A/B 4).
//
// The datastore lives under the default temp directory. For decision-grade
// numbers run with TMPDIR on a container-local disk: virtio-fs mounts are
// slow, and tmpfs makes every durability barrier a no-op. The binary is not
// meant to run under sanitizers; the readers and the stall writer race the
// engine on purpose, which is the workload, not a bug.

#include <benchmark/benchmark.h>

#include <privateer/error.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

	using namespace privateer;

	// every benchmark dirties and commits this many bytes, so the numbers
	// are comparable across block sizes
	constexpr uint64_t working_set = 256ull * 1024 * 1024;

	uint64_t block_size_of(benchmark::State const &state) {
		return static_cast<uint64_t>(state.range(0)) * 1024 * 1024;
	}

	[[noreturn]] void die(char const *what, error const &err) {
		std::fprintf(stderr, "%s: %s\n", what, to_string(err).c_str());
		std::abort();
	}

	struct bench_region {
		privateer::testing::temp_dir dir;
		region reg;

		bench_region(uint64_t block_size, size_t commit_workers)
			: reg{make(dir.path, block_size, commit_workers)} {
			if (auto armed = arm_thread_fault_stack(); !armed) {
				die("arm_thread_fault_stack", armed.error());
			}
			if (auto extended = reg.extend(working_set); !extended) {
				die("extend", extended.error());
			}
		}

		[[nodiscard]] size_t slots() const {
			return working_set / reg.block_size();
		}

		[[nodiscard]] unsigned char volatile *bytes() {
			return static_cast<unsigned char volatile *>(reg.segment());
		}

		// one store per slot, content unique per slot and epoch, so the
		// commit skips nothing and dedups nothing
		void dirty_all(uint64_t epoch) {
			auto const bs = reg.block_size();
			for (size_t slot = 0; slot < slots(); ++slot) {
				auto volatile *words = reinterpret_cast<uint64_t volatile *>(bytes() + slot * bs);
				words[0] = (static_cast<uint64_t>(slot + 1) << 32) | (epoch + 1);
			}
		}

		void commit_or_die(bool durable) {
			if (auto committed = reg.commit(durable); !committed) {
				die("commit", committed.error());
			}
		}

	private:
		static region make(std::filesystem::path const &path, uint64_t block_size,
						   size_t commit_workers) {
			region_options opts;
			opts.block_size = block_size;
			opts.commit_workers = commit_workers;
			auto reg = region::create(path / "store", working_set, opts);
			if (!reg) {
				die("create", reg.error());
			}
			return std::move(*reg);
		}
	};

	// --- barrier fault latency ---

	// One first write into a clean slot per iteration: signal, handler claim,
	// mprotect to read-write, dirty publish, retried store. The content never
	// changes, so the reset commits ride the value-identical skip and the
	// store keeps its single set of block files.
	void fault_latency(benchmark::State &state) {
		bench_region bench{block_size_of(state), 0};
		auto const bs = bench.reg.block_size();
		bench.dirty_all(0);
		bench.commit_or_die(false);  // all slots clean, all content known

		size_t slot = 0;
		for (auto _ : state) {
			if (slot == bench.slots()) {
				slot = 0;
				bench.commit_or_die(false);  // untimed epoch reset
			}
			auto volatile *words = reinterpret_cast<uint64_t volatile *>(bench.bytes() + slot * bs);
			auto const begin = std::chrono::steady_clock::now();
			words[0] = (static_cast<uint64_t>(slot + 1) << 32) | 1;
			auto const end = std::chrono::steady_clock::now();
			state.SetIterationTime(std::chrono::duration<double>{end - begin}.count());
			++slot;
		}
	}

	// --- commit throughput and worker scaling ---

	// Dirties the whole working set (untimed), then measures one commit.
	// The durable variant pays the fdatasync barrier and the rename; the
	// non-durable variant isolates the hash, write, and remap pipeline whose
	// worker scaling A/B 4 decides. The untimed durable commit afterwards
	// reclaims the retired blocks, so the store footprint stays bounded.
	void commit_bench(benchmark::State &state, bool durable) {
		bench_region bench{block_size_of(state), static_cast<size_t>(state.range(1))};
		uint64_t epoch = 0;
		for (auto _ : state) {
			state.PauseTiming();
			bench.dirty_all(epoch++);
			state.ResumeTiming();
			bench.commit_or_die(durable);
			state.PauseTiming();
			bench.commit_or_die(true);  // empty dirty set: reclaim and rename only
			state.ResumeTiming();
		}
		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * working_set));
	}

	void commit_non_durable(benchmark::State &state) {
		commit_bench(state, false);
	}

	void commit_durable(benchmark::State &state) {
		commit_bench(state, true);
	}

	// --- writer stall during a commit ---

	// A writer hammers random slots while the commit writes them out. A
	// write into a captured slot faults and waits for that slot's own
	// release, so the stall distribution measures the per-slot pipeline, not
	// the whole commit. The measured time is the commit under writer
	// pressure; the counters carry the stall tail.
	void commit_writer_stall(benchmark::State &state) {
		bench_region bench{block_size_of(state), 0};
		auto const bs = bench.reg.block_size();

		std::atomic<bool> run{true};
		std::atomic<bool> commit_active{false};
		std::vector<double> stalls_ns;
		std::thread writer{[&] {
			if (auto armed = arm_thread_fault_stack(); !armed) {
				die("arm_thread_fault_stack", armed.error());
			}
			uint64_t x = 88172645463325252ull;  // xorshift, deterministic slot walk
			while (run.load(std::memory_order_acquire)) {
				if (!commit_active.load(std::memory_order_acquire)) {
					std::this_thread::yield();
					continue;
				}
				x ^= x << 13;
				x ^= x >> 7;
				x ^= x << 17;
				size_t const slot = x % bench.slots();
				auto volatile *words = reinterpret_cast<uint64_t volatile *>(bench.bytes() + slot * bs);
				auto const begin = std::chrono::steady_clock::now();
				words[1] = x;
				auto const end = std::chrono::steady_clock::now();
				stalls_ns.push_back(std::chrono::duration<double, std::nano>{end - begin}.count());
			}
		}};

		uint64_t epoch = 0;
		for (auto _ : state) {
			state.PauseTiming();
			bench.dirty_all(epoch++);
			commit_active.store(true, std::memory_order_release);
			state.ResumeTiming();
			bench.commit_or_die(false);
			state.PauseTiming();
			commit_active.store(false, std::memory_order_release);
			bench.commit_or_die(true);  // capture the re-dirtied slots, reclaim
			state.ResumeTiming();
		}
		run.store(false, std::memory_order_release);
		writer.join();

		if (!stalls_ns.empty()) {
			std::sort(stalls_ns.begin(), stalls_ns.end());
			auto const at = [&](double q) {
				return stalls_ns[static_cast<size_t>(q * static_cast<double>(stalls_ns.size() - 1))];
			};
			state.counters["stall_p50_ns"] = at(0.50);
			state.counters["stall_p99_ns"] = at(0.99);
			state.counters["stall_max_ns"] = stalls_ns.back();
			state.counters["stall_samples"] = static_cast<double>(stalls_ns.size());
		}
	}

	// --- reader disturbance during commit capture ---

	// Readers scan committed slots while commits run; the capture's
	// run-coalesced protection downgrades shoot down their TLB entries. The
	// measured time is the commit; the counters compare the readers' scan
	// rate during commits against their undisturbed baseline.
	void reader_disturbance(benchmark::State &state) {
		bench_region bench{block_size_of(state), 0};
		auto const bs = bench.reg.block_size();
		bench.dirty_all(0);
		bench.commit_or_die(true);  // give the readers committed content

		constexpr size_t reader_count = 4;
		std::atomic<bool> run{true};
		std::atomic<uint64_t> words_read{0};
		std::vector<std::thread> readers;
		for (size_t r = 0; r < reader_count; ++r) {
			readers.emplace_back([&] {
				while (run.load(std::memory_order_acquire)) {
					for (size_t slot = 0; slot < bench.slots(); ++slot) {
						auto volatile const *words =
								reinterpret_cast<uint64_t volatile const *>(bench.bytes() + slot * bs);
						uint64_t sum = 0;
						for (size_t w = 0; w < bs / sizeof(uint64_t); w += 8) {
							sum += words[w];  // one load per cache line pair
						}
						benchmark::DoNotOptimize(sum);
						words_read.fetch_add(bs / sizeof(uint64_t) / 8, std::memory_order_relaxed);
					}
					if (!run.load(std::memory_order_acquire)) {
						break;
					}
				}
			});
		}

		auto const rate_over = [&](std::chrono::milliseconds window) {
			auto const before = words_read.load(std::memory_order_relaxed);
			auto const begin = std::chrono::steady_clock::now();
			std::this_thread::sleep_for(window);
			auto const elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - begin};
			auto const after = words_read.load(std::memory_order_relaxed);
			return static_cast<double>(after - before) / elapsed.count();
		};
		double const baseline = rate_over(std::chrono::milliseconds{500});

		uint64_t epoch = 0;
		uint64_t disturbed_words = 0;
		double disturbed_seconds = 0;
		for (auto _ : state) {
			state.PauseTiming();
			bench.dirty_all(epoch++);
			auto const before = words_read.load(std::memory_order_relaxed);
			auto const begin = std::chrono::steady_clock::now();
			state.ResumeTiming();
			bench.commit_or_die(false);
			state.PauseTiming();
			disturbed_seconds += std::chrono::duration<double>{std::chrono::steady_clock::now() - begin}.count();
			disturbed_words += words_read.load(std::memory_order_relaxed) - before;
			bench.commit_or_die(true);  // reclaim
			state.ResumeTiming();
		}
		run.store(false, std::memory_order_release);
		for (auto &reader : readers) {
			reader.join();
		}

		state.counters["reader_words_per_s_baseline"] = baseline;
		if (disturbed_seconds > 0) {
			state.counters["reader_words_per_s_during_commit"] =
					static_cast<double>(disturbed_words) / disturbed_seconds;
		}
	}

	BENCHMARK(fault_latency)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->UseManualTime();
	BENCHMARK(commit_non_durable)
			->ArgNames({"MiB", "workers"})
			->ArgsProduct({{2, 8, 32}, {1, 2, 4, 8, 16}})
			->Unit(benchmark::kMillisecond);
	BENCHMARK(commit_durable)
			->ArgNames({"MiB", "workers"})
			->ArgsProduct({{2, 8, 32}, {0}})
			->Unit(benchmark::kMillisecond);
	BENCHMARK(commit_writer_stall)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);
	BENCHMARK(reader_disturbance)->ArgNames({"MiB"})->Arg(2)->Arg(8)->Arg(32)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
