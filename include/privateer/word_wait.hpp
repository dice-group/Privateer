// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_WORD_WAIT_HPP
#define PRIVATEER_WORD_WAIT_HPP

// Async-signal-safe wait and wake on a 32-bit atomic word. The compare-value
// form closes the lost-wakeup race: the waiter passes the value it observed,
// and the wait parks only while the word still holds that value. Linux waits
// with the raw futex syscall; Darwin spins with nanosleep backoff (nanosleep
// is on the async-signal-safe list), so wakes are no-ops there. Timed waits
// bound the total wait with one CLOCK_MONOTONIC deadline that survives EINTR
// and spurious wakeups.

#include <atomic>
#include <cstdint>

namespace privateer {

	// monotonic clock in nanoseconds, async-signal-safe on both platforms
	[[nodiscard]] int64_t monotonic_now_ns() noexcept;

	// Waits while word holds observed. Returns the changed value (acquire).
	uint32_t word_wait(std::atomic<uint32_t> &word, uint32_t observed) noexcept;

	// Timed variant. Returns the current value of word; a return equal to
	// observed means the deadline passed with the word unchanged.
	uint32_t word_wait_for(std::atomic<uint32_t> &word, uint32_t observed, int64_t timeout_ns) noexcept;

	// Wakes waiters on the word. Darwin waiters poll, so this is a no-op there.
	void word_wake_one(std::atomic<uint32_t> &word) noexcept;
	void word_wake_all(std::atomic<uint32_t> &word) noexcept;

}  // namespace privateer

#endif  // PRIVATEER_WORD_WAIT_HPP
