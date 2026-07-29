// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_RESIDENT_HPP
#define PRIVATEER_RESIDENT_HPP

// Process residency accounting for the resident budget. The absolute number
// is the process Pss from /proc/self/smaps_rollup: one coarse read per
// sweep, proportional for shared pages, and it covers the whole process,
// which is why the resident budget is a process-level target. Linux only;
// Darwin has no equivalent and no resident budget.

#include <privateer/error.hpp>

#include <cstdint>
#include <string_view>

namespace privateer {

	// the two smaps_rollup numbers the sweep reads, in bytes
	struct resident_usage {
		uint64_t pss = 0;
		uint64_t private_dirty = 0;
	};

	// Parses smaps_rollup content: "Pss:" and "Private_Dirty:" lines with kB
	// values. Fails when either line is missing.
	result<resident_usage> parse_smaps_rollup(std::string_view content);

	// Reads and parses /proc/self/smaps_rollup. Fails on non-Linux hosts.
	result<resident_usage> read_resident_usage();

}  // namespace privateer

#endif  // PRIVATEER_RESIDENT_HPP
