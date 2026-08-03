// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_FILE_UTIL_HPP
#define PRIVATEER_FILE_UTIL_HPP

// Durable file utilities: data barriers for files and directories, full
// writes, and staged files that appear in their directory only through an
// atomic publication step (linkat or rename).

#include <privateer/error.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#ifdef PRIVATEER_TEST_HOOKS
#include <atomic>
#endif

namespace privateer {

#ifdef PRIVATEER_TEST_HOOKS
	// Test-only hooks, compiled in when the build includes the tests.
	namespace detail_file_util {

		// Counts every file and directory data barrier. Tests reset and read
		// it to observe what a durability path paid.
		extern std::atomic<uint64_t> sync_calls;

		// Counts every staged backing file created. Tests reset and read it
		// to observe whether a path wrote a file at all.
		extern std::atomic<uint64_t> staged_files;

	}  // namespace detail_file_util
#endif  // PRIVATEER_TEST_HOOKS

	// Data barrier for one file: fdatasync on Linux, fsync on macOS.
	// On Linux the barrier writes the data to the device and flushes the
	// device write cache, so it is durable at power loss. On macOS fsync does
	// not flush the device write cache; only fcntl(F_FULLFSYNC) (very slow)
	// does. fsync still survives process and OS crashes, and macOS is not a
	// production target, so plain fsync is used there.
	result<> sync_file(int fd) noexcept;

	// Entry barrier for one directory: makes renames and links in it durable.
	// fsync on both platforms, with the macOS caveat of sync_file.
	result<> sync_directory(int dirfd) noexcept;
	result<> sync_directory(std::filesystem::path const &dir);

	// writes all bytes, retrying short writes and EINTR
	result<> write_all(int fd, std::span<std::byte const> data) noexcept;

	// Name prefix of the temp files that back a staged_file where O_TMPFILE is
	// not available. The open-time sweep removes crash leftovers by this
	// prefix; published names must never start with it.
	inline constexpr char temp_name_prefix[] = ".privateer-tmp-";

	// Removes the staged files a crash left in dir: every entry whose name
	// starts with temp_name_prefix. Returns the number of files removed; a
	// name that cannot be unlinked is logged and left behind. Only the
	// entries of dir itself are examined, subdirectories are not.
	//
	// A temp name belongs to a staged file between its creation and its
	// publication, so this must not run while another process stages a file
	// here: that publication would then fail with ENOENT. Nothing is lost
	// that way (an unpublished file holds nothing anybody can read), but the
	// commit behind it reports an error. The engine takes one read-write
	// opener per datastore and sweeps before its first commit.
	result<size_t> sweep_temp_files(std::filesystem::path const &dir);

	// how a staged_file is backed before publication
	enum struct temp_backing : int {
		automatic,  // O_TMPFILE where available, named otherwise
		named,      // mkstemp file under temp_name_prefix
	};

	// what publish does when the target name already exists
	enum struct publish_mode : int {
		fail_if_exists,  // keep the existing file and report false (the dedup path)
		replace,         // atomically replace the existing file
	};

	// A file staged for atomic publication into one directory. Until publish,
	// the target name does not exist; an anonymous (O_TMPFILE) backing has no
	// directory entry at all. publish links the file under its final name in
	// one atomic step and spends this object; a staged_file destroyed
	// unpublished leaves nothing behind. publish does not sync: callers order
	// write, sync, publish, sync_directory themselves.
	struct staged_file {
		// creates the backing file inside dir
		static result<staged_file> create_in(std::filesystem::path const &dir,
											 temp_backing backing = temp_backing::automatic);

		staged_file(staged_file &&other) noexcept;
		staged_file &operator=(staged_file &&other) noexcept;
		staged_file(staged_file const &) = delete;
		staged_file &operator=(staged_file const &) = delete;
		~staged_file();

		// the writable file descriptor of the backing file
		[[nodiscard]] int fd() const noexcept { return fd_; }

		// true when the backing file has no directory entry (O_TMPFILE)
		[[nodiscard]] bool anonymous() const noexcept { return anonymous_; }

		result<> write(std::span<std::byte const> data) const noexcept;
		result<> sync() const noexcept;

		// Links the file under name in its directory and spends this object.
		// Returns false only in fail_if_exists mode when the name is already
		// taken; the staged backing is discarded in that case too.
		result<bool> publish(std::string const &name, publish_mode mode);

	private:
		staged_file() = default;

		// closes the fd and removes the temp name if one is left
		void discard() noexcept;

		int fd_ = -1;
		bool anonymous_ = false;
		std::filesystem::path dir_;
		std::filesystem::path temp_path_;  // empty for an anonymous backing
	};

}  // namespace privateer

#endif  // PRIVATEER_FILE_UTIL_HPP
