// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_VM_HPP
#define PRIVATEER_VM_HPP

// Virtual-memory primitives of a region: one PROT_NONE reservation whose
// base address never moves while the region is open, and fixed-address
// carve-outs inside it for the header and the slots. Replacing a slot's
// mapping (commit write-out, free) reuses the same carve-out calls: mmap
// with MAP_FIXED swaps the mapping atomically under the kernel address-space
// lock, so a concurrent reader either reads the old bytes or blocks inside
// its fault, and never observes an unmapped window.

#include <privateer/error.hpp>

#include <cstddef>
#include <filesystem>

namespace privateer {

	[[nodiscard]] size_t page_size() noexcept;

	enum struct page_access {
		none,
		read,
		read_write,
	};

	// A PROT_NONE MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE reservation,
	// unmapped on destruction. Carve-outs inside it do not change what the
	// destructor unmaps: munmap over the whole range removes them all.
	struct vm_reservation {
		static result<vm_reservation> reserve(size_t len);

		constexpr vm_reservation() = default;
		vm_reservation(vm_reservation &&other) noexcept;
		vm_reservation &operator=(vm_reservation &&other) noexcept;
		vm_reservation(vm_reservation const &) = delete;
		vm_reservation &operator=(vm_reservation const &) = delete;
		~vm_reservation();

		[[nodiscard]] void *addr() const noexcept { return addr_; }
		[[nodiscard]] size_t size() const noexcept { return len_; }

	private:
		void *addr_ = nullptr;
		size_t len_ = 0;
	};

	// Maps an anonymous range at addr (MAP_FIXED, MAP_PRIVATE). no_reserve
	// skips swap-space accounting for ranges that mostly stay untouched
	// (slots); the region header reserves normally.
	result<> map_anonymous(void *addr, size_t len, page_access access, bool no_reserve = true);

	// Maps a block file read-only at addr (MAP_FIXED, MAP_PRIVATE). The file
	// must be exactly len bytes: a shorter file would turn reads beyond its
	// end into a runtime SIGBUS, so the mismatch fails here as
	// block_file_invalid. The fd is closed right after mmap; the mapping
	// keeps the inode alive.
	result<> map_block_file(void *addr, size_t len, std::filesystem::path const &path);

	result<> protect(void *addr, size_t len, page_access access);

}  // namespace privateer

#endif  // PRIVATEER_VM_HPP
