#ifndef PRIVATEER_REGION_REGISTRY_HPP
#define PRIVATEER_REGION_REGISTRY_HPP

// The region registry the fault handler searches: a sorted array of address
// ranges published through one atomic pointer. add and remove build a new
// table and swap it in, serialized by a mutex; retired tables go on a
// never-freed list, so the handler can dereference any table it loaded
// without a reclamation protocol. The leak is bounded: tables change only at
// datastore open and close. All tables are mlocked; the handler must never
// take a page fault of its own.

#include <privateer/error.hpp>
#include <privateer/mlocked.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace privateer {

	// Per-region state shared between the region owner and the fault handler.
	// The owner keeps it alive and mlocked until remove() has returned.
	struct region_record {
		// Called by the fault handler for a fault inside [start, end), with
		// the record's in-flight counter held. Returns true when the fault is
		// handled (the faulting instruction is retried), false to forward to
		// the previous disposition. Must be async-signal-safe.
		bool (*on_fault)(region_record &rec, uintptr_t addr, int signo) = nullptr;
		void *context = nullptr;

		// how many lookups on this region are between acquire and release
		std::atomic<uint32_t> handler_in_flight{0};
		std::atomic<uint32_t> free_in_flight{0};
	};

	static_assert(std::atomic<uint32_t>::is_always_lock_free);

	struct region_registry {
		// whose in-flight counter a lookup holds: the fault handler's or free_region's
		enum struct in_flight_kind : int {
			handler,
			free_region,
		};

		constexpr region_registry() = default;
		region_registry(region_registry const &) = delete;
		region_registry &operator=(region_registry const &) = delete;
		~region_registry() = default;

		// Registers [start, end) for rec. Rejects empty and overlapping
		// ranges. Normal thread context only.
		result<> add(region_record &rec, uintptr_t start, uintptr_t end);

		// Removes rec's range and waits until every lookup that could still
		// reach the record has left: publishes the new table, flips the gate
		// epoch, drains the old epoch's gate counter, then drains both of the
		// record's in-flight counters. Serialized by the registry mutex, so
		// concurrent removes flip the epoch one at a time. Normal thread
		// context only.
		void remove(region_record &rec) noexcept;

		// Async-signal-safe lookup. On a hit the record's kind counter is
		// already incremented; the caller must call release() when done with
		// the record. Returns nullptr on a miss.
		region_record *acquire(uintptr_t addr, in_flight_kind kind) noexcept;
		static void release(region_record &rec, in_flight_kind kind) noexcept;

	private:
		struct entry {
			uintptr_t start;
			uintptr_t end;
			region_record *record;
		};

		// sorted entry array, placed in one mlocked buffer
		struct table {
			size_t count;

			entry *items() noexcept { return reinterpret_cast<entry *>(this + 1); }
			entry const *items() const noexcept { return reinterpret_cast<entry const *>(this + 1); }
		};

		// allocates a table for count entries and keeps its buffer in tables_
		result<table *> make_table(size_t count);

		std::atomic<table *> current_{nullptr};
		std::atomic<uint32_t> epoch_{0};
		std::atomic<uint32_t> gate_[2]{};

		std::mutex mutex_;                     // serializes add and remove
		std::vector<mlocked_buffer> tables_;   // every table ever published; never freed
	};

	// the process-global registry the fault handler searches
	region_registry &global_registry() noexcept;

}  // namespace privateer

#endif  // PRIVATEER_REGION_REGISTRY_HPP
