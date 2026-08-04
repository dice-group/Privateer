// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/slot_table.hpp>

#include <privateer/handler_text.hpp>
#include <privateer/vm.hpp>
#include <privateer/word_wait.hpp>

#include <algorithm>
#include <cassert>
#include <new>

#include <sys/mman.h>

namespace privateer {

	char const *to_string(slot_state state) noexcept {
		switch (state) {
			case slot_state::empty: return "empty";
			case slot_state::clean: return "clean";
			case slot_state::dirty: return "dirty";
			case slot_state::dirty_empty: return "dirty_empty";
			case slot_state::poisoned: return "poisoned";
			case slot_state::materializing: return "materializing";
			case slot_state::syncing: return "syncing";
			case slot_state::freeing: return "freeing";
		}
		return "unknown";
	}

	result<slot_table> slot_table::create(size_t slot_count, bool lock) {
		if (slot_count == 0) {
			return fail(errc::invalid_argument, "slot table needs at least one slot");
		}
		// allocated unlocked; the table locks its own prefix page-wise
		auto buffer = mlocked_buffer::allocate(sizeof(header) + slot_count * sizeof(std::atomic<uint32_t>), false);
		if (!buffer) {
			return std::unexpected{buffer.error()};
		}
		slot_table table;
		table.buffer_ = std::move(*buffer);
		table.count_ = slot_count;
		table.lock_ = lock;
		new (table.buffer_.addr()) header{};
		auto *states = table.states();
		for (size_t i = 0; i < slot_count; ++i) {
			new (states + i) std::atomic<uint32_t>{static_cast<uint32_t>(slot_state::empty)};
		}
		// the header page: the counters and the governor word are handler-read
		if (auto locked = table.lock_to(0); !locked) {
			return std::unexpected{locked.error()};
		}
		return table;
	}

	size_t slot_table::locked_bytes_for(size_t slots_in_use) noexcept {
		size_t const page = page_size();
		size_t const bytes = sizeof(header) + slots_in_use * sizeof(std::atomic<uint32_t>);
		return (bytes + page - 1) / page * page;
	}

	result<> slot_table::lock_to(size_t slots_in_use) noexcept {
		if (!lock_) {
			return {};
		}
		size_t const end = locked_bytes_for(std::min(slots_in_use, count_));
		if (end <= locked_end_) {
			return {};
		}
		if (::mlock(static_cast<std::byte *>(buffer_.addr()) + locked_end_, end - locked_end_) != 0) {
			return fail_errno(errc::memlock_limit_too_low, "mlock state pages");
		}
		locked_end_ = end;
		return {};
	}

	slot_table::header *slot_table::head() const noexcept {
		return static_cast<header *>(buffer_.addr());
	}

	std::atomic<uint32_t> *slot_table::states() const noexcept {
		return reinterpret_cast<std::atomic<uint32_t> *>(static_cast<std::byte *>(buffer_.addr()) + sizeof(header));
	}

	PRIVATEER_HANDLER_TEXT slot_state slot_table::load(size_t slot) const noexcept {
		assert(slot < count_);
		return static_cast<slot_state>(states()[slot].load(std::memory_order_acquire));
	}

	PRIVATEER_HANDLER_TEXT bool slot_table::try_claim(size_t slot, slot_state expected, slot_state claim) noexcept {
		assert(slot < count_);
		assert(!is_transient(expected));
		assert(is_transient(claim));
		auto expected_word = static_cast<uint32_t>(expected);
		return states()[slot].compare_exchange_strong(expected_word, static_cast<uint32_t>(claim),
													  std::memory_order_acq_rel, std::memory_order_acquire);
	}

	PRIVATEER_HANDLER_TEXT void slot_table::publish(size_t slot, slot_state terminal) noexcept {
		assert(slot < count_);
		assert(!is_transient(terminal));
		states()[slot].store(static_cast<uint32_t>(terminal), std::memory_order_release);
		word_wake_all(states()[slot]);
	}

	PRIVATEER_HANDLER_TEXT slot_state slot_table::wait_changed(size_t slot, slot_state observed) noexcept {
		assert(slot < count_);
		return static_cast<slot_state>(word_wait(states()[slot], static_cast<uint32_t>(observed)));
	}

	PRIVATEER_HANDLER_TEXT slot_state slot_table::wait_changed_for(size_t slot, slot_state observed,
																   int64_t timeout_ns) noexcept {
		assert(slot < count_);
		return static_cast<slot_state>(
				word_wait_for(states()[slot], static_cast<uint32_t>(observed), timeout_ns));
	}

	PRIVATEER_HANDLER_TEXT uint64_t slot_table::dirty_slots() const noexcept {
		return head()->dirty.load(std::memory_order_acquire);
	}

	PRIVATEER_HANDLER_TEXT uint64_t slot_table::add_dirty() noexcept {
		return head()->dirty.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	PRIVATEER_HANDLER_TEXT void slot_table::sub_dirty() noexcept {
		[[maybe_unused]] uint64_t const previous = head()->dirty.fetch_sub(1, std::memory_order_acq_rel);
		assert(previous > 0);
		wake_governor();
	}

	PRIVATEER_HANDLER_TEXT void slot_table::wake_governor() noexcept {
		head()->governor.fetch_add(1, std::memory_order_release);
		word_wake_all(head()->governor);
	}

	void slot_table::wake_slot_waiters(size_t slots_in_use) noexcept {
		size_t const end = std::min(slots_in_use, count_);
		for (size_t slot = 0; slot < end; ++slot) {
			slot_state const state = load(slot);
			if (is_transient(state) || state == slot_state::poisoned) {
				word_wake_all(states()[slot]);
			}
		}
	}

	PRIVATEER_HANDLER_TEXT std::atomic<uint32_t> &slot_table::governor_word() noexcept {
		return head()->governor;
	}

	PRIVATEER_HANDLER_TEXT uint64_t slot_table::extended_size() const noexcept {
		return head()->extended.load(std::memory_order_acquire);
	}

	void slot_table::set_extended_size(uint64_t bytes) noexcept {
		head()->extended.store(bytes, std::memory_order_release);
	}

}  // namespace privateer
