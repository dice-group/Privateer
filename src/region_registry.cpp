// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/region_registry.hpp>

#include <privateer/handler_text.hpp>

#include <cerrno>

#include <time.h>

namespace privateer {

	namespace {

		// short backoff for the drain loops in remove(); normal thread context
		void drain_backoff() noexcept {
			timespec const pause{0, 100'000};
			::nanosleep(&pause, nullptr);
		}

	}  // namespace

	result<> region_registry::reserve_tables(size_t slots) noexcept {
		if (tables_.capacity() >= slots) {
			return {};
		}
		try {
			tables_.reserve(slots);
		} catch (...) {
			return std::unexpected{error{errc::io_error, ENOMEM, "region registry table list"}};
		}
		return {};
	}

	result<region_registry::table *> region_registry::make_table(size_t count, bool lock) noexcept {
		if (auto room = reserve_tables(tables_.size() + 1); !room) {
			return std::unexpected{room.error()};
		}
		auto buf = mlocked_buffer::allocate(sizeof(table) + count * sizeof(entry), lock);
		if (!buf) {
			return std::unexpected{buf.error()};
		}
		auto *t = static_cast<table *>(buf->addr());
		t->count = count;
		// the slot is there, so keeping the buffer takes no memory and cannot throw
		tables_.push_back(std::move(*buf));
		return t;
	}

	result<> region_registry::add(region_record &rec, uintptr_t start, uintptr_t end) {
		if (start >= end) {
			return fail(errc::invalid_argument, "region range is empty");
		}
		std::lock_guard const lock{mutex_};
		table const *const old = current_.load(std::memory_order_relaxed);
		size_t const old_count = old != nullptr ? old->count : 0;

		for (size_t i = 0; i < old_count; ++i) {
			auto const &it = old->items()[i];
			if (start < it.end && it.start < end) {
				return fail(errc::invalid_argument, "region range overlaps a registered region");
			}
		}

		// remove() must not fail and must not throw, so the successor table
		// it builds finds its slot in the table list already there: every
		// registered region keeps one spare slot, reserved here.
		if (auto room = reserve_tables(tables_.size() + old_count + 2); !room) {
			return std::unexpected{room.error()};
		}
		auto t = make_table(old_count + 1);
		if (!t) {
			return std::unexpected{t.error()};
		}
		size_t out = 0;
		for (size_t i = 0; i < old_count && old->items()[i].start < start; ++i) {
			(*t)->items()[out++] = old->items()[i];
		}
		(*t)->items()[out++] = entry{start, end, &rec};
		for (size_t i = out - 1; i < old_count; ++i) {
			(*t)->items()[out++] = old->items()[i];
		}
		current_.store(*t, std::memory_order_release);
		return {};
	}

	void region_registry::remove(region_record &rec) noexcept {
		std::lock_guard const lock{mutex_};
		table const *const old = current_.load(std::memory_order_relaxed);
		size_t const old_count = old != nullptr ? old->count : 0;

		size_t index = old_count;
		for (size_t i = 0; i < old_count; ++i) {
			if (old->items()[i].record == &rec) {
				index = i;
				break;
			}
		}
		if (index == old_count) {
			return;
		}

		// The successor table needs one small mapping; its slot in the table
		// list is the spare the matching add reserved, so nothing here takes
		// heap memory. remove must not fail (close depends on it), so a
		// memory allocation failure is retried; a process unable to allocate
		// a few hundred bytes is lost anyway. A failed mlock is not
		// transient: RLIMIT_MEMLOCK is exhausted and stays so. The table is
		// then left unlocked, trading the handler's reclaimed-page hazard
		// for a close that terminates.
		table *next = nullptr;
		bool lock_table = true;
		for (;;) {
			auto t = make_table(old_count - 1, lock_table);
			if (t) {
				next = *t;
				break;
			}
			if (t.error().code == errc::memlock_limit_too_low) {
				lock_table = false;
				continue;
			}
			drain_backoff();
		}
		size_t out = 0;
		for (size_t i = 0; i < old_count; ++i) {
			if (i != index) {
				next->items()[out++] = old->items()[i];
			}
		}
		current_.store(next, std::memory_order_release);

		// Flip the gate epoch and drain only the old epoch's counter: that
		// covers every lookup that could have loaded the pre-swap table,
		// without waiting for the instantaneous global zero that a fault
		// storm on another region could postpone forever.
		uint32_t const old_epoch = epoch_.fetch_add(1, std::memory_order_seq_cst);
		while (gate_[old_epoch & 1].load(std::memory_order_acquire) != 0) {
			drain_backoff();
		}

		// every lookup that found the record has incremented its counter
		// before leaving the gate; wait until they have all released it
		while (rec.handler_in_flight.load(std::memory_order_acquire) != 0 ||
			   rec.free_in_flight.load(std::memory_order_acquire) != 0) {
			drain_backoff();
		}
	}

	PRIVATEER_HANDLER_TEXT region_record *region_registry::acquire(uintptr_t addr, in_flight_kind kind) noexcept {
		// Enter the gate: increment the current epoch's counter, then confirm
		// the epoch is still current. A lookup that raced an epoch flip undoes
		// its increment and retries, so a held gate slot always belongs to the
		// epoch that was current at validation time; remove() relies on that
		// when it drains only the old epoch's counter.
		uint32_t e;
		for (;;) {
			e = epoch_.load(std::memory_order_seq_cst);
			gate_[e & 1].fetch_add(1, std::memory_order_seq_cst);
			if (epoch_.load(std::memory_order_seq_cst) == e) {
				break;
			}
			gate_[e & 1].fetch_sub(1, std::memory_order_release);
		}

		region_record *found = nullptr;
		if (table const *const t = current_.load(std::memory_order_acquire); t != nullptr) {
			size_t lo = 0;
			size_t hi = t->count;
			while (lo < hi) {
				size_t const mid = lo + (hi - lo) / 2;
				auto const &it = t->items()[mid];
				if (addr < it.start) {
					hi = mid;
				} else if (addr >= it.end) {
					lo = mid + 1;
				} else {
					found = it.record;
					break;
				}
			}
		}
		if (found != nullptr) {
			// taken before the gate is left, so remove() sees it after the drain
			auto &counter = kind == in_flight_kind::handler ? found->handler_in_flight : found->free_in_flight;
			counter.fetch_add(1, std::memory_order_acq_rel);
		}
		gate_[e & 1].fetch_sub(1, std::memory_order_release);
		return found;
	}

	PRIVATEER_HANDLER_TEXT void region_registry::release(region_record &rec, in_flight_kind kind) noexcept {
		auto &counter = kind == in_flight_kind::handler ? rec.handler_in_flight : rec.free_in_flight;
		counter.fetch_sub(1, std::memory_order_release);
	}

	void region_registry::visit(void (*fn)(region_record &)) noexcept {
		if (table const *const t = current_.load(std::memory_order_acquire); t != nullptr) {
			for (size_t i = 0; i < t->count; ++i) {
				fn(*t->items()[i].record);
			}
		}
	}

	namespace {
		constinit region_registry g_registry;
	}

	PRIVATEER_HANDLER_TEXT region_registry &global_registry() noexcept {
		return g_registry;
	}

}  // namespace privateer
