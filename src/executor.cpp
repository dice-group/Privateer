#include <privateer/executor.hpp>

#include <algorithm>
#include <thread>

namespace privateer {

	size_t work_pool_size() noexcept {
		// oversubscribable on purpose: the work pool's tasks block in
		// write and fdatasync, not on the CPU
		return std::max(1u, std::thread::hardware_concurrency());
	}

	asio::thread_pool &work_pool() {
		static asio::thread_pool pool{work_pool_size()};
		return pool;
	}

	asio::thread_pool &timer_pool() {
		static asio::thread_pool pool{1};
		return pool;
	}

}  // namespace privateer
