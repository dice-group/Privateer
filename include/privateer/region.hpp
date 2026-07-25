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
// PROT_NONE.
//
// A read-write open registers the region with the process-wide fault
// handler: the first write into a slot faults, the handler claims the slot,
// makes it writable, counts it dirty, and the retried store lands. Later
// writes to the slot are native. Read-only opens never register; a stray
// write crashes honestly.

#include <privateer/block_hash.hpp>
#include <privateer/error.hpp>
#include <privateer/slot_table.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace privateer {

	inline constexpr uint64_t default_block_size = 8ull * 1024 * 1024;

	// Background write-back of dirty slots ahead of commits. off disables
	// it. non_durable writes and remaps blocks; their durability stays with
	// the next durable commit. eager_durable additionally syncs each batch's
	// block files and shard directory entries, so a later durable commit
	// pays only for dirt the cleaner has not reached yet.
	enum struct cleaner_mode : uint8_t {
		off,
		non_durable,
		eager_durable,
	};

	struct cleaner_options {
		cleaner_mode mode = cleaner_mode::off;

		// cadence of the background sweep
		std::chrono::nanoseconds interval = std::chrono::seconds{1};

		// most slots written back per batch; bounds one commit-mutex hold
		size_t batch_slots = 8;

		// Re-dirty backoff. A slot that turns dirty again within backoff_cap
		// of its last write-back becomes eligible only after backoff_base,
		// doubling per repeat up to backoff_cap, so hot slots stop being
		// written back. A quiet period longer than backoff_cap clears the
		// backoff.
		std::chrono::nanoseconds backoff_base = std::chrono::milliseconds{500};
		std::chrono::nanoseconds backoff_cap = std::chrono::seconds{30};
	};

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

		// Worker count of the commit write-out fan-out on the process-wide
		// executor. 0 selects the hardware concurrency; 1 keeps the
		// write-out on the committing thread.
		size_t commit_workers = 0;

		// Background write-back. Victims are picked cold first, by the time
		// each slot was first seen dirty. Read-only opens have no dirty
		// slots and ignore this.
		cleaner_options cleaner;
	};

	struct region;

#ifdef PRIVATEER_TEST_HOOKS
	// Test-only hooks, compiled in when the build includes the tests.
	namespace detail_region {

		// access to the slot table behind a region
		[[nodiscard]] slot_table &table_of(region &reg) noexcept;

		// Runs the fault path exactly as the process-wide handler would for
		// a fault at addr. Returns whether the fault was handled.
		bool deliver_fault(region &reg, uintptr_t addr, int signo) noexcept;

		// The protection-change syscall of the fault path. Tests replace it
		// to fail the change on purpose; everything else leaves it alone.
		extern int (*mprotect_fn)(void *addr, size_t len, int prot);

		// When set, called after each completed commit phase (1 capture,
		// 2 write-out, 3 durability barrier, 4 recipe rename, 5 reclaim).
		// Crash tests kill the process inside it.
		extern void (*commit_phase_hook)(int completed_phase);

		// The hard-link syscall of the snapshot staging. Tests replace it to
		// force the per-file copy fallback or to kill mid-staging.
		extern int (*link_fn)(char const *from, char const *to);

		// Posts fn as a region-owned executor task: the closing no-op
		// wrapper, the catch-all, and the outstanding-task counter apply.
		void post_task(region &reg, std::function<void()> fn);

		// Arms a one-shot region-owned timer on the timer pool. The handler
		// runs on the timer thread; aborted is true when close cancelled the
		// wait, or when closing began before the handler ran.
		void start_timer(region &reg, std::chrono::nanoseconds delay,
						 std::function<void(bool aborted)> handler);

		// The cleaner's time source, monotonic nanoseconds. Tests replace it
		// for deterministic backoff decisions.
		extern int64_t (*clock_fn)();

		// When set, called after each slot the cleaner writes back. Crash
		// tests kill the process inside it.
		extern void (*cleaner_slot_hook)(size_t slot);

		// Runs one cleaner batch synchronously on the calling thread,
		// regardless of the region's cleaner mode and interval.
		// override_backoff ignores the re-dirty backoff. Returns the number
		// of slots written back.
		size_t run_cleaner_batch(region &reg, bool override_backoff);

	}  // namespace detail_region
#endif  // PRIVATEER_TEST_HOOKS

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

		// true while no failure was recorded on the region; once false,
		// commits and close fail, and metall withholds the consistency mark
		[[nodiscard]] bool check_sanity() const noexcept;

		// Extends the region to at least target_size bytes, rounded up to
		// whole slots: maps the grown range as anonymous zeros and publishes
		// the new size. Fails cleanly beyond capacity or beyond the VMA
		// budget; a target at or below the current size is a no-op. The new
		// size is persisted by the next commit.
		result<> extend(uint64_t target_size);

		// Frees the whole slots fully covered by [offset, offset + nbytes):
		// each is remapped to fresh anonymous zeros now, its old block file
		// becomes reclaimable at the next durable commit, and reads observe
		// zeros. Partially covered slots stay untouched (a documented
		// divergence: sub-slot frees reclaim nothing; metall's allocator
		// never reads freed memory expecting content). Best-effort and safe
		// against concurrent writers: a write racing the free either lands
		// in the discarded pages or materializes the fresh zeros. Callable
		// concurrently with commits; fails on read-only and closing regions.
		result<> free_region(uint64_t offset, uint64_t nbytes);

		// Commits the region's content: captures every dirty slot, freezes
		// it, writes changed blocks to the store, and atomically replaces
		// the recipe. durable adds the durability barrier before the rename
		// and reclaims retired block files after it; a durable commit
		// returns only after everything the new recipe references is on
		// stable storage. One commit runs at a time; readers stay live
		// throughout, and a writer that faults a captured slot waits only
		// for that slot's own write-out. A consistent cut requires that the
		// application does not write concurrently. On a read-only region
		// this is a no-op success.
		result<> commit(bool durable);

		// Stages a self-contained copy of the region's committed state into
		// staging_segment_dir: a durable commit runs first, then every
		// referenced block file is hard-linked (per-file copy fallback where
		// link fails) and the recipe copy is written and synced. Both steps
		// run under the commit mutex, so no commit in between can reclaim a
		// block the staged recipe references. The caller (metall) fsyncs the
		// staged tree and publishes the datastore with one atomic rename.
		result<> snapshot_to(std::filesystem::path const &staging_segment_dir);

		// Stages a self-contained copy of an on-disk datastore that no
		// writer holds open (metall keeps a shared lock on the source). The
		// source is never mutated; link only bumps inode link counts.
		static result<> copy(std::filesystem::path const &src_segment_dir,
							 std::filesystem::path const &dst_segment_dir);

	private:
		region();

		static result<region> open_impl(std::filesystem::path const &segment_dir,
										region_options const &options, bool read_only);

		// the commit phases; the caller holds the commit mutex
		result<> commit_impl(bool durable);

		struct state;
		std::unique_ptr<state> state_;

#ifdef PRIVATEER_TEST_HOOKS
		friend slot_table &detail_region::table_of(region &reg) noexcept;
		friend bool detail_region::deliver_fault(region &reg, uintptr_t addr, int signo) noexcept;
		friend void detail_region::post_task(region &reg, std::function<void()> fn);
		friend void detail_region::start_timer(region &reg, std::chrono::nanoseconds delay,
											   std::function<void(bool aborted)> handler);
		friend size_t detail_region::run_cleaner_batch(region &reg, bool override_backoff);
#endif
	};

}  // namespace privateer

#endif  // PRIVATEER_REGION_HPP
