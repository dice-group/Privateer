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

namespace privateer {

	// Darwin flavor of the data barrier. On Linux both values behave the same:
	// fdatasync for files, fsync for directories. On Darwin, full uses
	// fcntl(F_FULLFSYNC), which flushes the drive write cache; plain fsync on
	// Darwin does not, so fsync_only trades durability for speed.
	enum struct sync_policy : int {
		full,
		fsync_only,
	};

	// data barrier for one file
	result<> sync_file(int fd, sync_policy policy) noexcept;

	// entry barrier for one directory: makes renames and links in it durable
	result<> sync_directory(int dirfd, sync_policy policy) noexcept;
	result<> sync_directory(std::filesystem::path const &dir, sync_policy policy);

	// writes all bytes, retrying short writes and EINTR
	result<> write_all(int fd, std::span<std::byte const> data) noexcept;

	// Name prefix of the temp files that back a staged_file where O_TMPFILE is
	// not available. The open-time sweep removes crash leftovers by this
	// prefix; published names must never start with it.
	inline constexpr char temp_name_prefix[] = ".privateer-tmp-";

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
		result<> sync(sync_policy policy) const noexcept;

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
