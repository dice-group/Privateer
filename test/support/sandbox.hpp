// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_TEST_SANDBOX_HPP
#define PRIVATEER_TEST_SANDBOX_HPP

// Fork-based crash harness, a port of the dice-template-library sandbox.
// Runs a code block in a child process and reports how the child exited.
// Used to assert that code crashes (or does not crash) without killing the test binary.
//
// Preconditions:
// - the process is single threaded when the sandbox forks (or no other thread holds a global lock)
// - the block must not call std::exit; return instead
// - under TSan the binary links support/sanitizer_options.cpp, which turns off
//   the guard that kills a child creating a thread after a multi-threaded fork

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

namespace privateer::testing {

	// how the sandboxed child exited
	enum struct subprocess_result : int {
		exit_success = -1,  // normal exit, code == 0
		exit_failure = -2,  // normal exit, code != 0
		interrupted = SIGINT,
		illegal_instruction = SIGILL,
		aborted = SIGABRT,
		floating_point_exception = SIGFPE,
		killed = SIGKILL,
		bus_error = SIGBUS,
		segmentation_fault = SIGSEGV,
		terminated = SIGTERM,
	};

	// true if the child died from the platform's protection-fault signal
	// (SIGSEGV on Linux; Darwin uses SIGBUS for protection faults and SIGSEGV for unmapped addresses)
	inline bool is_fault_signal(subprocess_result res) {
		return res == subprocess_result::segmentation_fault || res == subprocess_result::bus_error;
	}

	namespace detail_sandbox {

		inline void clear_all_signal_handlers() {
			static constexpr int max_sig = [] {
#ifdef NSIG
				return NSIG;
#elifdef _NSIG
				return _NSIG;
#else
				return 64;
#endif
			}();

			sigset_t empty_sigs;
			sigemptyset(&empty_sigs);

			// unblock globally inhibited signals
			::sigprocmask(SIG_SETMASK, &empty_sigs, nullptr);

			struct sigaction sa {};
			sa.sa_handler = SIG_DFL;
			sa.sa_mask = empty_sigs;
			sa.sa_flags = 0;

			for (int i = 1; i < max_sig; ++i) {
				::sigaction(i, &sa, nullptr);  // ignore errors (EINVAL for unknown signals)
			}
		}

		inline void flush_all_streams() {
			// if std::ios_base::sync_with_stdio(false) was ever called the C++ streams
			// have their own buffers, so flush them explicitly
			std::cout.flush();
			std::cerr.flush();
			fflush(nullptr);
		}

		// the noexcept is important: if func throws, this turns it into std::terminate signalling SIGABRT
		template<typename F>
		[[nodiscard]] int invoke_like_main(F &&func) noexcept {
			if constexpr (std::is_same_v<std::invoke_result_t<F>, void>) {
				std::invoke(std::forward<F>(func));
				return 0;
			} else {
				return std::invoke(std::forward<F>(func));
			}
		}

		struct sandbox {};

		template<typename F>
		[[nodiscard]] subprocess_result operator+(sandbox, F func) {
			static_assert(std::is_invocable_r_v<void, F> || std::is_invocable_r_v<int, F>,
						  "Function must be invocable like a main() function");

			// ensure no stale data is in output streams before fork duplicates them
			flush_all_streams();

			int const pid = fork();
			if (pid < 0) {
				throw std::system_error{errno, std::system_category(), "Unable to fork"};
			}

			if (pid == 0) {
				// child: remove handlers that may have been installed (e.g. by gtest or a sanitizer)
				clear_all_signal_handlers();

				int const exit_code = invoke_like_main(std::move(func));

				flush_all_streams();

				// _exit so no destructors run and no resources of the parent are closed
				::_exit(exit_code);
			}

			// parent
			int wstatus;
			int res;
			do {
				res = ::waitpid(pid, &wstatus, 0);
			} while (res < 0 && errno == EINTR);

			if (res < 0) {
				throw std::system_error{errno, std::system_category(), "waitpid failed"};
			}

			if (WIFEXITED(wstatus)) {
				return WEXITSTATUS(wstatus) == 0 ? subprocess_result::exit_success
												 : subprocess_result::exit_failure;
			}
			if (WIFSIGNALED(wstatus)) {
				return static_cast<subprocess_result>(WTERMSIG(wstatus));
			}
			throw std::runtime_error{"Process exited in an unexpected way"};
		}

	}  // namespace detail_sandbox
}  // namespace privateer::testing

// Runs the following code block in a forked child process and yields a subprocess_result.
//
// auto const res = PRIVATEER_SANDBOX {
//     *(volatile int *)8 = 1;
// };
// EXPECT_TRUE(is_fault_signal(res));
#define PRIVATEER_SANDBOX ::privateer::testing::detail_sandbox::sandbox{} + [&]()

#endif  // PRIVATEER_TEST_SANDBOX_HPP
