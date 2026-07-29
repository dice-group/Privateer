// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_FAULT_HANDLER_HPP
#define PRIVATEER_FAULT_HANDLER_HPP

// The process-wide fault handler. It consumes protection faults inside
// registered regions and forwards everything else to the previous
// disposition, following the standard chaining protocol: a saved SA_SIGINFO
// handler gets all three arguments, a plain handler gets the signal number,
// SIG_DFL is forwarded by restoring the default disposition so the retried
// instruction dies honestly, and SIG_IGN on a synchronous fault is treated
// like SIG_DFL (ignoring it would retry-loop forever).
//
// Linux delivers protection faults as SIGSEGV; the handler registers only
// SIGSEGV there. Darwin delivers them as SIGBUS and uses SIGSEGV for
// unmapped addresses; the handler registers both.

#include <privateer/error.hpp>

namespace privateer {

	// Installs the handler once for the process (SA_SIGINFO | SA_ONSTACK),
	// saves the previous dispositions for chaining, and mlocks the handler
	// text range. Idempotent and thread-safe.
	result<> install_fault_handler();

	// true after install_fault_handler succeeded
	bool fault_handler_installed() noexcept;

	// Arms the calling thread with an mlocked alternate signal stack, so the
	// handler never runs on a reclaimable stack page. Idempotent per thread;
	// the stack is released at thread exit.
	result<> arm_thread_fault_stack();

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_fault_handler {

		// Test-only: restores the saved dispositions and forgets the
		// installation, so a test child process can install again with fresh
		// previous dispositions. Never called by the engine.
		void uninstall_for_tests() noexcept;

	}  // namespace detail_fault_handler
#endif

}  // namespace privateer

#endif  // PRIVATEER_FAULT_HANDLER_HPP
