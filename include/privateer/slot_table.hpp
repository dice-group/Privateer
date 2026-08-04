// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_SLOT_TABLE_HPP
#define PRIVATEER_SLOT_TABLE_HPP

// The per-region slot state table. Every actor that changes a slot's mapping
// or protection goes through this table: the fault handler, the committer,
// the cleaner, and free_region. Two rules make the state trustworthy:
//
// - Claim before touch: whoever changes a slot's protection or mapping first
//   CASes the state to a transient claim state, then issues the syscall.
//   There is never a window where the page is read-only while the state
//   still claims writable.
// - Publish after protect: a terminal state is stored only after the
//   mprotect or mmap that establishes its protection has returned, so an
//   observed terminal state always describes the mapping.
//
// The state array lives in one buffer whose pages are mlocked as far as the
// region's extended size reaches: the fault handler reads and waits on these
// words while its own signal is masked, and a page fault on a reclaimed
// state page there would kill the process. The handler gates on the extended
// size before it touches any state word, so pages beyond the locked prefix
// are never read in signal context.

#include <privateer/error.hpp>
#include <privateer/mlocked.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace privateer {

	// One 32-bit word per slot: waiters futex-wait on the state word, and
	// futex operates on aligned 32-bit words, so a word-wide state gives
	// every slot its own uncontended wait word.
	enum struct slot_state : uint32_t {
		// Terminal states. Each implies its mapping and protection are in place.
		empty = 0,    // fresh anonymous zero mapping, read-only; the recipe holds the empty sentinel
		clean,        // block-file mapping matching the recipe entry, read-only
		dirty,        // private copy-on-write pages, writable
		dirty_empty,  // freed since the last commit: anonymous zeros, the recipe entry is stale
		poisoned,     // a protection change failed; recoverable, waits on it are always timed
		// Transient claim states, each owned by exactly one actor until it
		// publishes a terminal state.
		materializing,  // a faulting writer installs write access
		syncing,        // a commit-mutex holder (committer or cleaner) writes the slot out
		freeing,        // a free_region caller replaces the mapping
	};

	static_assert(std::atomic<uint32_t>::is_always_lock_free);

	[[nodiscard]] constexpr bool is_transient(slot_state state) noexcept {
		return state == slot_state::materializing || state == slot_state::syncing ||
			   state == slot_state::freeing;
	}

	// short stable name of the state, for logs and test failures
	[[nodiscard]] char const *to_string(slot_state state) noexcept;

	// The state array plus the dirty accounting it feeds. The table performs
	// no syscalls besides waits and wakes; mapping and protection changes are
	// the callers' job, ordered by the two rules above.
	struct slot_table {
		// Allocates the state array at the full slot_count; every slot starts
		// empty. With lock true the pages covering the header are locked
		// right away and lock_to locks more as the region grows. With lock
		// false nothing is ever locked (the override for swapless
		// deployments).
		static result<slot_table> create(size_t slot_count, bool lock = true);

		constexpr slot_table() = default;
		slot_table(slot_table &&) = default;
		slot_table &operator=(slot_table &&) = default;
		slot_table(slot_table const &) = delete;
		slot_table &operator=(slot_table const &) = delete;
		~slot_table() = default;

		[[nodiscard]] size_t slot_count() const noexcept { return count_; }

		// Grows the locked prefix to the pages covering the header and the
		// first slots_in_use state words; it never shrinks. A failed mlock
		// changes nothing and reports memlock_limit_too_low. With locking
		// disabled at create this is a no-op.
		result<> lock_to(size_t slots_in_use) noexcept;

		// bytes lock_to(slots_in_use) keeps locked, page granular
		[[nodiscard]] static size_t locked_bytes_for(size_t slots_in_use) noexcept;

		// bytes currently locked; 0 with locking disabled
		[[nodiscard]] size_t locked_bytes() const noexcept { return locked_end_; }

		// whether create enabled locking
		[[nodiscard]] bool locking() const noexcept { return lock_; }

		[[nodiscard]] slot_state load(size_t slot) const noexcept;  // acquire

		// Claims the slot for one actor: CAS expected -> claim (acq_rel).
		// The winner owns the slot's mapping and protection until it
		// publishes a terminal state. Returns false when the slot does not
		// hold expected; the caller reloads and re-decides.
		[[nodiscard]] bool try_claim(size_t slot, slot_state expected, slot_state claim) noexcept;

		// Publishes a terminal state (release) and wakes every waiter parked
		// on the slot. Call only after the protecting syscall has returned.
		void publish(size_t slot, slot_state terminal) noexcept;

		// Waits while the slot holds observed; returns the changed state.
		[[nodiscard]] slot_state wait_changed(size_t slot, slot_state observed) noexcept;

		// Timed variant: waits at most timeout_ns. Returns the current state;
		// observed back means the deadline passed with the word unchanged.
		// Waits on a poisoned slot use this, so a slot whose recovery never
		// succeeds cannot park a waiter forever.
		[[nodiscard]] slot_state wait_changed_for(size_t slot, slot_state observed,
												  int64_t timeout_ns) noexcept;

		// Dirty accounting, slot-granular: a slot counts from the moment a
		// faulting writer wins its materializing claim until it leaves dirty
		// or materializing for a terminal state.
		[[nodiscard]] uint64_t dirty_slots() const noexcept;
		// Increments and returns the new count, so the caller can detect
		// the exact crossing of a watermark.
		uint64_t add_dirty() noexcept;
		// Decrements, then bumps the governor word and wakes it: writers
		// blocked on the dirty budget recheck on every decrease.
		void sub_dirty() noexcept;

		// The word the governor's waiters park on. Waiting and waking go
		// through word_wait and word_wake_all with a value read from here.
		[[nodiscard]] std::atomic<uint32_t> &governor_word() noexcept;

		// Bumps the governor word and wakes its waiters: the soft-mark
		// crossing in the handler, and close releasing blocked waiters. The
		// bump makes a wake between a waiter's value read and its park
		// visible as a changed value.
		void wake_governor() noexcept;

		// Wakes the waiters parked on the state word of a slot below
		// slots_in_use, for close releasing them. Only the states a waiter
		// waits on are woken. The state stays what it is, so a waiter
		// between its state load and its park does not see this wake; the
		// waits on those states are timed, which is what covers it.
		void wake_slot_waiters(size_t slots_in_use) noexcept;

		// The extended region size in bytes, a slot multiple. The fault
		// handler gates on it before it reads any slot state, so it lives in
		// the same mlocked buffer as the states. The store happens only after
		// the grown range's mappings and states are in place (release), and
		// every reader pairs with it (acquire), so a slot is only ever
		// examined after its mapping and state are visible.
		[[nodiscard]] uint64_t extended_size() const noexcept;
		void set_extended_size(uint64_t bytes) noexcept;

	private:
		// buffer layout: the counters, then the state array
		struct header {
			std::atomic<uint64_t> dirty;
			std::atomic<uint64_t> extended;
			std::atomic<uint32_t> governor;
		};

		[[nodiscard]] header *head() const noexcept;
		[[nodiscard]] std::atomic<uint32_t> *states() const noexcept;

		mlocked_buffer buffer_;
		size_t count_ = 0;
		bool lock_ = false;
		size_t locked_end_ = 0;  // page multiple, from the buffer start
	};

}  // namespace privateer

#endif  // PRIVATEER_SLOT_TABLE_HPP
