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
// The engine creates the pool threads itself and attaches them to an empty
// pool. asio's own thread_pool creates its threads inside its constructor
// and, when a later creation fails, joins the threads it already started
// without stopping their scheduler: that join never returns, so a failing
// pthread_create deadlocks the process inside a function-local static, with
// no way to catch or retry. Attached threads keep the failure recoverable.
// The pool runs with however many threads started, and a pool that got none
// reports zero, so a caller can refuse instead of queueing work that nothing
// will ever run.
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

	// Threads actually running each pool, zero when none could be started.
	// The first call starts the pool.
	[[nodiscard]] size_t work_pool_size() noexcept;
	[[nodiscard]] size_t timer_pool_size() noexcept;

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_executor {

		// Starts a pool of wanted threads the way the process pools are
		// started, runs one task on it, and returns how many threads it got
		// (zero when none started, or when the task never ran). Tests lower
		// RLIMIT_NPROC to force creation failures: the answer must arrive,
		// degraded or zero, never hang.
		size_t start_pool_and_run_task(size_t wanted);

	}  // namespace detail_executor
#endif

}  // namespace privateer

#endif  // PRIVATEER_EXECUTOR_HPP
