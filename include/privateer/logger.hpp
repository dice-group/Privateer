// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_LOGGER_HPP
#define PRIVATEER_LOGGER_HPP

// Logging goes through the metall logger hook (logger_interface.h), so a
// consumer that already provides metall_log receives privateer's messages
// through the same sink. Without a consumer-provided sink, the weak default
// in src/logger.cpp prints to stderr with a level filter.
//
// Not async-signal-safe. The signal handler never logs; it only sets the
// error flag. Log sites are outside the handler.

#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include <privateer/logger_interface.h>

namespace privateer {

	// same numeric values as metall_log_level
	enum struct log_level : int {
		verbose = metall_verbose,
		debug = metall_debug,
		info = metall_info,
		warning = metall_warning,
		error = metall_error,
		critical = metall_critical,
	};

	// minimum level the built-in stderr sink prints, default warning;
	// has no effect when the consumer provides its own metall_log
	void set_default_log_min_level(log_level lvl) noexcept;

	namespace detail_logger {

		inline void emit(log_level lvl, char const *file, size_t line, char const *message) noexcept {
			::metall_log(static_cast<metall_log_level>(lvl), file, line, message);
		}

		inline void emit(log_level lvl, char const *file, size_t line, std::string const &message) noexcept {
			::metall_log(static_cast<metall_log_level>(lvl), file, line, message.c_str());
		}

		template<typename... Args>
			requires(sizeof...(Args) > 0)
		void emit(log_level lvl, char const *file, size_t line, std::format_string<Args...> fmt, Args &&...args) {
			std::string const message = std::format(fmt, std::forward<Args>(args)...);
			::metall_log(static_cast<metall_log_level>(lvl), file, line, message.c_str());
		}

	}  // namespace detail_logger

}  // namespace privateer

// PRIVATEER_LOG(level, "message") or PRIVATEER_LOG(level, "x = {}", x)
#define PRIVATEER_LOG(level_, ...) \
	::privateer::detail_logger::emit((level_), __FILE__, __LINE__, __VA_ARGS__)

#endif  // PRIVATEER_LOGGER_HPP
