// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_ERROR_HPP
#define PRIVATEER_ERROR_HPP

// Error codes and the result type of the engine.
// The public adapter API returns bool where metall's contract wants bool,
// with a queryable last_error() on the region; internal functions return
// result<T>. No exceptions cross the adapter boundary, no exit() anywhere.

#include <cerrno>
#include <expected>
#include <string>

namespace privateer {

	enum struct errc : int {
		invalid_argument = 1,
		io_error,                // failed syscall, sys_errno holds the errno
		datastore_missing,       // open of a path that holds no datastore
		datastore_exists,        // create over an existing datastore
		datastore_inconsistent,  // consistency mark absent, or the error flag is set
		recipe_corrupt,          // bad magic, torn write, or checksum mismatch
		recipe_unsupported,      // format version newer than the build, or unknown hash algorithm id
		option_mismatch,         // requested options conflict with the recipe header
		block_file_invalid,      // block file size is not exactly block_size
		memlock_limit_too_low,   // RLIMIT_MEMLOCK cannot hold the mlocked state
		vma_budget_exceeded,     // extend would cross the vm.max_map_count budget
		hash_collision,          // dedup byte-compare found different content under one name
		shutting_down,           // the region is closing
		capacity_exceeded,       // extend beyond the capacity fixed at create
	};

	// short stable name of the code, for logs and messages
	char const *name(errc code) noexcept;

	// An error is a code, the errno of the failed syscall (0 if none), and a
	// static string naming the failed operation. It owns no memory, so it is
	// cheap to copy and to store.
	struct error {
		errc code;
		int sys_errno = 0;
		char const *context = "";
	};

	// one-line message: "<code name>: <context> (errno <n>: <text>)"
	std::string to_string(error const &err);

	template<typename T = void>
	using result = std::expected<T, error>;

	// builds the unexpected arm of a result
	[[nodiscard]] inline std::unexpected<error> fail(errc code, char const *context = "") noexcept {
		return std::unexpected{error{code, 0, context}};
	}

	// like fail, but captures the errno of the syscall that just failed
	[[nodiscard]] inline std::unexpected<error> fail_errno(errc code, char const *context) noexcept {
		return std::unexpected{error{code, errno, context}};
	}

}  // namespace privateer

#endif  // PRIVATEER_ERROR_HPP
