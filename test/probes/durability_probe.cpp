// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Probe P9: the durability plumbing the block store publication uses.
//
// Linux: O_TMPFILE plus linkat through /proc/self/fd (with mkstemp fallback on
// filesystems without O_TMPFILE), fdatasync, directory fsync, and EEXIST on a
// racing publication (the dedup path relies on it).
// Darwin: plain fsync (no device-cache flush; macOS is not a production target).
// Both: mkstemp plus rename publication under a directory fsync.
//
// Overlayfs power-cut durability stays a documented manual procedure: a
// kill-based probe cannot see past the page cache.

#include <gtest/gtest.h>

#include "probe_support.hpp"

#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace privateer::probes;

namespace {

	// mkdtemp'd directory, removed recursively on destruction
	struct temp_dir {
		std::string path;
		int dirfd = -1;

		temp_dir() {
			std::string tmpl = (std::filesystem::temp_directory_path() / "privateer-probe-XXXXXX").string();
			std::vector<char> name(tmpl.begin(), tmpl.end());
			name.push_back('\0');
			if (::mkdtemp(name.data()) == nullptr) {
				throw std::system_error{errno, std::system_category(), "mkdtemp"};
			}
			path = name.data();
			dirfd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
			if (dirfd < 0) {
				throw std::system_error{errno, std::system_category(), "open dir"};
			}
		}

		temp_dir(temp_dir const &) = delete;
		temp_dir &operator=(temp_dir const &) = delete;

		~temp_dir() {
			if (dirfd >= 0) {
				::close(dirfd);
			}
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	// Data barrier for one file: fdatasync on Linux, fsync on Darwin. macOS
	// fsync does not flush the device write cache (only the very slow
	// fcntl(F_FULLFSYNC) does) but survives process and OS crashes; macOS is
	// not a production target, so the engine uses plain fsync there.
	int data_sync(int fd) {
#ifdef __linux__
		return ::fdatasync(fd);
#else
		return ::fsync(fd);
#endif
	}

	void write_all(int fd, char const *data, size_t len) {
		size_t written = 0;
		while (written < len) {
			ssize_t const n = ::write(fd, data + written, len - written);
			ASSERT_GE(n, 0) << "write failed: " << errno;
			written += static_cast<size_t>(n);
		}
	}

	std::string read_file(std::string const &path) {
		int const fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0) {
			return {};
		}
		char buf[256];
		ssize_t const n = ::read(fd, buf, sizeof(buf));
		::close(fd);
		return n > 0 ? std::string{buf, static_cast<size_t>(n)} : std::string{};
	}

#ifdef __linux__

	TEST(DurabilityProbe, TmpfileLinkatPublishes) {
		temp_dir dir;
		int const fd = ::open(dir.path.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0) {
			// EOPNOTSUPP on NFS and others: the mkstemp fallback below covers those
			GTEST_SKIP() << "O_TMPFILE unsupported here, errno " << errno;
		}

		char const content[] = "block-content";
		write_all(fd, content, sizeof(content) - 1);
		ASSERT_EQ(data_sync(fd), 0);

		// nothing is visible in the directory before linkat
		EXPECT_TRUE(std::filesystem::is_empty(dir.path));

		// the unprivileged idiom goes through /proc/self/fd and needs procfs mounted
		char proc_path[64];
		std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
		ASSERT_EQ(::linkat(AT_FDCWD, proc_path, dir.dirfd, "block", AT_SYMLINK_FOLLOW), 0)
				<< "linkat via /proc/self/fd failed: " << errno;
		ASSERT_EQ(::fsync(dir.dirfd), 0);

		EXPECT_EQ(read_file(dir.path + "/block"), content);

		// publication of the same name loses with EEXIST; dedup relies on that
		EXPECT_NE(::linkat(AT_FDCWD, proc_path, dir.dirfd, "block", AT_SYMLINK_FOLLOW), 0);
		EXPECT_EQ(errno, EEXIST);

		::close(fd);
	}

#endif

	TEST(DurabilityProbe, MkstempRenamePublishes) {
		temp_dir dir;
		std::string tmpl = dir.path + "/tmp-XXXXXX";
		std::vector<char> name(tmpl.begin(), tmpl.end());
		name.push_back('\0');
		int const fd = ::mkstemp(name.data());
		ASSERT_GE(fd, 0);

		char const content[] = "block-content";
		write_all(fd, content, sizeof(content) - 1);
		ASSERT_EQ(data_sync(fd), 0);

		std::string const final_path = dir.path + "/block";
		ASSERT_EQ(::rename(name.data(), final_path.c_str()), 0);
		ASSERT_EQ(::fsync(dir.dirfd), 0);

		EXPECT_EQ(read_file(final_path), content);
		::close(fd);
	}

	TEST(DurabilityProbe, DirectoryFsyncSucceeds) {
		temp_dir dir;
		EXPECT_EQ(::fsync(dir.dirfd), 0);
	}

}  // namespace
