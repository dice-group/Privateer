// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/file_util.hpp>

#include <privateer/logger.hpp>

#include <atomic>
#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace privateer {

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_file_util {

		std::atomic<uint64_t> sync_calls{0};
		std::atomic<uint64_t> staged_files{0};

	}  // namespace detail_file_util
#endif

	namespace {

		void count_sync() noexcept {
#ifdef PRIVATEER_TEST_HOOKS
			detail_file_util::sync_calls.fetch_add(1, std::memory_order_relaxed);
#endif
		}

		void count_staged_file() noexcept {
#ifdef PRIVATEER_TEST_HOOKS
			detail_file_util::staged_files.fetch_add(1, std::memory_order_relaxed);
#endif
		}

		// fsync with EINTR retry
		int fsync_retry(int fd) noexcept {
			int rc;
			do {
				rc = ::fsync(fd);
			} while (rc != 0 && errno == EINTR);
			return rc;
		}

	}  // namespace

	result<> sync_file(int fd) noexcept {
		count_sync();
#ifdef __linux__
		int rc;
		do {
			rc = ::fdatasync(fd);
		} while (rc != 0 && errno == EINTR);
		if (rc != 0) {
			return fail_errno(errc::io_error, "fdatasync");
		}
		return {};
#else
		if (fsync_retry(fd) != 0) {
			return fail_errno(errc::io_error, "fsync");
		}
		return {};
#endif
	}

	result<> sync_directory(int dirfd) noexcept {
		count_sync();
		if (fsync_retry(dirfd) != 0) {
			return fail_errno(errc::io_error, "fsync directory");
		}
		return {};
	}

	result<> sync_directory(std::filesystem::path const &dir) {
		int const dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dirfd < 0) {
			return fail_errno(errc::io_error, "open directory");
		}
		auto res = sync_directory(dirfd);
		::close(dirfd);
		return res;
	}

	result<> write_all(int fd, std::span<std::byte const> data) noexcept {
		size_t written = 0;
		while (written < data.size()) {
			ssize_t const n = ::write(fd, data.data() + written, data.size() - written);
			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				return fail_errno(errc::io_error, "write");
			}
			written += static_cast<size_t>(n);
		}
		return {};
	}

	result<size_t> sweep_temp_files(std::filesystem::path const &dir) {
		size_t removed = 0;
		std::error_code ec;
		for (auto it = std::filesystem::directory_iterator{dir, ec};
			 !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
			if (!it->path().filename().string().starts_with(temp_name_prefix)) {
				continue;
			}
			if (::unlink(it->path().c_str()) != 0 && errno != ENOENT) {
				PRIVATEER_LOG(log_level::warning, "sweep cannot unlink {} (errno {})", it->path().string(),
							  errno);
				continue;
			}
			++removed;
		}
		if (ec) {
			return std::unexpected{error{errc::io_error, ec.value(), "list a directory for the sweep"}};
		}
		return removed;
	}

	result<staged_file> staged_file::create_in(std::filesystem::path const &dir, temp_backing backing) {
		staged_file f;
		f.dir_ = dir;
#ifdef __linux__
		if (backing == temp_backing::automatic) {
			int const fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0644);
			if (fd >= 0) {
				// the unprivileged linkat publication goes through
				// /proc/self/fd, so procfs must be mounted
				if (::access("/proc/self/fd", F_OK) == 0) {
					f.fd_ = fd;
					f.anonymous_ = true;
					count_staged_file();
					return f;
				}
				::close(fd);
			} else if (errno != EOPNOTSUPP && errno != EISDIR && errno != EINVAL && errno != ENOSYS) {
				// those four mean no O_TMPFILE on this filesystem or kernel
				// and select the named fallback; everything else is a real error
				return fail_errno(errc::io_error, "open O_TMPFILE");
			}
		}
#else
		(void) backing;
#endif
		std::string const templ = (dir / (std::string{temp_name_prefix} + "XXXXXX")).string();
		std::vector<char> name{templ.begin(), templ.end()};
		name.push_back('\0');
		int const fd = ::mkostemp(name.data(), O_CLOEXEC);
		if (fd < 0) {
			return fail_errno(errc::io_error, "mkstemp");
		}
		f.fd_ = fd;
		f.temp_path_ = name.data();
		count_staged_file();
		return f;
	}

	staged_file::staged_file(staged_file &&other) noexcept
		: fd_{std::exchange(other.fd_, -1)},
		  anonymous_{std::exchange(other.anonymous_, false)},
		  dir_{std::move(other.dir_)},
		  temp_path_{std::move(other.temp_path_)} {
		other.dir_.clear();
		other.temp_path_.clear();
	}

	staged_file &staged_file::operator=(staged_file &&other) noexcept {
		if (this != &other) {
			discard();
			fd_ = std::exchange(other.fd_, -1);
			anonymous_ = std::exchange(other.anonymous_, false);
			dir_ = std::move(other.dir_);
			temp_path_ = std::move(other.temp_path_);
			other.dir_.clear();
			other.temp_path_.clear();
		}
		return *this;
	}

	staged_file::~staged_file() {
		discard();
	}

	void staged_file::discard() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
		if (!temp_path_.empty()) {
			::unlink(temp_path_.c_str());
			temp_path_.clear();
		}
		anonymous_ = false;
	}

	result<> staged_file::write(std::span<std::byte const> data) const noexcept {
		return write_all(fd_, data);
	}

	result<> staged_file::sync() const noexcept {
		return sync_file(fd_);
	}

	result<bool> staged_file::publish(std::string const &name, publish_mode mode) {
		if (fd_ < 0) {
			return fail(errc::invalid_argument, "publish on a spent staged_file");
		}
		if (name.empty() || name.find('/') != std::string::npos || name.starts_with(temp_name_prefix)) {
			return fail(errc::invalid_argument, "publish name");
		}
		std::filesystem::path const target = dir_ / name;

#ifdef __linux__
		if (anonymous_) {
			char proc_path[64];
			std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd_);

			if (mode == publish_mode::fail_if_exists) {
				if (::linkat(AT_FDCWD, proc_path, AT_FDCWD, target.c_str(), AT_SYMLINK_FOLLOW) == 0) {
					discard();
					return true;
				}
				if (errno == EEXIST) {
					discard();
					return false;
				}
				auto const err = fail_errno(errc::io_error, "linkat");
				discard();
				return err;
			}

			// linkat cannot replace an existing name: link under a unique temp
			// name first, then rename over the target
			static std::atomic<unsigned> replace_counter{0};
			std::filesystem::path linked;
			for (int attempt = 0; attempt < 64 && linked.empty(); ++attempt) {
				auto const candidate = dir_ / (std::string{temp_name_prefix} + std::to_string(::getpid()) + "-" +
											   std::to_string(replace_counter.fetch_add(1, std::memory_order_relaxed)));
				if (::linkat(AT_FDCWD, proc_path, AT_FDCWD, candidate.c_str(), AT_SYMLINK_FOLLOW) == 0) {
					linked = candidate;
				} else if (errno != EEXIST) {
					auto const err = fail_errno(errc::io_error, "linkat");
					discard();
					return err;
				}
			}
			if (linked.empty()) {
				discard();
				return fail(errc::io_error, "linkat temp name");
			}
			if (::rename(linked.c_str(), target.c_str()) != 0) {
				auto const err = fail_errno(errc::io_error, "rename");
				::unlink(linked.c_str());
				discard();
				return err;
			}
			discard();
			return true;
		}
#endif

		if (mode == publish_mode::fail_if_exists) {
			// link keeps fail-on-exist semantics that rename does not have
			if (::link(temp_path_.c_str(), target.c_str()) == 0) {
				discard();
				return true;
			}
			if (errno == EEXIST) {
				discard();
				return false;
			}
			auto const err = fail_errno(errc::io_error, "link");
			discard();
			return err;
		}

		if (::rename(temp_path_.c_str(), target.c_str()) != 0) {
			auto const err = fail_errno(errc::io_error, "rename");
			discard();
			return err;
		}
		temp_path_.clear();  // the temp name now is the target name; only the fd is left to close
		discard();
		return true;
	}

}  // namespace privateer
