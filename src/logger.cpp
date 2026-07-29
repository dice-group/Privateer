// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/logger.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

	std::atomic<int> default_min_level{static_cast<int>(privateer::log_level::warning)};

	char const *level_name(metall_log_level lvl) noexcept {
		switch (lvl) {
			case metall_verbose: return "verbose";
			case metall_debug: return "debug";
			case metall_info: return "info";
			case metall_warning: return "warning";
			case metall_error: return "error";
			case metall_critical: return "critical";
		}
		return "unknown";
	}

	char const *basename_of(char const *path) noexcept {
		char const *slash = std::strrchr(path, '/');
		return slash != nullptr ? slash + 1 : path;
	}

}  // namespace

namespace privateer {

	void set_default_log_min_level(log_level lvl) noexcept {
		default_min_level.store(static_cast<int>(lvl), std::memory_order_relaxed);
	}

}  // namespace privateer

// Built-in sink: one line per message to stderr, filtered by the minimum
// level. Weak, so a strong metall_log anywhere in the process replaces it.
extern "C" __attribute__((weak)) void metall_log(metall_log_level lvl, char const *file_name, size_t line_no, char const *message) {
	if (static_cast<int>(lvl) < default_min_level.load(std::memory_order_relaxed)) {
		return;
	}
	// one fprintf call per message, so concurrent lines do not interleave
	std::fprintf(stderr, "privateer [%s] %s:%zu: %s\n", level_name(lvl), basename_of(file_name), line_no, message);
}
