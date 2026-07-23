#include <privateer/word_wait.hpp>

#include <privateer/handler_text.hpp>

#include <ctime>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace privateer {

	namespace {

		constexpr int64_t ns_per_s = 1'000'000'000;

#ifndef __linux__
		// Darwin has no signal-safe wait-on-address, so waiters poll with
		// exponential nanosleep backoff between these bounds.
		constexpr int64_t backoff_min_ns = 1'000;
		constexpr int64_t backoff_max_ns = 1'000'000;
#endif

	}  // namespace

	PRIVATEER_HANDLER_TEXT int64_t monotonic_now_ns() noexcept {
#ifdef __linux__
		timespec ts{};
		::clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<int64_t>(ts.tv_sec) * ns_per_s + ts.tv_nsec;
#else
		return static_cast<int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#endif
	}

#ifdef __linux__

	namespace {

		// 0: woken; EAGAIN: the value already changed; EINTR, ETIMEDOUT:
		// the caller re-checks the word and its deadline
		PRIVATEER_HANDLER_TEXT void futex_wait(std::atomic<uint32_t> &word, uint32_t observed,
											   timespec const *relative_timeout) noexcept {
			::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAIT_PRIVATE, observed,
					  relative_timeout, nullptr, 0);
		}

	}  // namespace

	PRIVATEER_HANDLER_TEXT uint32_t word_wait(std::atomic<uint32_t> &word, uint32_t observed) noexcept {
		uint32_t value = word.load(std::memory_order_acquire);
		while (value == observed) {
			futex_wait(word, observed, nullptr);
			value = word.load(std::memory_order_acquire);
		}
		return value;
	}

	PRIVATEER_HANDLER_TEXT uint32_t word_wait_for(std::atomic<uint32_t> &word, uint32_t observed,
												  int64_t timeout_ns) noexcept {
		int64_t const deadline = monotonic_now_ns() + timeout_ns;
		uint32_t value = word.load(std::memory_order_acquire);
		while (value == observed) {
			int64_t const remaining = deadline - monotonic_now_ns();
			if (remaining <= 0) {
				return value;
			}
			timespec relative{};
			relative.tv_sec = remaining / ns_per_s;
			relative.tv_nsec = remaining % ns_per_s;
			futex_wait(word, observed, &relative);
			value = word.load(std::memory_order_acquire);
		}
		return value;
	}

	PRIVATEER_HANDLER_TEXT void word_wake_one(std::atomic<uint32_t> &word) noexcept {
		::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
	}

	PRIVATEER_HANDLER_TEXT void word_wake_all(std::atomic<uint32_t> &word) noexcept {
		::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr,
				  nullptr, 0);
	}

#else

	namespace {

		// one poll step: sleeps backoff_ns, re-loops on EINTR against nothing
		// (the callers own the deadline), and doubles the backoff up to the cap
		PRIVATEER_HANDLER_TEXT void backoff_sleep(int64_t &backoff_ns) noexcept {
			timespec const pause{0, static_cast<long>(backoff_ns)};
			::nanosleep(&pause, nullptr);
			if (backoff_ns < backoff_max_ns) {
				backoff_ns *= 2;
			}
		}

	}  // namespace

	PRIVATEER_HANDLER_TEXT uint32_t word_wait(std::atomic<uint32_t> &word, uint32_t observed) noexcept {
		int64_t backoff_ns = backoff_min_ns;
		uint32_t value = word.load(std::memory_order_acquire);
		while (value == observed) {
			backoff_sleep(backoff_ns);
			value = word.load(std::memory_order_acquire);
		}
		return value;
	}

	PRIVATEER_HANDLER_TEXT uint32_t word_wait_for(std::atomic<uint32_t> &word, uint32_t observed,
												  int64_t timeout_ns) noexcept {
		int64_t const deadline = monotonic_now_ns() + timeout_ns;
		int64_t backoff_ns = backoff_min_ns;
		uint32_t value = word.load(std::memory_order_acquire);
		while (value == observed) {
			if (monotonic_now_ns() >= deadline) {
				return value;
			}
			backoff_sleep(backoff_ns);
			value = word.load(std::memory_order_acquire);
		}
		return value;
	}

	PRIVATEER_HANDLER_TEXT void word_wake_one(std::atomic<uint32_t> &) noexcept {}

	PRIVATEER_HANDLER_TEXT void word_wake_all(std::atomic<uint32_t> &) noexcept {}

#endif

}  // namespace privateer
