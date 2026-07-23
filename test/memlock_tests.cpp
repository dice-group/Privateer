// Tests for mlocked buffers and the resource limit handling: the
// raise-or-refuse contract for RLIMIT_MEMLOCK, the unlocked override for
// swapless deployments, and the Darwin RLIMIT_NOFILE raise. The limit
// lowering runs in sandbox children so the test process keeps its limits.

#include <gtest/gtest.h>

#include <privateer/mlocked.hpp>
#include <privateer/rlimits.hpp>

#include "support/sandbox.hpp"

#include <algorithm>
#include <cstring>

#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

// sanitizer detection: gcc defines __SANITIZE_*, clang answers __has_feature
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PRIVATEER_TEST_SANITIZED 1
#endif
#ifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PRIVATEER_TEST_SANITIZED 1
#endif
#endif

using namespace privateer;
using privateer::testing::subprocess_result;

namespace {

	TEST(MlockedBuffer, AllocatesLockedMemory) {
		auto buf = mlocked_buffer::allocate(4096);
		ASSERT_TRUE(buf.has_value()) << to_string(buf.error());
		EXPECT_NE(buf->addr(), nullptr);
		EXPECT_EQ(buf->size(), 4096u);
		EXPECT_TRUE(buf->locked());
		std::memset(buf->addr(), 0xAB, buf->size());
	}

	TEST(MlockedBuffer, RejectsZeroLength) {
		auto buf = mlocked_buffer::allocate(0);
		ASSERT_FALSE(buf.has_value());
		EXPECT_EQ(buf.error().code, errc::invalid_argument);
	}

	TEST(MlockedBuffer, MoveTransfersOwnership) {
		auto buf = mlocked_buffer::allocate(4096);
		ASSERT_TRUE(buf.has_value());
		void *const addr = buf->addr();

		mlocked_buffer moved = std::move(*buf);
		EXPECT_EQ(moved.addr(), addr);
		EXPECT_EQ(moved.size(), 4096u);
		EXPECT_EQ(buf->addr(), nullptr);

		mlocked_buffer assigned;
		assigned = std::move(moved);
		EXPECT_EQ(assigned.addr(), addr);
		EXPECT_EQ(moved.addr(), nullptr);
	}

	TEST(Rlimits, MemlockSucceedsForASmallRequirement) {
		EXPECT_TRUE(ensure_memlock_limit(4096));
	}

	TEST(Rlimits, RefusesWhenTheHardLimitIsBelowTheRequirement) {
		if (::geteuid() == 0) {
			GTEST_SKIP() << "root may lock memory regardless of RLIMIT_MEMLOCK";
		}
		auto const res = PRIVATEER_SANDBOX {
			rlimit const zero{0, 0};
			if (::setrlimit(RLIMIT_MEMLOCK, &zero) != 0) {
				return 2;
			}
			auto const r = ensure_memlock_limit(size_t{1} << 20);
			if (r.has_value()) {
				return 3;
			}
			if (r.error().code != errc::memlock_limit_too_low) {
				return 4;
			}
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

	TEST(Rlimits, RaisesTheSoftLimitTowardTheHardLimit) {
		auto const res = PRIVATEER_SANDBOX {
			rlimit lim{};
			if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
				return 2;
			}
			if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < 4096) {
				return 0;  // hard limit already below one page; the refuse test covers this shape
			}
			lim.rlim_cur = 0;
			if (::setrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
				return 3;
			}
			if (!ensure_memlock_limit(4096)) {
				return 4;
			}
			if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0 || (lim.rlim_cur != RLIM_INFINITY && lim.rlim_cur < 4096)) {
				return 5;
			}
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

	TEST(MlockedBuffer, UnlockedOverrideBypassesTheLimit) {
#ifdef PRIVATEER_TEST_SANITIZED
		GTEST_SKIP() << "sanitizers intercept mlock as a no-op, so the over-limit lock cannot fail";
#endif
		if (::geteuid() == 0) {
			GTEST_SKIP() << "root may lock memory regardless of RLIMIT_MEMLOCK";
		}
		auto const res = PRIVATEER_SANDBOX {
			rlimit const zero{0, 0};
			if (::setrlimit(RLIMIT_MEMLOCK, &zero) != 0) {
				return 2;
			}
			auto locked = mlocked_buffer::allocate(size_t{1} << 20);
			if (locked.has_value() || locked.error().code != errc::memlock_limit_too_low) {
				return 3;
			}
			auto unlocked = mlocked_buffer::allocate(size_t{1} << 20, false);
			if (!unlocked.has_value() || unlocked->locked()) {
				return 4;
			}
			std::memset(unlocked->addr(), 0xCD, unlocked->size());
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

	TEST(Rlimits, RaiseNofileLimitSucceeds) {
		EXPECT_TRUE(raise_nofile_limit());
#ifdef __APPLE__
		rlimit lim{};
		ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &lim), 0);
		rlim_t expected = static_cast<rlim_t>(OPEN_MAX);
		if (lim.rlim_max != RLIM_INFINITY) {
			expected = std::min(expected, lim.rlim_max);
		}
		EXPECT_GE(lim.rlim_cur, expected);
#endif
	}

}  // namespace
