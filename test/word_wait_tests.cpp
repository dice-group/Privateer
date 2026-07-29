// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/word_wait.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <thread>
#include <vector>

#include <pthread.h>

using namespace privateer;

namespace {

	constexpr int64_t ms = 1'000'000;

	TEST(WordWaitTest, MonotonicClockAdvances) {
		int64_t const before = monotonic_now_ns();
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
		EXPECT_GT(monotonic_now_ns(), before);
	}

	TEST(WordWaitTest, ChangedWordReturnsImmediately) {
		std::atomic<uint32_t> word{5};
		EXPECT_EQ(word_wait(word, 4u), 5u);
		EXPECT_EQ(word_wait_for(word, 4u, 60'000 * ms), 5u);
	}

	TEST(WordWaitTest, TimedWaitRespectsTheDeadline) {
		std::atomic<uint32_t> word{0};
		int64_t const timeout = 50 * ms;
		int64_t const before = monotonic_now_ns();
		EXPECT_EQ(word_wait_for(word, 0u, timeout), 0u);
		int64_t const elapsed = monotonic_now_ns() - before;
		EXPECT_GE(elapsed, timeout);
		EXPECT_LT(elapsed, 100 * timeout);  // wide bound, CI machines stall
	}

	TEST(WordWaitTest, NonPositiveTimeoutReturnsImmediately) {
		std::atomic<uint32_t> word{0};
		EXPECT_EQ(word_wait_for(word, 0u, 0), 0u);
		EXPECT_EQ(word_wait_for(word, 0u, -1), 0u);
	}

	TEST(WordWaitTest, WakeReleasesTheWaiter) {
		std::atomic<uint32_t> word{0};
		uint32_t seen = 0;
		std::thread waiter{[&] { seen = word_wait(word, 0u); }};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		word.store(7, std::memory_order_release);
		word_wake_all(word);
		waiter.join();
		EXPECT_EQ(seen, 7u);
	}

	TEST(WordWaitTest, WakeReleasesTheTimedWaiterBeforeTheDeadline) {
		std::atomic<uint32_t> word{0};
		uint32_t seen = 0;
		int64_t elapsed = 0;
		std::thread waiter{[&] {
			int64_t const before = monotonic_now_ns();
			seen = word_wait_for(word, 0u, 60'000 * ms);
			elapsed = monotonic_now_ns() - before;
		}};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		word.store(3, std::memory_order_release);
		word_wake_all(word);
		waiter.join();
		EXPECT_EQ(seen, 3u);
		EXPECT_LT(elapsed, 30'000 * ms);
	}

	TEST(WordWaitTest, WakeAllReleasesEveryWaiter) {
		std::atomic<uint32_t> word{0};
		std::atomic<int> released{0};
		std::vector<std::thread> waiters;
		for (int i = 0; i < 8; ++i) {
			waiters.emplace_back([&] {
				if (word_wait(word, 0u) == 9u) {
					released.fetch_add(1);
				}
			});
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		word.store(9, std::memory_order_release);
		word_wake_all(word);
		for (auto &waiter : waiters) {
			waiter.join();
		}
		EXPECT_EQ(released.load(), 8);
	}

	TEST(WordWaitTest, WakeOneReleasesAtLeastOneWaiter) {
		std::atomic<uint32_t> word{0};
		std::atomic<int> released{0};
		std::thread waiter{[&] {
			word_wait(word, 0u);
			released.fetch_add(1);
		}};
		std::this_thread::sleep_for(std::chrono::milliseconds{20});
		word.store(1, std::memory_order_release);
		word_wake_one(word);
		waiter.join();
		EXPECT_EQ(released.load(), 1);
	}

	TEST(WordWaitTest, SignalInterruptsDoNotExtendTheDeadline) {
		struct sigaction sa {};
		sa.sa_handler = [](int) {};
		sigemptyset(&sa.sa_mask);
		struct sigaction saved {};
		ASSERT_EQ(sigaction(SIGUSR1, &sa, &saved), 0);

		std::atomic<uint32_t> word{0};
		int64_t const timeout = 200 * ms;
		int64_t elapsed = 0;
		std::atomic<bool> done{false};
		std::thread waiter{[&] {
			int64_t const before = monotonic_now_ns();
			word_wait_for(word, 0u, timeout);
			elapsed = monotonic_now_ns() - before;
			done.store(true);
		}};
		while (!done.load()) {
			pthread_kill(waiter.native_handle(), SIGUSR1);
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
		waiter.join();
		ASSERT_EQ(sigaction(SIGUSR1, &saved, nullptr), 0);

		EXPECT_GE(elapsed, timeout);
		EXPECT_LT(elapsed, 100 * timeout);
	}

}  // namespace
