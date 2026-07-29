// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Probes P1 to P3: the write-barrier signal mechanics the engine is built on.
//
// P1: a write to a PROT_READ MAP_PRIVATE file mapping faults with the expected
//     signal and si_code, the handler upgrades the page and the retried store lands.
// P2: the handler can park on a timed wait (futex on Linux, nanosleep backoff on
//     Darwin) and is woken by another thread; EINTR re-loops against one deadline;
//     on timeout the handler proceeds anyway (the governor's bounded wait).
// P3: faults the handler does not own are forwarded: to a saved SA_SIGINFO
//     handler, to a saved plain handler, and to SIG_DFL / SIG_IGN by restoring
//     the default disposition so the retried instruction dies honestly.

#include <gtest/gtest.h>

#include <support/sandbox.hpp>

#include "probe_support.hpp"

#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <thread>

using namespace privateer::probes;
using privateer::testing::is_fault_signal;
using privateer::testing::subprocess_result;

namespace {

	// ---- P1: fault, classify, upgrade, resume ----

	void *g_slot = nullptr;
	size_t g_slot_len = 0;
	std::atomic<int> g_fault_count{0};
	std::atomic<int> g_signo{0};
	std::atomic<int> g_si_code{0};
	std::atomic<uintptr_t> g_si_addr{0};

	extern "C" void upgrade_handler(int sig, siginfo_t *si, void *) {
		auto const addr = reinterpret_cast<uintptr_t>(si->si_addr);
		auto const slot = reinterpret_cast<uintptr_t>(g_slot);
		if (addr >= slot && addr < slot + g_slot_len) {
			g_fault_count.fetch_add(1, std::memory_order_relaxed);
			g_signo.store(sig, std::memory_order_relaxed);
			g_si_code.store(si->si_code, std::memory_order_relaxed);
			g_si_addr.store(addr, std::memory_order_relaxed);
			::mprotect(g_slot, g_slot_len, PROT_READ | PROT_WRITE);
			return;  // the retried store succeeds
		}
		::signal(sig, SIG_DFL);  // not ours: refault into the default action
	}

	TEST(BarrierProbe, WriteFaultUpgradesAndResumes) {
		size_t const len = page_size();
		temp_file file{len, 'A'};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);
		g_slot = m.addr;
		g_slot_len = len;
		g_fault_count.store(0);

		scoped_sigaction const segv{SIGSEGV, upgrade_handler};
		scoped_sigaction const bus{SIGBUS, upgrade_handler};

		auto *p = m.bytes();
		EXPECT_EQ(p[0], 'A');  // a read does not fault
		EXPECT_EQ(g_fault_count.load(), 0);

		p[0] = 'B';  // one barrier fault
		EXPECT_EQ(g_fault_count.load(), 1);
		EXPECT_EQ(p[0], 'B');

		p[1] = 'C';  // the page is writable now, no second fault
		EXPECT_EQ(g_fault_count.load(), 1);

		EXPECT_EQ(g_si_addr.load(), reinterpret_cast<uintptr_t>(m.addr));
#ifdef __linux__
		EXPECT_EQ(g_signo.load(), SIGSEGV);
		EXPECT_EQ(g_si_code.load(), SEGV_ACCERR);
#else
		EXPECT_EQ(g_signo.load(), SIGBUS);
		RecordProperty("darwin_si_code", g_si_code.load());
#endif

		// MAP_PRIVATE: the file itself is untouched
		unsigned char b = 0;
		ASSERT_EQ(::pread(file.fd, &b, 1, 0), 1);
		EXPECT_EQ(b, 'A');

		g_slot = nullptr;
	}

	// ---- P2: timed wait inside the handler ----

	std::atomic<uint32_t> g_wait_word{0};
	std::atomic<int64_t> g_wait_timeout_ns{0};
	std::atomic<bool> g_timed_out{false};

	extern "C" void waiting_handler(int sig, siginfo_t *si, void *) {
		auto const addr = reinterpret_cast<uintptr_t>(si->si_addr);
		auto const slot = reinterpret_cast<uintptr_t>(g_slot);
		if (addr >= slot && addr < slot + g_slot_len) {
			if (!wait_for_word(g_wait_word, g_wait_timeout_ns.load(std::memory_order_relaxed))) {
				g_timed_out.store(true, std::memory_order_relaxed);
			}
			// woken or timed out: proceed either way, like the governor's bounded wait
			::mprotect(g_slot, g_slot_len, PROT_READ | PROT_WRITE);
			return;
		}
		::signal(sig, SIG_DFL);
	}

	extern "C" void noop_handler(int, siginfo_t *, void *) {}

	TEST(BarrierProbe, InHandlerWaitWokenByOtherThread) {
		size_t const len = page_size();
		temp_file file{len, 0};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);
		g_slot = m.addr;
		g_slot_len = len;
		g_wait_word.store(0);
		g_timed_out.store(false);
		g_wait_timeout_ns.store(5'000'000'000);

		scoped_sigaction const segv{SIGSEGV, waiting_handler};
		scoped_sigaction const bus{SIGBUS, waiting_handler};
		scoped_sigaction const usr1{SIGUSR1, noop_handler};

		pthread_t const faulting_thread = pthread_self();
		std::thread waker{[&] {
			std::this_thread::sleep_for(std::chrono::milliseconds{50});
			// exercise the EINTR re-loop while the handler is parked
			for (int i = 0; i < 3; ++i) {
				::pthread_kill(faulting_thread, SIGUSR1);
				std::this_thread::sleep_for(std::chrono::milliseconds{5});
			}
			g_wait_word.store(1, std::memory_order_release);
			wake_all(g_wait_word);
		}};

		auto const t0 = std::chrono::steady_clock::now();
		m.bytes()[0] = 1;  // faults, parks in the handler until the waker stores 1
		auto const elapsed = std::chrono::steady_clock::now() - t0;
		waker.join();

		EXPECT_FALSE(g_timed_out.load());
		EXPECT_GE(elapsed, std::chrono::milliseconds{40});
		EXPECT_EQ(m.bytes()[0], 1);

		g_slot = nullptr;
	}

	TEST(BarrierProbe, InHandlerWaitTimesOutAndProceeds) {
		size_t const len = page_size();
		temp_file file{len, 0};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);
		g_slot = m.addr;
		g_slot_len = len;
		g_wait_word.store(0);
		g_timed_out.store(false);
		g_wait_timeout_ns.store(100'000'000);

		scoped_sigaction const segv{SIGSEGV, waiting_handler};
		scoped_sigaction const bus{SIGBUS, waiting_handler};

		auto const t0 = std::chrono::steady_clock::now();
		m.bytes()[0] = 1;  // nobody wakes; the handler must time out and proceed
		auto const elapsed = std::chrono::steady_clock::now() - t0;

		EXPECT_TRUE(g_timed_out.load());
		EXPECT_GE(elapsed, std::chrono::milliseconds{100});
		EXPECT_EQ(m.bytes()[0], 1);

		g_slot = nullptr;
	}

	// ---- P3: chaining to the previous disposition ----

	void *g_chain_slot = nullptr;  // what the chaining handler owns; everything else forwards
	size_t g_chain_len = 0;
	std::atomic<int> g_forward_count{0};
	struct sigaction g_prev_segv {};
	struct sigaction g_prev_bus {};

	void forward_to(struct sigaction const &prev, int sig, siginfo_t *si, void *ctx) {
		if ((prev.sa_flags & SA_SIGINFO) != 0) {
			prev.sa_sigaction(sig, si, ctx);
			return;
		}
		if (prev.sa_handler == SIG_DFL || prev.sa_handler == SIG_IGN) {
			// SIG_IGN on a synchronous fault would loop forever, so both go to the
			// default action: restore SIG_DFL and let the retried instruction die
			::signal(sig, SIG_DFL);
			return;
		}
		prev.sa_handler(sig);
	}

	extern "C" void chaining_handler(int sig, siginfo_t *si, void *ctx) {
		auto const addr = reinterpret_cast<uintptr_t>(si->si_addr);
		auto const slot = reinterpret_cast<uintptr_t>(g_chain_slot);
		if (g_chain_slot != nullptr && addr >= slot && addr < slot + g_chain_len) {
			::mprotect(g_chain_slot, g_chain_len, PROT_READ | PROT_WRITE);
			return;
		}
		g_forward_count.fetch_add(1, std::memory_order_relaxed);
		forward_to(sig == SIGSEGV ? g_prev_segv : g_prev_bus, sig, si, ctx);
	}

	// previous SA_SIGINFO handler: records the call and repairs the fault so execution resumes
	std::atomic<int> g_prev_ran{0};

	extern "C" void prev_siginfo_handler(int, siginfo_t *si, void *) {
		g_prev_ran.fetch_add(1, std::memory_order_relaxed);
		void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(si->si_addr) & ~(page_size() - 1));
		::mprotect(page, page_size(), PROT_READ | PROT_WRITE);
	}

	TEST(BarrierProbe, ForwardsToSavedSiginfoHandler) {
		size_t const len = page_size();
		temp_file file{len, 0};
		mapping foreign = mapping::map_file(file.fd, len, PROT_READ);  // not owned by the chaining handler
		g_chain_slot = nullptr;
		g_chain_len = 0;
		g_forward_count.store(0);
		g_prev_ran.store(0);

		scoped_sigaction const inner_segv{SIGSEGV, prev_siginfo_handler};
		scoped_sigaction const inner_bus{SIGBUS, prev_siginfo_handler};
		scoped_sigaction const outer_segv{SIGSEGV, chaining_handler};
		scoped_sigaction const outer_bus{SIGBUS, chaining_handler};
		g_prev_segv = outer_segv.saved;
		g_prev_bus = outer_bus.saved;

		foreign.bytes()[0] = 1;  // chaining handler forwards, the previous handler repairs

		EXPECT_EQ(g_forward_count.load(), 1);
		EXPECT_EQ(g_prev_ran.load(), 1);
		EXPECT_EQ(foreign.bytes()[0], 1);
	}

	// previous plain handler: only gets the signal number, escapes via siglongjmp
	sigjmp_buf g_jmp;

	extern "C" void prev_plain_handler(int) {
		siglongjmp(g_jmp, 1);
	}

	TEST(BarrierProbe, ForwardsToSavedPlainHandler) {
		size_t const len = page_size();
		temp_file file{len, 0};
		mapping foreign = mapping::map_file(file.fd, len, PROT_READ);
		g_chain_slot = nullptr;
		g_chain_len = 0;
		g_forward_count.store(0);

		struct sigaction plain {};
		plain.sa_handler = prev_plain_handler;
		sigemptyset(&plain.sa_mask);
		plain.sa_flags = 0;
		struct sigaction saved_segv {}, saved_bus {};
		ASSERT_EQ(::sigaction(SIGSEGV, &plain, &saved_segv), 0);
		ASSERT_EQ(::sigaction(SIGBUS, &plain, &saved_bus), 0);

		{
			scoped_sigaction const outer_segv{SIGSEGV, chaining_handler};
			scoped_sigaction const outer_bus{SIGBUS, chaining_handler};
			g_prev_segv = outer_segv.saved;
			g_prev_bus = outer_bus.saved;

			bool reached = false;
			if (sigsetjmp(g_jmp, 1) == 0) {
				foreign.bytes()[0] = 1;  // forwards to the plain handler, which longjmps out
				FAIL() << "the plain handler did not run";
			} else {
				reached = true;
			}
			EXPECT_TRUE(reached);
			EXPECT_EQ(g_forward_count.load(), 1);
		}

		::sigaction(SIGSEGV, &saved_segv, nullptr);
		::sigaction(SIGBUS, &saved_bus, nullptr);
	}

	TEST(BarrierProbe, ForwardsToDefaultDisposition) {
		auto const res = PRIVATEER_SANDBOX {
			static scoped_sigaction const segv{SIGSEGV, chaining_handler};
			static scoped_sigaction const bus{SIGBUS, chaining_handler};
			g_chain_slot = nullptr;
			g_prev_segv = segv.saved;  // SIG_DFL, the sandbox cleared all handlers
			g_prev_bus = bus.saved;
			*reinterpret_cast<int volatile *>(8) = 1;  // unmapped, not ours: must die
			return 1;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST(BarrierProbe, TreatsIgnoredDispositionAsDefault) {
		auto const res = PRIVATEER_SANDBOX {
			::signal(SIGSEGV, SIG_IGN);
			::signal(SIGBUS, SIG_IGN);
			static scoped_sigaction const segv{SIGSEGV, chaining_handler};
			static scoped_sigaction const bus{SIGBUS, chaining_handler};
			g_chain_slot = nullptr;
			g_prev_segv = segv.saved;  // SIG_IGN
			g_prev_bus = bus.saved;
			*reinterpret_cast<int volatile *>(8) = 1;  // must die, not loop on the ignored signal
			return 1;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

}  // namespace
