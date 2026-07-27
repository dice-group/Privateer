// The A/B 6 microbenchmark: the shape of the commit durability barrier.
// The engine's barrier is one fdatasync per new block file plus one fsync
// per shard directory that received an entry, all issued in line on the
// committing thread (block_store::make_durable). Three shapes over the same
// freshly written files:
//
//   serial      block_store::make_durable itself: open, sync, close, one
//               file after another, then the shard directories
//   pool:<w>    the same syncs claimed from one index counter by w threads
//               that park between batches: the fan-out shape of the commit
//               write-out, applied to the barrier
//   uring:<d>   one io_uring batch, IORING_FSYNC_DATASYNC per block file and
//               a plain fsync per shard directory, up to d requests in
//               flight (full means the whole batch at once)
//
// The block count sweeps 1, 8, 64, 512 and 4096, which spans a one-slot
// commit up to a 8 GiB dirty set at 2 MiB blocks. Every iteration publishes
// that many block files with fresh random content and no syncs (untimed),
// times the barrier over them, then unlinks them (untimed). The timed region
// includes the open of every file and directory, because make_durable opens
// them too. Only the serial arm keeps the durable-name set up to date; that
// is one hash-set insert per name against a device barrier, so the arms stay
// comparable.
//
// Run with TMPDIR on the file system under test. On tmpfs every barrier is a
// no-op and the numbers mean nothing. A full run writes about 80 GiB, so
// budget device space and time, and --benchmark_repetitions=3 triples it.
// The io_uring arms need Linux and liburing; without it they are not
// registered. Inside a container they also need the io_uring_setup syscall,
// which docker's default seccomp profile denies (run with
// --security-opt seccomp=unconfined); an arm whose ring is refused reports the
// reason and is skipped. The deep queue needs file descriptors, so the binary
// raises RLIMIT_NOFILE to its hard limit at start and caps the in-flight count
// at what is left; the effective depth is reported as a counter.

#include <benchmark/benchmark.h>

#include <privateer/block_hash.hpp>
#include <privateer/block_store.hpp>
#include <privateer/error.hpp>
#include <privateer/file_util.hpp>
#include <privateer/word_wait.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef PRIVATEER_BENCH_HAS_URING
#include <liburing.h>
#endif

namespace {

	using namespace privateer;

	// Bytes one arm dirties per iteration, before the iteration count is
	// capped. It bounds how long the small-block arms run.
	constexpr uint64_t bytes_per_arm = 2ull * 1024 * 1024 * 1024;

	// descriptors kept for the engine, the store and the benchmark harness
	constexpr size_t fd_reserve = 64;

	[[noreturn]] void die(char const *what, error const &err) {
		std::fprintf(stderr, "%s: %s\n", what, to_string(err).c_str());
		std::abort();
	}

	[[noreturn]] void die_errno(char const *what, int code) {
		std::fprintf(stderr, "%s: %s\n", what, std::strerror(code));
		std::abort();
	}

	// Raises the soft descriptor limit to the hard limit and reports the
	// resulting soft limit.
	size_t raise_open_files() {
		rlimit limit{};
		if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
			die_errno("getrlimit(RLIMIT_NOFILE)", errno);
		}
		if (limit.rlim_cur < limit.rlim_max) {
			rlimit raised = limit;
			raised.rlim_cur = limit.rlim_max;
			if (::setrlimit(RLIMIT_NOFILE, &raised) == 0) {
				limit = raised;
			}
		}
		return static_cast<size_t>(limit.rlim_cur);
	}

	size_t open_file_budget() {
		static size_t const budget = raise_open_files();
		return budget > fd_reserve ? budget - fd_reserve : 1;
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

	// A block name for iteration and index, spread over the shards by the
	// first byte the way content digests are. No hashing: the barrier does
	// not care how the name was found.
	block_digest batch_name(uint64_t iteration, size_t index) {
		block_digest name;
		name.size = 16;
		uint64_t const high = (iteration << 24) | static_cast<uint64_t>(index);
		uint64_t const low = ~high;
		std::memcpy(name.bytes.data(), &high, sizeof(high));
		std::memcpy(name.bytes.data() + sizeof(high), &low, sizeof(low));
		name.bytes[0] = static_cast<std::byte>(index & 0xffu);
		return name;
	}

	// one sync the barrier owes: a block file (fdatasync) or a shard
	// directory (fsync)
	struct sync_request {
		std::string path;
		bool datasync = true;
	};

	int open_for_sync(sync_request const &request) {
		int const flags = O_RDONLY | O_CLOEXEC | (request.datasync ? 0 : O_DIRECTORY);
		int const fd = ::open(request.path.c_str(), flags);
		if (fd < 0) {
			die_errno("open for the barrier", errno);
		}
		return fd;
	}

	void sync_one(sync_request const &request) {
		int const fd = open_for_sync(request);
		auto const synced = request.datasync ? sync_file(fd) : sync_directory(fd);
		::close(fd);
		if (!synced) {
			die("barrier sync", synced.error());
		}
	}

	// --- the pool shape ---

	// Threads that park between batches and claim requests from one index
	// counter, joined through a countdown word: the shape of the commit
	// write-out fan-out. On Darwin word_wait polls instead of parking, so the
	// join costs more there; the decision runs on Linux.
	struct sync_pool {
		explicit sync_pool(size_t workers) {
			threads_.reserve(workers);
			for (size_t w = 0; w < workers; ++w) {
				threads_.emplace_back([this] { work(); });
			}
		}

		~sync_pool() {
			stop_.store(true, std::memory_order_relaxed);
			release_.fetch_add(1, std::memory_order_release);
			word_wake_all(release_);
			for (auto &thread : threads_) {
				thread.join();
			}
		}

		sync_pool(sync_pool const &) = delete;
		sync_pool &operator=(sync_pool const &) = delete;

		void run(std::span<sync_request const> requests) {
			requests_ = requests;
			next_.store(0, std::memory_order_relaxed);
			pending_.store(static_cast<uint32_t>(threads_.size()), std::memory_order_relaxed);
			release_.fetch_add(1, std::memory_order_release);
			word_wake_all(release_);
			for (uint32_t left = pending_.load(std::memory_order_acquire); left != 0;) {
				left = word_wait(pending_, left);
			}
		}

	private:
		void work() {
			// The main thread bumps the release word once per batch and waits
			// for every worker to count down, so a worker observes exactly one
			// bump per batch.
			uint32_t seen = 0;
			for (;;) {
				uint32_t current = release_.load(std::memory_order_acquire);
				while (current == seen) {
					current = word_wait(release_, current);
				}
				seen = current;
				if (stop_.load(std::memory_order_relaxed)) {
					return;
				}
				for (;;) {
					size_t const index = next_.fetch_add(1, std::memory_order_relaxed);
					if (index >= requests_.size()) {
						break;
					}
					sync_one(requests_[index]);
				}
				if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
					word_wake_all(pending_);
				}
			}
		}

		std::vector<std::thread> threads_;
		std::span<sync_request const> requests_;
		std::atomic<size_t> next_{0};
		std::atomic<uint32_t> pending_{0};
		std::atomic<uint32_t> release_{0};
		std::atomic<bool> stop_{false};
	};

	// --- the io_uring shape ---

#ifdef PRIVATEER_BENCH_HAS_URING
	// One ring, reused across iterations. Requests go in up to depth at a
	// time, each with the descriptor it owns; a completion closes it. A host
	// that refuses the ring (a container seccomp profile denies io_uring_setup,
	// and old kernels have no such call) leaves the ring unusable and the arm
	// reports why instead of ending the run.
	struct sync_ring {
		explicit sync_ring(unsigned entries) { init_rc_ = ::io_uring_queue_init(entries, &ring_, 0); }

		~sync_ring() {
			if (init_rc_ == 0) {
				::io_uring_queue_exit(&ring_);
			}
		}

		sync_ring(sync_ring const &) = delete;
		sync_ring &operator=(sync_ring const &) = delete;

		[[nodiscard]] bool usable() const { return init_rc_ == 0; }
		[[nodiscard]] std::string init_error() const {
			return std::string{"io_uring_queue_init: "} + std::strerror(-init_rc_);
		}

		void run(std::span<sync_request const> requests, size_t depth) {
			fds_.assign(requests.size(), -1);
			size_t next = 0;
			size_t in_flight = 0;
			size_t done = 0;
			while (done < requests.size()) {
				size_t queued = 0;
				while (next < requests.size() && in_flight < depth) {
					io_uring_sqe *const sqe = ::io_uring_get_sqe(&ring_);
					if (sqe == nullptr) {
						break;
					}
					fds_[next] = open_for_sync(requests[next]);
					::io_uring_prep_fsync(sqe, fds_[next],
										  requests[next].datasync ? IORING_FSYNC_DATASYNC : 0u);
					::io_uring_sqe_set_data64(sqe, next);
					++next;
					++in_flight;
					++queued;
				}
				if (queued != 0) {
					if (int const rc = ::io_uring_submit(&ring_); rc < 0) {
						die_errno("io_uring_submit", -rc);
					}
				}
				io_uring_cqe *cqe = nullptr;
				if (int const rc = ::io_uring_wait_cqe(&ring_, &cqe); rc < 0) {
					die_errno("io_uring_wait_cqe", -rc);
				}
				unsigned head = 0;
				unsigned reaped = 0;
				io_uring_for_each_cqe(&ring_, head, cqe) {
					if (cqe->res < 0) {
						die_errno("io_uring fsync", -cqe->res);
					}
					size_t const index = static_cast<size_t>(::io_uring_cqe_get_data64(cqe));
					::close(fds_[index]);
					fds_[index] = -1;
					++reaped;
				}
				::io_uring_cq_advance(&ring_, reaped);
				in_flight -= reaped;
				done += reaped;
			}
		}

	private:
		io_uring ring_{};
		int init_rc_ = 0;
		std::vector<int> fds_;
	};
#endif  // PRIVATEER_BENCH_HAS_URING

	// --- the batch under the barrier ---

	// One store whose files are republished per iteration: publish writes the
	// content through a staged file and links it under its name, without any
	// sync, which is what the commit write-out leaves behind for the barrier.
	struct barrier_batch {
		explicit barrier_batch(size_t block_len) : store_{make_store(dir_.path)}, payload_{random_block(block_len)} {}

		void publish(size_t blocks) {
			names_.clear();
			names_.reserve(blocks);
			requests_.clear();
			requests_.reserve(blocks);
			shards_.clear();
			for (size_t index = 0; index < blocks; ++index) {
				auto const name = batch_name(iteration_, index);
				// unique content per file, so nothing shares extents and no
				// compressing file system collapses the batch
				uint64_t const stamp = (iteration_ << 24) | static_cast<uint64_t>(index);
				std::memcpy(payload_.data(), &stamp, sizeof(stamp));
				if (auto published = store_.publish(name, payload_); !published) {
					die("publish", published.error());
				}
				names_.push_back(name);
				auto const path = store_.block_path(name);
				requests_.push_back({path.string(), true});
				shards_.emplace(static_cast<uint8_t>(name.bytes[0]), path.parent_path().string());
			}
			for (auto const &[shard, path] : shards_) {
				requests_.push_back({path, false});
			}
			++iteration_;
		}

		[[nodiscard]] std::span<sync_request const> requests() const { return requests_; }
		[[nodiscard]] size_t shards() const { return shards_.size(); }

		// the engine's own barrier: block_store::make_durable
		void make_durable_inline() {
			if (auto synced = store_.make_durable(names_); !synced) {
				die("make_durable", synced.error());
			}
		}

		void remove() {
			std::error_code ec;
			for (auto const &name : names_) {
				std::filesystem::remove(store_.block_path(name), ec);
			}
		}

	private:
		static block_store make_store(std::filesystem::path const &path) {
			auto created = block_store::create(path);
			if (!created) {
				die("block_store::create", created.error());
			}
			return std::move(*created);
		}

		privateer::testing::temp_dir dir_;
		block_store store_;
		std::vector<std::byte> payload_;
		uint64_t iteration_ = 1;
		std::vector<block_digest> names_;
		std::vector<sync_request> requests_;
		std::map<uint8_t, std::string> shards_;
	};

	enum struct barrier_shape { serial, pool, uring };

	// width is the worker count for pool and the in-flight cap for uring,
	// zero meaning the whole batch at once
	void barrier_arm(benchmark::State &state, size_t blocks, size_t block_len, barrier_shape shape, size_t width) {
		barrier_batch batch{block_len};
		size_t depth = 0;

		std::optional<sync_pool> pool;
		if (shape == barrier_shape::pool) {
			pool.emplace(width);
		}
#ifdef PRIVATEER_BENCH_HAS_URING
		std::optional<sync_ring> ring;
		if (shape == barrier_shape::uring) {
			// the batch is the block files plus at most one directory per shard
			size_t const requests = blocks + std::min<size_t>(blocks, 256);
			depth = std::min(width == 0 ? requests : width, std::min(requests, open_file_budget()));
			ring.emplace(static_cast<unsigned>(std::min<size_t>(depth, 32768)));
			if (!ring->usable()) {
				state.SkipWithError(ring->init_error());
				return;
			}
		}
#endif

		for (auto _ : state) {
			state.PauseTiming();
			batch.publish(blocks);
			state.ResumeTiming();
			switch (shape) {
				case barrier_shape::serial:
					batch.make_durable_inline();
					break;
				case barrier_shape::pool:
					pool->run(batch.requests());
					break;
				case barrier_shape::uring:
#ifdef PRIVATEER_BENCH_HAS_URING
					ring->run(batch.requests(), depth);
#endif
					break;
			}
			state.PauseTiming();
			batch.remove();
			state.ResumeTiming();
		}

		state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * blocks * block_len));
		state.counters["blocks_per_s"] =
				benchmark::Counter(static_cast<double>(blocks), benchmark::Counter::kIsIterationInvariantRate);
		state.counters["shards"] = static_cast<double>(batch.shards());
		if (shape == barrier_shape::uring) {
			state.counters["depth"] = static_cast<double>(depth);
		}
	}

	// --- registration ---

	// Iterations that keep an arm near bytes_per_arm, at least one and at
	// most twenty.
	int iterations_for(size_t blocks, size_t block_len) {
		uint64_t const per_iteration = static_cast<uint64_t>(blocks) * block_len;
		uint64_t const wanted = std::max<uint64_t>(1, bytes_per_arm / per_iteration);
		return static_cast<int>(std::min<uint64_t>(wanted, 20));
	}

	void register_arm(std::string const &name, size_t blocks, size_t block_len, barrier_shape shape, size_t width) {
		auto const label = name + "/blocks:" + std::to_string(blocks) + "/MiB:" +
						   std::to_string(block_len / (1024 * 1024));
		// Real time, because a barrier waits for a device and the pool and ring
		// shapes spend most of it outside the calling thread; the reported rates
		// divide by the reported time.
		benchmark::RegisterBenchmark(label,
									 [blocks, block_len, shape, width](benchmark::State &state) {
										 barrier_arm(state, blocks, block_len, shape, width);
									 })
				->Iterations(iterations_for(blocks, block_len))
				->UseRealTime()
				->Unit(benchmark::kMillisecond);
	}

	void register_arms() {
		constexpr size_t mib = 1024 * 1024;

		// the block-count sweep at the block size A/B 3 selected
		for (size_t const blocks : std::array<size_t, 5>{1, 8, 64, 512, 4096}) {
			register_arm("barrier/serial", blocks, 2 * mib, barrier_shape::serial, 0);
			for (size_t const workers : std::array<size_t, 3>{4, 16, 48}) {
				if (workers <= blocks) {
					register_arm("barrier/pool:" + std::to_string(workers), blocks, 2 * mib, barrier_shape::pool,
								 workers);
				}
			}
#ifdef PRIVATEER_BENCH_HAS_URING
			register_arm("barrier/uring:full", blocks, 2 * mib, barrier_shape::uring, 0);
			if (blocks > 64) {
				register_arm("barrier/uring:64", blocks, 2 * mib, barrier_shape::uring, 64);
			}
#endif
		}

		// the block-size axis at one batch size: the barrier moves bytes as
		// well as descriptors
		for (size_t const block_mib : std::array<size_t, 2>{8, 32}) {
			register_arm("barrier/serial", 64, block_mib * mib, barrier_shape::serial, 0);
			register_arm("barrier/pool:16", 64, block_mib * mib, barrier_shape::pool, 16);
#ifdef PRIVATEER_BENCH_HAS_URING
			register_arm("barrier/uring:full", 64, block_mib * mib, barrier_shape::uring, 0);
#endif
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
