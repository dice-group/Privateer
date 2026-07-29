// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/fault_handler.hpp>

#include <privateer/handler_text.hpp>
#include <privateer/logger.hpp>
#include <privateer/mlocked.hpp>
#include <privateer/region_registry.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

// The bounds of the handler text section, resolved by the linker: ld64
// resolves section$start / section$end asm labels, ELF linkers publish
// __start_/__stop_ symbols for sections whose name is a C identifier.
#ifdef __APPLE__
extern "C" {
extern char const __start_pv_handler_text[] __asm("section$start$__TEXT$__pv_handler");
extern char const __stop_pv_handler_text[] __asm("section$end$__TEXT$__pv_handler");
}
#else
extern "C" {
extern char const __start_pv_handler_text[];
extern char const __stop_pv_handler_text[];
}
#endif

namespace privateer {

	namespace {

		std::mutex g_install_mutex;
		std::atomic<bool> g_installed{false};
		struct sigaction g_prev_segv {};
		struct sigaction g_prev_bus {};  // used on Darwin only

		// forwards a fault the handler does not own to the saved disposition
		PRIVATEER_HANDLER_TEXT void forward_to(struct sigaction const &prev, int sig, siginfo_t *si, void *ctx) {
			if ((prev.sa_flags & SA_SIGINFO) != 0) {
				prev.sa_sigaction(sig, si, ctx);
				return;
			}
			if (prev.sa_handler == SIG_DFL || prev.sa_handler == SIG_IGN) {
				// SIG_IGN on a synchronous fault would retry-loop forever, so
				// both go to the default action: restore SIG_DFL and return,
				// the retried instruction re-faults into the default action
				::signal(sig, SIG_DFL);
				return;
			}
			prev.sa_handler(sig);
		}

		extern "C" PRIVATEER_HANDLER_TEXT void privateer_fault_handler(int sig, siginfo_t *si, void *ctx) {
			// the fault path issues syscalls; the interrupted thread's errno
			// must survive the handler
			int const saved_errno = errno;
			auto addr = reinterpret_cast<uintptr_t>(si != nullptr ? si->si_addr : nullptr);
#ifdef __aarch64__
			// The kernel delivers si_addr untagged without SA_EXPOSE_TAGBITS,
			// but a tagged fault address must never miss the region lookup and
			// turn a barrier fault into a forwarded crash: mask the top byte.
			addr &= (uintptr_t{1} << 56) - 1;
#endif
			auto *const rec = global_registry().acquire(addr, region_registry::in_flight_kind::handler);
			if (rec != nullptr) {
				bool const handled = rec->on_fault != nullptr && rec->on_fault(*rec, addr, sig);
				region_registry::release(*rec, region_registry::in_flight_kind::handler);
				if (handled) {
					errno = saved_errno;
					return;  // the faulting instruction is retried
				}
			}
			forward_to(sig == SIGBUS ? g_prev_bus : g_prev_segv, sig, si, ctx);
			errno = saved_errno;
		}

		// mlocks the pages of the handler text section
		result<> mlock_handler_text() {
			auto begin = reinterpret_cast<uintptr_t>(__start_pv_handler_text);
			auto end = reinterpret_cast<uintptr_t>(__stop_pv_handler_text);
			auto const page = static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
			begin &= ~(page - 1);
			end = (end + page - 1) & ~(page - 1);
			if (::mlock(reinterpret_cast<void *>(begin), end - begin) != 0) {
				if (errno == ENOMEM || errno == EAGAIN) {
					return fail_errno(errc::memlock_limit_too_low, "mlock handler text");
				}
				// Darwin refuses to lock executable pages (EPERM). A paged-out
				// text page is repaired by a transparent kernel page-in, not by
				// a fault signal, so running unlocked degrades handler latency,
				// not correctness.
				PRIVATEER_LOG(log_level::warning, "cannot lock the handler text range (errno {})", errno);
			}
			return {};
		}

		// per-thread alternate stack, mlocked, released at thread exit
		struct thread_stack {
			mlocked_buffer buf;
			bool armed = false;

			~thread_stack() {
				if (armed) {
					stack_t ss{};
					ss.ss_flags = SS_DISABLE;
					::sigaltstack(&ss, nullptr);
				}
			}
		};

		thread_local thread_stack t_stack;

	}  // namespace

	result<> install_fault_handler() {
		std::lock_guard const lock{g_install_mutex};
		if (g_installed.load(std::memory_order_relaxed)) {
			return {};
		}
		if (auto locked = mlock_handler_text(); !locked) {
			return locked;
		}

		struct sigaction sa {};
		sa.sa_sigaction = privateer_fault_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		if (::sigaction(SIGSEGV, &sa, &g_prev_segv) != 0) {
			return fail_errno(errc::io_error, "sigaction SIGSEGV");
		}
#ifdef __APPLE__
		if (::sigaction(SIGBUS, &sa, &g_prev_bus) != 0) {
			auto const err = fail_errno(errc::io_error, "sigaction SIGBUS");
			::sigaction(SIGSEGV, &g_prev_segv, nullptr);
			return err;
		}
#endif
		g_installed.store(true, std::memory_order_release);
		return {};
	}

	bool fault_handler_installed() noexcept {
		return g_installed.load(std::memory_order_acquire);
	}

	result<> arm_thread_fault_stack() {
		if (t_stack.armed) {
			return {};
		}
		// generous over SIGSTKSZ: sanitizer instrumentation inflates frames
		size_t const len = std::max(static_cast<size_t>(SIGSTKSZ), size_t{128} * 1024);
		auto buf = mlocked_buffer::allocate(len);
		if (!buf) {
			return std::unexpected{buf.error()};
		}
		stack_t ss{};
		ss.ss_sp = buf->addr();
		ss.ss_size = buf->size();
		ss.ss_flags = 0;
		if (::sigaltstack(&ss, nullptr) != 0) {
			return fail_errno(errc::io_error, "sigaltstack");
		}
		t_stack.buf = std::move(*buf);
		t_stack.armed = true;
		return {};
	}

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_fault_handler {

		void uninstall_for_tests() noexcept {
			std::lock_guard const lock{g_install_mutex};
			if (!g_installed.load(std::memory_order_relaxed)) {
				return;
			}
			::sigaction(SIGSEGV, &g_prev_segv, nullptr);
#ifdef __APPLE__
			::sigaction(SIGBUS, &g_prev_bus, nullptr);
#endif
			g_installed.store(false, std::memory_order_release);
		}

	}  // namespace detail_fault_handler
#endif

}  // namespace privateer
