#include <privateer/executor.hpp>

#include <privateer/logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <asio/post.hpp>

namespace privateer {

	namespace {

		// A pool whose threads the engine owns. The pool is created empty and
		// each thread attaches to it, which is what keeps a failed thread
		// creation recoverable (see executor.hpp). Threads return from attach
		// only once the pool is stopped, so the destructor stops first and
		// joins after.
		struct attached_pool {
			asio::thread_pool pool{0};
			std::vector<std::thread> threads;

			explicit attached_pool(size_t wanted) noexcept {
				for (size_t i = 0; i < wanted; ++i) {
					try {
						threads.emplace_back([this] { pool.attach(); });
					} catch (...) {
						break;  // out of thread budget: run with what started
					}
				}
				if (threads.size() < wanted) {
					try {
						PRIVATEER_LOG(log_level::warning,
									  "the executor started {} of {} threads; this host is out of thread budget",
									  threads.size(), wanted);
					} catch (...) {
						// a failed log must not decide the outcome
					}
				}
			}

			attached_pool(attached_pool const &) = delete;
			attached_pool &operator=(attached_pool const &) = delete;

			~attached_pool() {
				pool.stop();
				for (auto &thread : threads) {
					thread.join();
				}
			}
		};

		attached_pool &work_pool_state() {
			// oversubscribable on purpose: the work pool's tasks block in
			// write and fdatasync, not on the CPU
			static attached_pool state{std::max(1u, std::thread::hardware_concurrency())};
			return state;
		}

		attached_pool &timer_pool_state() {
			static attached_pool state{1};
			return state;
		}

	}  // namespace

	asio::thread_pool &work_pool() {
		return work_pool_state().pool;
	}

	asio::thread_pool &timer_pool() {
		return timer_pool_state().pool;
	}

	size_t work_pool_size() noexcept {
		return work_pool_state().threads.size();
	}

	size_t timer_pool_size() noexcept {
		return timer_pool_state().threads.size();
	}

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_executor {

		size_t start_pool_and_run_task(size_t wanted) {
			attached_pool state{wanted};
			if (state.threads.empty()) {
				return 0;
			}
			std::atomic<uint32_t> ran{0};
			try {
				asio::post(state.pool, [&ran] { ran.store(1, std::memory_order_release); });
			} catch (...) {
				return 0;
			}
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
			while (ran.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			}
			return ran.load(std::memory_order_acquire) != 0 ? state.threads.size() : 0;
		}

	}  // namespace detail_executor
#endif

}  // namespace privateer
