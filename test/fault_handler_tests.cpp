// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Tests for the process-wide fault handler: installation, the alternate
// stack, a handled fault inside a registered region, the chaining matrix for
// unowned faults (previous SA_SIGINFO handler, plain handler, SIG_DFL,
// SIG_IGN), and remove() draining while another region takes a fault storm.

#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region_registry.hpp>
#include <privateer/rlimits.hpp>

#include "support/sandbox.hpp"

#include <atomic>
#include <csignal>
#include <system_error>
#include <thread>

#include <sys/mman.h>
#include <unistd.h>

using namespace privateer;
using privateer::testing::is_fault_signal;
using privateer::testing::subprocess_result;

namespace {

	size_t page_size() {
		static size_t const value = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
		return value;
	}

	// The handler text, the registry tables, and two alternate stacks must be
	// lockable; a container default of 64 KiB is not enough. The refuse path
	// itself is covered in memlock_tests.cpp.
	void require_memlock_budget() {
		if (auto r = ensure_memlock_limit(size_t{4} * 1024 * 1024); !r) {
			GTEST_SKIP() << "RLIMIT_MEMLOCK too low for the handler tests: " << to_string(r.error());
		}
	}

	// anonymous mapping, unmapped on destruction
	struct anon_mapping {
		void *addr = nullptr;
		size_t len = 0;

		anon_mapping(size_t len, int prot) : len{len} {
			addr = ::mmap(nullptr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (addr == MAP_FAILED) {
				throw std::system_error{errno, std::system_category(), "mmap"};
			}
		}

		anon_mapping(anon_mapping const &) = delete;
		anon_mapping &operator=(anon_mapping const &) = delete;

		~anon_mapping() {
			::munmap(addr, len);
		}

		[[nodiscard]] unsigned char volatile *bytes() const {
			return static_cast<unsigned char volatile *>(addr);
		}

		[[nodiscard]] uintptr_t base() const {
			return reinterpret_cast<uintptr_t>(addr);
		}
	};

	std::atomic<int> g_handled_count{0};
	std::atomic<bool> g_on_alt_stack{false};
	std::atomic<int> g_signo{0};

	// async-signal-safe: records how the handler ran and upgrades the faulting page
	extern "C" bool upgrading_on_fault(region_record &, uintptr_t addr, int signo) {
		stack_t ss{};
		::sigaltstack(nullptr, &ss);
		g_on_alt_stack.store((ss.ss_flags & SS_ONSTACK) != 0, std::memory_order_relaxed);
		g_signo.store(signo, std::memory_order_relaxed);
		g_handled_count.fetch_add(1, std::memory_order_relaxed);
		uintptr_t const page = addr & ~(page_size() - 1);
		return ::mprotect(reinterpret_cast<void *>(page), page_size(), PROT_READ | PROT_WRITE) == 0;
	}

	TEST(FaultHandler, InstallIsIdempotent) {
		require_memlock_budget();
		EXPECT_TRUE(install_fault_handler());
		EXPECT_TRUE(fault_handler_installed());
		EXPECT_TRUE(install_fault_handler());
	}

	TEST(FaultHandler, ArmThreadFaultStackIsIdempotent) {
		require_memlock_budget();
		EXPECT_TRUE(arm_thread_fault_stack());
		EXPECT_TRUE(arm_thread_fault_stack());
	}

	TEST(FaultHandler, HandledFaultUpgradesAndRetries) {
		require_memlock_budget();
		ASSERT_TRUE(install_fault_handler());
		ASSERT_TRUE(arm_thread_fault_stack());

		anon_mapping m{page_size(), PROT_READ};
		region_record rec;
		rec.on_fault = upgrading_on_fault;
		ASSERT_TRUE(global_registry().add(rec, m.base(), m.base() + m.len));
		g_handled_count.store(0);
		g_on_alt_stack.store(false);

		m.bytes()[0] = 42;  // one barrier fault, upgraded, retried
		EXPECT_EQ(m.bytes()[0], 42);
		EXPECT_EQ(g_handled_count.load(), 1);
		EXPECT_TRUE(g_on_alt_stack.load());
#ifdef __linux__
		EXPECT_EQ(g_signo.load(), SIGSEGV);
#else
		EXPECT_EQ(g_signo.load(), SIGBUS);
#endif

		m.bytes()[1] = 7;  // the page is writable now, no second fault
		EXPECT_EQ(g_handled_count.load(), 1);
		EXPECT_EQ(rec.handler_in_flight.load(), 0u);

		global_registry().remove(rec);
	}

	// ---- chaining matrix: each case needs its own previous disposition,
	// so each runs in a sandbox child that installs the handler freshly ----

	std::atomic<int> g_prev_ran{0};

	extern "C" void repairing_prev_handler(int, siginfo_t *si, void *) {
		g_prev_ran.fetch_add(1, std::memory_order_relaxed);
		uintptr_t const page = reinterpret_cast<uintptr_t>(si->si_addr) & ~(page_size() - 1);
		::mprotect(reinterpret_cast<void *>(page), page_size(), PROT_READ | PROT_WRITE);
	}

	extern "C" void exiting_plain_handler(int) {
		::_exit(0);
	}

	TEST(FaultHandlerChaining, ForwardsToASavedSiginfoHandler) {
		require_memlock_budget();
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();
			struct sigaction sa {};
			sa.sa_sigaction = repairing_prev_handler;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = SA_SIGINFO;
			::sigaction(SIGSEGV, &sa, nullptr);
			::sigaction(SIGBUS, &sa, nullptr);
			if (!install_fault_handler() || !arm_thread_fault_stack()) {
				return 2;
			}
			anon_mapping m{page_size(), PROT_READ};
			m.bytes()[0] = 1;  // unowned: forwarded, the previous handler repairs
			if (g_prev_ran.load() != 1) {
				return 3;
			}
			if (m.bytes()[0] != 1) {
				return 4;
			}
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

	TEST(FaultHandlerChaining, ForwardsToASavedPlainHandler) {
		require_memlock_budget();
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();
			struct sigaction sa {};
			sa.sa_handler = exiting_plain_handler;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = 0;
			::sigaction(SIGSEGV, &sa, nullptr);
			::sigaction(SIGBUS, &sa, nullptr);
			if (!install_fault_handler() || !arm_thread_fault_stack()) {
				return 2;
			}
			anon_mapping m{page_size(), PROT_READ};
			m.bytes()[0] = 1;  // forwarded to the plain handler, which exits 0
			return 5;
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
	}

	TEST(FaultHandlerChaining, ForwardsToTheDefaultDisposition) {
		require_memlock_budget();
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();
			::signal(SIGSEGV, SIG_DFL);
			::signal(SIGBUS, SIG_DFL);
			if (!install_fault_handler() || !arm_thread_fault_stack()) {
				return 2;
			}
			anon_mapping m{page_size(), PROT_READ};
			m.bytes()[0] = 1;  // unowned, previous disposition SIG_DFL: must die
			return 5;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST(FaultHandlerChaining, TreatsAnIgnoredDispositionAsDefault) {
		require_memlock_budget();
		auto const res = PRIVATEER_SANDBOX {
			detail_fault_handler::uninstall_for_tests();
			::signal(SIGSEGV, SIG_IGN);
			::signal(SIGBUS, SIG_IGN);
			if (!install_fault_handler() || !arm_thread_fault_stack()) {
				return 2;
			}
			anon_mapping m{page_size(), PROT_READ};
			m.bytes()[0] = 1;  // must die, not retry-loop on the ignored signal
			return 5;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	// ---- the epoch-pair gate under load ----

	extern "C" bool storm_on_fault(region_record &, uintptr_t addr, int) {
		uintptr_t const page = addr & ~(page_size() - 1);
		return ::mprotect(reinterpret_cast<void *>(page), page_size(), PROT_READ | PROT_WRITE) == 0;
	}

	TEST(FaultHandler, RemoveDrainsWhileAnotherRegionTakesFaults) {
		require_memlock_budget();
		ASSERT_TRUE(install_fault_handler());
		ASSERT_TRUE(arm_thread_fault_stack());

		anon_mapping stormed{page_size(), PROT_READ};
		region_record storm_rec;
		storm_rec.on_fault = storm_on_fault;
		ASSERT_TRUE(global_registry().add(storm_rec, stormed.base(), stormed.base() + stormed.len));

		// address space for the churned region; PROT_NONE, never touched
		anon_mapping placeholder{page_size(), PROT_NONE};

		std::atomic<bool> stop{false};
		std::atomic<bool> storm_dead{false};
		std::atomic<uint64_t> faults{0};
		std::thread storm{[&] {
			if (!arm_thread_fault_stack()) {
				storm_dead.store(true, std::memory_order_release);
				return;
			}
			while (!stop.load(std::memory_order_acquire)) {
				::mprotect(stormed.addr, stormed.len, PROT_READ);
				stormed.bytes()[0] = 1;  // one barrier fault per iteration
				faults.fetch_add(1, std::memory_order_relaxed);
			}
		}};

		// The churn must overlap live faults, so it starts only after the
		// storm took its first one; without this wait the 200 add/remove
		// pairs can finish before the storm thread is even scheduled.
		while (faults.load(std::memory_order_acquire) == 0 &&
			   !storm_dead.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
		if (storm_dead.load(std::memory_order_acquire)) {
			storm.join();
			global_registry().remove(storm_rec);
			FAIL() << "the storm thread could not arm its fault stack";
		}

		// every remove flips the gate epoch and must drain despite the storm
		region_record churn_rec;
		for (int i = 0; i < 200; ++i) {
			ASSERT_TRUE(global_registry().add(churn_rec, placeholder.base(), placeholder.base() + placeholder.len));
			global_registry().remove(churn_rec);
		}

		stop.store(true, std::memory_order_release);
		storm.join();
		EXPECT_GT(faults.load(), 0u);
		EXPECT_EQ(storm_rec.handler_in_flight.load(), 0u);
		global_registry().remove(storm_rec);
	}

}  // namespace
