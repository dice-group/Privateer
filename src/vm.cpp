// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/vm.hpp>

#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace privateer {

	namespace {

		int prot_of(page_access access) noexcept {
			switch (access) {
				case page_access::none: return PROT_NONE;
				case page_access::read: return PROT_READ;
				case page_access::read_write: return PROT_READ | PROT_WRITE;
			}
			return PROT_NONE;
		}

	}  // namespace

	size_t page_size() noexcept {
		static size_t const value = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
		return value;
	}

	result<vm_reservation> vm_reservation::reserve(size_t len) {
		if (len == 0 || len % page_size() != 0) {
			return fail(errc::invalid_argument, "reservation length is not a positive page multiple");
		}
		void *const addr = ::mmap(nullptr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (addr == MAP_FAILED) {
			return fail_errno(errc::io_error, "reserve the region address range");
		}
		vm_reservation reservation;
		reservation.addr_ = addr;
		reservation.len_ = len;
		return reservation;
	}

	vm_reservation::vm_reservation(vm_reservation &&other) noexcept
		: addr_{std::exchange(other.addr_, nullptr)}, len_{std::exchange(other.len_, 0)} {}

	vm_reservation &vm_reservation::operator=(vm_reservation &&other) noexcept {
		if (this != &other) {
			this->~vm_reservation();
			addr_ = std::exchange(other.addr_, nullptr);
			len_ = std::exchange(other.len_, 0);
		}
		return *this;
	}

	vm_reservation::~vm_reservation() {
		if (addr_ != nullptr) {
			::munmap(addr_, len_);
			addr_ = nullptr;
			len_ = 0;
		}
	}

	result<> map_anonymous(void *addr, size_t len, page_access access, bool no_reserve) {
		int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
		if (no_reserve) {
			flags |= MAP_NORESERVE;
		}
		if (::mmap(addr, len, prot_of(access), flags, -1, 0) == MAP_FAILED) {
			return fail_errno(errc::io_error, "map an anonymous range");
		}
		return {};
	}

	result<> map_block_file(void *addr, size_t len, std::filesystem::path const &path) {
		int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return fail_errno(errc::io_error, "open block file for mapping");
		}
		struct stat st {};
		if (::fstat(fd, &st) != 0) {
			auto const err = fail_errno(errc::io_error, "stat block file for mapping");
			::close(fd);
			return err;
		}
		if (!S_ISREG(st.st_mode) || std::cmp_not_equal(st.st_size, len)) {
			::close(fd);
			return fail(errc::block_file_invalid, "block file is not exactly the mapped length");
		}
		void *const mapped = ::mmap(addr, len, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
		int const mmap_errno = errno;
		::close(fd);
		if (mapped == MAP_FAILED) {
			errno = mmap_errno;
			return fail_errno(errc::io_error, "map block file");
		}
		return {};
	}

	result<> protect(void *addr, size_t len, page_access access) {
		if (::mprotect(addr, len, prot_of(access)) != 0) {
			return fail_errno(errc::io_error, "change a mapping's protection");
		}
		return {};
	}

}  // namespace privateer
