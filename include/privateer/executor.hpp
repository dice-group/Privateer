#ifndef PRIVATEER_EXECUTOR_HPP
#define PRIVATEER_EXECUTOR_HPP

// The process-wide executor: two thread pools, split so a timer expiry
// never queues behind blocking file I/O. The work pool runs the blocking
// tasks (commit write-out workers, background write-back); it is
// post-only, so it needs no reactor. The timer pool has one thread and
// owns the steady timers; constructing a timer instantiates asio's
// reactor on that pool (an epoll fd, an eventfd, and a timerfd on
// Linux), and its thread never blocks on I/O.
//
// Both pools live until static teardown. Every region joins its own
// tasks and cancels its own timers when it closes, so the pool queues
// are empty at teardown. Regions must be closed before static teardown
// begins.
//
// The pools' threads do not survive a fork. A child process must not
// touch a region that was open before the fork; a region opened in the
// child works, its commits run on the committing thread.

#include <asio/thread_pool.hpp>

#include <cstddef>

namespace privateer {

	[[nodiscard]] asio::thread_pool &work_pool();
	[[nodiscard]] asio::thread_pool &timer_pool();

	// thread count of the work pool
	[[nodiscard]] size_t work_pool_size() noexcept;

}  // namespace privateer

#endif  // PRIVATEER_EXECUTOR_HPP
