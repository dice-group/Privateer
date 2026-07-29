// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/error.hpp>

#include <format>
#include <system_error>

namespace privateer {

	char const *name(errc code) noexcept {
		switch (code) {
			case errc::invalid_argument: return "invalid_argument";
			case errc::io_error: return "io_error";
			case errc::datastore_missing: return "datastore_missing";
			case errc::datastore_exists: return "datastore_exists";
			case errc::datastore_inconsistent: return "datastore_inconsistent";
			case errc::recipe_corrupt: return "recipe_corrupt";
			case errc::recipe_unsupported: return "recipe_unsupported";
			case errc::option_mismatch: return "option_mismatch";
			case errc::block_file_invalid: return "block_file_invalid";
			case errc::memlock_limit_too_low: return "memlock_limit_too_low";
			case errc::vma_budget_exceeded: return "vma_budget_exceeded";
			case errc::hash_collision: return "hash_collision";
			case errc::shutting_down: return "shutting_down";
			case errc::capacity_exceeded: return "capacity_exceeded";
		}
		return "unknown_error";
	}

	std::string to_string(error const &err) {
		std::string out{name(err.code)};
		if (err.context != nullptr && err.context[0] != '\0') {
			out += ": ";
			out += err.context;
		}
		if (err.sys_errno != 0) {
			// std::system_category().message is thread safe, unlike strerror
			out += std::format(" (errno {}: {})", err.sys_errno, std::system_category().message(err.sys_errno));
		}
		return out;
	}

}  // namespace privateer
