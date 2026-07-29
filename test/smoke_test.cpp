// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/version.hpp>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <latch>

TEST(Smoke, Version) {
	EXPECT_STREQ(privateer::version(), "0.2.0");
}

// Validates the ASIO_SEPARATE_COMPILATION setup: posting work exercises the
// scheduler implementation that lives in the dedicated asio TU.
TEST(Smoke, ThreadPoolPostAndJoin) {
	asio::thread_pool pool{4};
	std::atomic<int> count{0};
	std::latch done{100};
	for (int i = 0; i < 100; ++i) {
		asio::post(pool, [&] {
			count.fetch_add(1, std::memory_order_relaxed);
			done.count_down();
		});
	}
	done.wait();
	EXPECT_EQ(count.load(), 100);
	pool.join();
}

// Validates that a steady_timer works on a thread_pool executor (the design
// runs all periodic work on a one-thread timer pool, not an io_context).
TEST(Smoke, SteadyTimerOnThreadPool) {
	asio::thread_pool timer_pool{1};
	asio::steady_timer timer{timer_pool, std::chrono::milliseconds{1}};
	std::atomic<bool> fired{false};
	std::latch done{1};
	timer.async_wait([&](std::error_code const &ec) {
		fired.store(!ec);
		done.count_down();
	});
	done.wait();
	EXPECT_TRUE(fired.load());
	timer_pool.join();
}
