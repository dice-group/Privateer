#ifndef PRIVATEER_REGION_HPP
#define PRIVATEER_REGION_HPP

// A region is one open datastore mapped as one contiguous VM reservation:
//
//   [ segment header | slot 0 | slot 1 | ... ]
//
// The base address never changes while the region is open; all offset
// pointers hang off the segment start. The header is a private anonymous
// read-write mapping, volatile, never persisted. Each slot is block_size
// bytes and maps its recipe entry: a block file read-only, or anonymous
// zeros for the empty sentinel. Slots beyond the extended size stay
// PROT_NONE. Every slot mapping is read-only; writes go through the fault
// handler once it is registered for the region.

#include <privateer/block_hash.hpp>
#include <privateer/error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace privateer {

	inline constexpr uint64_t default_block_size = 8ull * 1024 * 1024;

	struct region_options {
		// Datastore constants, written into the recipe header at create and
		// adopted from the header at open. Set on open they must match the
		// header; a mismatch fails with option_mismatch.
		std::optional<uint64_t> block_size;       // create default: default_block_size
		std::optional<hash_algorithm> algorithm;  // create default: xxh3_128

		// bytes of volatile segment header mapped before slot 0, rounded up
		// to a page multiple
		size_t header_size = 0;

		// map-count entries kept free for the rest of the process when slot
		// counts are checked against vm.max_map_count (Linux; Darwin has no
		// such limit and no budget)
		size_t vma_headroom = 4096;

		// mlock the slot state array; the override for swapless deployments,
		// where anonymous pages are never reclaimed
		bool lock_state_array = true;
	};

	struct region {
		// Creates a datastore: the block store skeleton and an empty durable
		// recipe, then opens it. capacity is rounded up to whole slots and
		// fixed for the datastore's lifetime; the extended size starts at
		// zero.
		static result<region> create(std::filesystem::path const &segment_dir, uint64_t capacity,
									 region_options const &options = {});

		// Opens an existing datastore: validates the recipe and every
		// referenced block file, maps the slots, seeds the durable-name set
		// and the reference counts from the recipe, and sweeps unreferenced
		// files. The caller has verified the consistency mark.
		static result<region> open(std::filesystem::path const &segment_dir,
								   region_options const &options = {});

		// Read-only open: maps and validates like open, but never mutates
		// the datastore (no sweep) and refuses extend. A stray write crashes
		// honestly; shared files are never written through.
		static result<region> open_read_only(std::filesystem::path const &segment_dir,
											 region_options const &options = {});

		region(region &&) noexcept;
		region &operator=(region &&) noexcept;
		region(region const &) = delete;
		region &operator=(region const &) = delete;
		~region();

		// slot 0; the segment metall's offset pointers hang off
		[[nodiscard]] void *segment() const noexcept;

		// start of the reservation, header_size bytes of volatile memory
		[[nodiscard]] void *segment_header() const noexcept;

		// the extended size in bytes, a block_size multiple
		[[nodiscard]] uint64_t size() const noexcept;

		[[nodiscard]] uint64_t capacity() const noexcept;
		[[nodiscard]] uint64_t block_size() const noexcept;
		[[nodiscard]] hash_algorithm algorithm() const noexcept;
		[[nodiscard]] bool read_only() const noexcept;

		// Extends the region to at least target_size bytes, rounded up to
		// whole slots: maps the grown range as anonymous zeros and publishes
		// the new size. Fails cleanly beyond capacity or beyond the VMA
		// budget; a target at or below the current size is a no-op. The new
		// size is persisted by the next commit.
		result<> extend(uint64_t target_size);

	private:
		region();

		static result<region> open_impl(std::filesystem::path const &segment_dir,
										region_options const &options, bool read_only);

		struct state;
		std::unique_ptr<state> state_;
	};

}  // namespace privateer

#endif  // PRIVATEER_REGION_HPP
