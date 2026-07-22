#pragma once

// Shared helpers for the platform probes: temp files, mappings, sigaction
// guards, and the in-handler wait primitive the write barrier will use
// (timed compare-value futex on Linux, nanosleep backoff spin on Darwin).

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#endif

// sanitizer detection: gcc defines __SANITIZE_*, clang answers __has_feature
#ifdef __SANITIZE_ADDRESS__
#define PRIVATEER_PROBE_ASAN 1
#endif
#ifdef __SANITIZE_THREAD__
#define PRIVATEER_PROBE_TSAN 1
#endif
#ifdef __has_feature
#if __has_feature(address_sanitizer)
#define PRIVATEER_PROBE_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define PRIVATEER_PROBE_TSAN 1
#endif
#endif

namespace privateer::probes {

	inline size_t page_size() {
		static size_t const value = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
		return value;
	}

#ifdef __linux__
	inline constexpr int barrier_signal = SIGSEGV;
#else
	inline constexpr int barrier_signal = SIGBUS;
#endif

	// monotonic clock in nanoseconds; safe to call from a signal handler
	// (vDSO clock_gettime on Linux, commpage-backed clock_gettime_nsec_np on Darwin)
	inline int64_t monotonic_ns() {
#ifdef __linux__
		timespec ts;
		::clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#else
		return static_cast<int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#endif
	}

	// Waits until word becomes non-zero, bounded by one monotonic deadline that
	// survives EINTR and spurious wakeups. Async-signal-safe on both platforms.
	// Returns true if woken (word non-zero), false on timeout.
	inline bool wait_for_word(std::atomic<uint32_t> &word, int64_t timeout_ns) {
		int64_t const deadline = monotonic_ns() + timeout_ns;
#ifdef __linux__
		while (word.load(std::memory_order_acquire) == 0) {
			int64_t const remaining = deadline - monotonic_ns();
			if (remaining <= 0) {
				return false;
			}
			timespec rel{};
			rel.tv_sec = remaining / 1'000'000'000;
			rel.tv_nsec = remaining % 1'000'000'000;
			// 0: woken; EAGAIN: value already changed; EINTR or ETIMEDOUT: re-check against the deadline
			::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAIT_PRIVATE, 0u, &rel, nullptr, 0);
		}
		return true;
#else
		timespec const backoff{0, 1'000'000};
		while (word.load(std::memory_order_acquire) == 0) {
			if (monotonic_ns() >= deadline) {
				return false;
			}
			::nanosleep(&backoff, nullptr);  // EINTR re-loops against the deadline
		}
		return true;
#endif
	}

	inline void wake_all(std::atomic<uint32_t> &word) {
#ifdef __linux__
		::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
#else
		(void) word;  // the Darwin waiter polls
#endif
	}

	// unlinked-on-destruction temp file of len bytes, every byte set to fill, synced to disk
	struct temp_file {
		int fd = -1;
		std::string path;

		temp_file(size_t len, unsigned char fill) {
			std::string tmpl = (std::filesystem::temp_directory_path() / "privateer-probe-XXXXXX").string();
			std::vector<char> name(tmpl.begin(), tmpl.end());
			name.push_back('\0');
			fd = ::mkstemp(name.data());
			if (fd < 0) {
				throw std::system_error{errno, std::system_category(), "mkstemp"};
			}
			path = name.data();
			std::vector<unsigned char> const buf(len, fill);
			size_t written = 0;
			while (written < len) {
				ssize_t const n = ::write(fd, buf.data() + written, len - written);
				if (n < 0) {
					throw std::system_error{errno, std::system_category(), "write"};
				}
				written += static_cast<size_t>(n);
			}
			if (::fsync(fd) != 0) {
				throw std::system_error{errno, std::system_category(), "fsync"};
			}
		}

		temp_file(temp_file const &) = delete;
		temp_file &operator=(temp_file const &) = delete;

		~temp_file() {
			if (fd >= 0) {
				::close(fd);
				::unlink(path.c_str());
			}
		}
	};

	// munmapped-on-destruction mapping
	struct mapping {
		void *addr = MAP_FAILED;
		size_t len = 0;

		static mapping map_file(int fd, size_t len, int prot) {
			mapping m;
			m.addr = ::mmap(nullptr, len, prot, MAP_PRIVATE, fd, 0);
			if (m.addr == MAP_FAILED) {
				throw std::system_error{errno, std::system_category(), "mmap"};
			}
			m.len = len;
			return m;
		}

		mapping() = default;
		mapping(mapping &&other) noexcept : addr{other.addr}, len{other.len} {
			other.addr = MAP_FAILED;
			other.len = 0;
		}
		mapping(mapping const &) = delete;
		mapping &operator=(mapping const &) = delete;
		mapping &operator=(mapping &&) = delete;

		~mapping() {
			if (addr != MAP_FAILED) {
				::munmap(addr, len);
			}
		}

		unsigned char volatile *bytes() const {
			return static_cast<unsigned char volatile *>(addr);
		}
	};

	// installs a SA_SIGINFO handler and restores the previous disposition on destruction
	struct scoped_sigaction {
		int signo;
		struct sigaction saved {};

		scoped_sigaction(int signo, void (*fn)(int, siginfo_t *, void *)) : signo{signo} {
			struct sigaction sa {};
			sa.sa_sigaction = fn;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = SA_SIGINFO;
			if (::sigaction(signo, &sa, &saved) != 0) {
				throw std::system_error{errno, std::system_category(), "sigaction"};
			}
		}

		scoped_sigaction(scoped_sigaction const &) = delete;
		scoped_sigaction &operator=(scoped_sigaction const &) = delete;

		~scoped_sigaction() {
			::sigaction(signo, &saved, nullptr);
		}
	};

}  // namespace privateer::probes
