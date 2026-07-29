// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_RLIMITS_HPP
#define PRIVATEER_RLIMITS_HPP

// Resource limit handling at engine init.

#include <privateer/error.hpp>

#include <cstddef>

namespace privateer {

	// Raises the RLIMIT_MEMLOCK soft limit until at_least bytes are lockable.
	// Fails with memlock_limit_too_low when the hard limit is below at_least:
	// running with an unlockable state array is a correctness hazard (a
	// reclaimed state page touched in the fault handler kills the process),
	// so the engine refuses instead of degrading. Swapless deployments can
	// skip locking instead (mlocked_buffer::allocate with lock false).
	result<> ensure_memlock_limit(size_t at_least) noexcept;

	// Darwin only: raises the RLIMIT_NOFILE soft limit toward the hard limit
	// (capped at OPEN_MAX). The Darwin default soft limit of 256 is too small
	// for a sharded store. On Linux the default is sufficient; no-op there.
	result<> raise_nofile_limit() noexcept;

}  // namespace privateer

#endif  // PRIVATEER_RLIMITS_HPP
