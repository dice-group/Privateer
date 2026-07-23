#include <privateer/region.hpp>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/handler_text.hpp>
#include <privateer/logger.hpp>
#include <privateer/recipe.hpp>
#include <privateer/region_registry.hpp>
#include <privateer/rlimits.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>
#include <privateer/word_wait.hpp>

#include <cstdio>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sys/mman.h>

namespace privateer {

	namespace fs = std::filesystem;

	namespace {

		constexpr uint64_t round_up(uint64_t value, uint64_t multiple) noexcept {
			return (value + multiple - 1) / multiple * multiple;
		}

		// The VMA budget: vm.max_map_count minus the configured headroom for
		// the rest of the process. Darwin has no map-count limit.
		size_t vma_budget(size_t headroom) noexcept {
#ifdef __linux__
			size_t limit = 65530;  // the kernel default, used when the sysctl is unreadable
			if (std::FILE *file = std::fopen("/proc/sys/vm/max_map_count", "r"); file != nullptr) {
				unsigned long long value = 0;
				if (std::fscanf(file, "%llu", &value) == 1) {
					limit = static_cast<size_t>(value);
				}
				std::fclose(file);
			}
			return limit > headroom ? limit - headroom : 0;
#else
			(void) headroom;
			return SIZE_MAX;
#endif
		}

		// The region state the fault handler dereferences, placed in one
		// mlocked buffer: the fault signal is masked while the handler runs,
		// so a page fault on this data would kill the process.
		struct region_hot {
			region_record record{};
			slot_table table;
			std::atomic<uint32_t> error{0};
			std::atomic<uint32_t> closing{0};
			uintptr_t segment_start = 0;
			uint64_t block_size = 0;
		};

	}  // namespace

	namespace detail_region {

		int (*mprotect_fn)(void *, size_t, int) = ::mprotect;
		void (*commit_phase_hook)(int) = nullptr;

	}  // namespace detail_region

	namespace {

		void commit_phase_done(int phase) {
			if (detail_region::commit_phase_hook != nullptr) {
				detail_region::commit_phase_hook(phase);
			}
		}

	}  // namespace

	namespace {

		// The fault path. Runs in the process-wide handler with the record's
		// in-flight counter held; everything it touches is mlocked, and every
		// call it makes is async-signal-safe.
		PRIVATEER_HANDLER_TEXT bool region_on_fault(region_record &rec, uintptr_t addr, int) {
			auto &hot = *static_cast<region_hot *>(rec.context);
			if (hot.closing.load(std::memory_order_acquire) != 0) {
				return false;  // the application must have quiesced; fail loudly
			}
			uint64_t const offset = addr - hot.segment_start;
			if (offset >= hot.table.extended_size()) {
				return false;  // beyond the extended size: a genuine wild access
			}
			size_t const slot = offset / hot.block_size;
			for (;;) {
				slot_state const state = hot.table.load(slot);
				switch (state) {
					case slot_state::empty:
					case slot_state::clean:
					case slot_state::dirty_empty: {
						if (!hot.table.try_claim(slot, state, slot_state::materializing)) {
							continue;  // the slot moved; re-examine
						}
						hot.table.add_dirty();
						void *const slot_addr =
								reinterpret_cast<void *>(hot.segment_start + slot * hot.block_size);
						if (detail_region::mprotect_fn(slot_addr, hot.block_size, PROT_READ | PROT_WRITE) != 0) {
							// The slot is dead. Balance the count, publish the
							// terminal poisoned state so no waiter parks
							// forever, record the failure, and forward.
							hot.table.sub_dirty();
							hot.table.publish(slot, slot_state::poisoned);
							hot.error.store(1, std::memory_order_release);
							return false;
						}
						hot.table.publish(slot, slot_state::dirty);
						return true;  // the retried store lands
					}
					case slot_state::materializing:
					case slot_state::syncing:
					case slot_state::freeing:
						// Wait out the transient, then retry the instruction;
						// the retry classifies itself: a read succeeds against
						// the restored mapping, a write re-faults into the
						// claim path.
						(void) hot.table.wait_changed(slot, state);
						return true;
					case slot_state::dirty:
						// Writable by publish-after-protect, so this is a
						// stale TLB entry or a benign race with a fresh
						// transition; the retry succeeds.
						return true;
					case slot_state::poisoned:
						hot.error.store(1, std::memory_order_release);
						return false;
				}
				// a corrupt state value: no claim was won, nothing to balance
				hot.error.store(1, std::memory_order_release);
				return false;
			}
		}

		// The engine is fork-unsafe: memory locks are not inherited, no
		// executor thread survives into the child, and held mutexes stay
		// locked. The child handler marks every open region so misuse fails
		// loudly instead of hanging or corrupting. Lock-free, because the
		// parent may fork while another thread holds the registry mutex.
		void poison_regions_in_fork_child() noexcept {
			global_registry().visit([](region_record &rec) {
				if (rec.fork_poison != nullptr) {
					rec.fork_poison->store(1, std::memory_order_release);
				}
			});
		}

		std::once_flag g_atfork_once;

		void install_fork_poison() {
			std::call_once(g_atfork_once,
						   [] { ::pthread_atfork(nullptr, nullptr, poison_regions_in_fork_child); });
		}

	}  // namespace

	struct region::state {
		fs::path segment_dir;
		std::optional<block_store> store;
		recipe rec;  // the in-memory recipe table; entries change only under the commit mutex
		mlocked_buffer hot_buffer;
		region_hot *hot = nullptr;
		bool registered = false;
		vm_reservation reservation;
		size_t header_bytes = 0;
		size_t vma_headroom = 0;
		bool read_only = false;
		std::mutex region_mutex;  // serializes extend and close bookkeeping
		std::mutex commit_mutex;  // one commit at a time; owns the recipe table and the store bookkeeping

		~state() {
			if (hot != nullptr) {
				if (registered) {
					// New faults forward as crashes from here on; the
					// application has quiesced its readers and writers.
					hot->closing.store(1, std::memory_order_seq_cst);
					word_wake_all(hot->table.governor_word());
					global_registry().remove(hot->record);
				}
				hot->~region_hot();
				hot = nullptr;
			}
		}
	};

	region::region() : state_{std::make_unique<state>()} {}
	region::region(region &&) noexcept = default;
	region &region::operator=(region &&) noexcept = default;
	region::~region() = default;

	result<region> region::create(fs::path const &segment_dir, uint64_t capacity,
								  region_options const &options) {
		uint64_t const block_size = options.block_size.value_or(default_block_size);
		if (block_size == 0 || block_size % page_size() != 0) {
			return fail(errc::invalid_argument, "block_size must be a positive page multiple");
		}
		if (capacity == 0) {
			return fail(errc::invalid_argument, "capacity is zero");
		}
		std::error_code ec;
		fs::create_directories(segment_dir, ec);
		if (ec) {
			return std::unexpected{error{errc::io_error, ec.value(), "create the segment directory"}};
		}
		auto store = block_store::create(segment_dir);
		if (!store) {
			return std::unexpected{store.error()};
		}
		recipe rec;
		rec.block_size = block_size;
		rec.capacity = round_up(capacity, block_size);
		rec.size = 0;
		rec.algorithm = options.algorithm.value_or(hash_algorithm::xxh3_128);
		if (auto committed = rec.commit(segment_dir, true); !committed) {
			return std::unexpected{committed.error()};
		}
		return open_impl(segment_dir, options, false);
	}

	result<region> region::open(fs::path const &segment_dir, region_options const &options) {
		return open_impl(segment_dir, options, false);
	}

	result<region> region::open_read_only(fs::path const &segment_dir, region_options const &options) {
		return open_impl(segment_dir, options, true);
	}

	result<region> region::open_impl(fs::path const &segment_dir, region_options const &options,
									 bool read_only) {
		auto rec = recipe::load(segment_dir);
		if (!rec) {
			return std::unexpected{rec.error()};
		}
		auto store = block_store::open(segment_dir);
		if (!store) {
			return std::unexpected{store.error()};
		}

		if (options.block_size && *options.block_size != rec->block_size) {
			return fail(errc::option_mismatch, "requested block_size differs from the recipe header");
		}
		if (options.algorithm && *options.algorithm != rec->algorithm) {
			return fail(errc::option_mismatch, "requested hash algorithm differs from the recipe header");
		}
		if (rec->block_size % page_size() != 0) {
			return fail(errc::option_mismatch, "block_size is not a multiple of this host's page size");
		}
		uint64_t const slot_count = rec->capacity / rec->block_size;
		if (slot_count == 0) {
			return fail(errc::datastore_inconsistent, "capacity is smaller than one slot");
		}

		if (auto validated = validate_blocks(*rec, *store); !validated) {
			return std::unexpected{validated.error()};
		}

		uint64_t const size_slots = rec->size / rec->block_size;
		if (size_slots > vma_budget(options.vma_headroom)) {
			return fail(errc::vma_budget_exceeded, "mapping the extended size would cross the VMA budget");
		}

		bool const lock = !read_only && options.lock_state_array;
		if (lock) {
			if (auto raised = ensure_memlock_limit(slot_count * sizeof(uint32_t) + sizeof(region_hot) + 64);
				!raised) {
				return std::unexpected{raised.error()};
			}
		}
		auto table = slot_table::create(slot_count, lock);
		if (!table) {
			return std::unexpected{table.error()};
		}
		auto hot_buffer = mlocked_buffer::allocate(sizeof(region_hot), lock);
		if (!hot_buffer) {
			return std::unexpected{hot_buffer.error()};
		}

		size_t const header_bytes = round_up(options.header_size, page_size());
		auto reservation = vm_reservation::reserve(header_bytes + round_up(rec->capacity, page_size()));
		if (!reservation) {
			return std::unexpected{reservation.error()};
		}

		// From here on the region owns everything; its destructor cleans up
		// every early-return path.
		region reg;
		auto &st = *reg.state_;
		st.segment_dir = segment_dir;
		st.rec = std::move(*rec);
		st.hot_buffer = std::move(*hot_buffer);
		st.hot = new (st.hot_buffer.addr()) region_hot{};
		st.hot->table = std::move(*table);
		st.hot->block_size = st.rec.block_size;
		st.reservation = std::move(*reservation);
		st.header_bytes = header_bytes;
		st.vma_headroom = options.vma_headroom;
		st.read_only = read_only;
		st.store = std::move(*store);

		auto *const base = static_cast<std::byte *>(st.reservation.addr());
		if (header_bytes > 0) {
			if (auto mapped = map_anonymous(base, header_bytes, page_access::read_write, false); !mapped) {
				return std::unexpected{mapped.error()};
			}
		}

		std::byte *const segment = base + header_bytes;
		st.hot->segment_start = reinterpret_cast<uintptr_t>(segment);
		for (uint64_t i = 0; i < size_slots; ++i) {
			auto const &entry = st.rec.entries[i];
			void *const slot_addr = segment + i * st.rec.block_size;
			if (entry.size == 0) {
				if (auto mapped = map_anonymous(slot_addr, st.rec.block_size, page_access::read); !mapped) {
					return std::unexpected{mapped.error()};
				}
				st.hot->table.publish(i, slot_state::empty);
			} else {
				if (auto mapped = map_block_file(slot_addr, st.rec.block_size, st.store->block_path(entry));
					!mapped) {
					return std::unexpected{mapped.error()};
				}
				st.hot->table.publish(i, slot_state::clean);
			}
		}
		st.hot->table.set_extended_size(st.rec.size);

		if (!read_only) {
			for (auto const &entry : st.rec.entries) {
				if (entry.size != 0) {
					st.store->seed_durable(entry);
					st.store->add_reference(entry);
				}
			}
			auto swept = st.store->sweep(st.rec.entries);
			if (!swept) {
				return std::unexpected{swept.error()};
			}
			if (*swept > 0) {
				PRIVATEER_LOG(log_level::info, "open-time sweep removed {} unreferenced files", *swept);
			}

			// arm the write barrier
			if (auto installed = install_fault_handler(); !installed) {
				return std::unexpected{installed.error()};
			}
			if (auto armed = arm_thread_fault_stack(); !armed) {
				return std::unexpected{armed.error()};
			}
			install_fork_poison();
			st.hot->record.on_fault = &region_on_fault;
			st.hot->record.context = st.hot;
			st.hot->record.fork_poison = &st.hot->error;
			auto const start = st.hot->segment_start;
			if (auto added = global_registry().add(st.hot->record, start, start + st.rec.capacity); !added) {
				return std::unexpected{added.error()};
			}
			st.registered = true;
		}
		return reg;
	}

	void *region::segment() const noexcept {
		return static_cast<std::byte *>(state_->reservation.addr()) + state_->header_bytes;
	}

	void *region::segment_header() const noexcept {
		return state_->reservation.addr();
	}

	uint64_t region::size() const noexcept {
		return state_->hot->table.extended_size();
	}

	uint64_t region::capacity() const noexcept {
		return state_->rec.capacity;
	}

	uint64_t region::block_size() const noexcept {
		return state_->rec.block_size;
	}

	hash_algorithm region::algorithm() const noexcept {
		return state_->rec.algorithm;
	}

	bool region::read_only() const noexcept {
		return state_->read_only;
	}

	bool region::check_sanity() const noexcept {
		return state_->hot->error.load(std::memory_order_acquire) == 0;
	}

	result<> region::extend(uint64_t target_size) {
		auto &st = *state_;
		if (st.read_only) {
			return fail(errc::invalid_argument, "extend on a read-only region");
		}
		std::lock_guard lock{st.region_mutex};
		uint64_t const current = st.hot->table.extended_size();
		if (target_size <= current) {
			return {};
		}
		uint64_t const target = round_up(target_size, st.rec.block_size);
		if (target > st.rec.capacity) {
			return fail(errc::capacity_exceeded, "extend beyond the region capacity");
		}
		if (target / st.rec.block_size > vma_budget(st.vma_headroom)) {
			return fail(errc::vma_budget_exceeded, "extend would cross the VMA budget");
		}
		auto *const grown = static_cast<std::byte *>(segment()) + current;
		if (auto mapped = map_anonymous(grown, target - current, page_access::read); !mapped) {
			return std::unexpected{mapped.error()};
		}
		for (uint64_t i = current / st.rec.block_size; i < target / st.rec.block_size; ++i) {
			st.hot->table.publish(i, slot_state::empty);
		}
		st.hot->table.set_extended_size(target);
		return {};
	}

	result<> region::commit(bool durable) {
		auto &st = *state_;
		if (st.read_only) {
			// metall's flush and the reference backend's destructor call sync
			// without a read-only guard; a shared-locked datastore is never
			// mutated, so this succeeds untouched
			return {};
		}
		std::lock_guard const commit_lock{st.commit_mutex};
		if (!check_sanity()) {
			return fail(errc::datastore_inconsistent, "the region error flag is set");
		}
		auto &table = st.hot->table;
		uint64_t const block_size = st.rec.block_size;
		auto *const segment_base = static_cast<std::byte *>(segment());
		auto const slot_addr = [&](size_t slot) { return segment_base + slot * block_size; };

		// Phase 1: capture. The grown tail of a concurrent extend belongs to
		// the next epoch; this epoch persists the size captured here.
		uint64_t const captured_size = table.extended_size();
		size_t const captured_slots = captured_size / block_size;
		st.rec.entries.resize(captured_slots);

		struct captured_slot {
			size_t slot;
			bool empty;  // captured from dirty_empty: an empty commit
		};
		std::vector<captured_slot> captured;

		// Restores captured but unwritten slots after a failure, so no waiter
		// parks on a syncing slot forever. A write capture may already be
		// read-only; it must be writable again before dirty reappears, and
		// re-protecting a still-writable page is harmless.
		auto const restore_range = [&](size_t from, size_t to) {
			for (size_t i = from; i < to; ++i) {
				auto const &cap = captured[i];
				if (cap.empty) {
					table.publish(cap.slot, slot_state::dirty_empty);
					continue;
				}
				if (::mprotect(slot_addr(cap.slot), block_size, PROT_READ | PROT_WRITE) != 0) {
					table.publish(cap.slot, slot_state::poisoned);
					table.sub_dirty();
					st.hot->error.store(1, std::memory_order_release);
					continue;
				}
				table.publish(cap.slot, slot_state::dirty);
			}
		};

		for (size_t slot = 0; slot < captured_slots; ++slot) {
			for (;;) {
				slot_state const state = table.load(slot);
				if (state == slot_state::dirty) {
					if (table.try_claim(slot, slot_state::dirty, slot_state::syncing)) {
						captured.push_back({slot, false});
						break;
					}
					continue;
				}
				if (state == slot_state::dirty_empty) {
					if (table.try_claim(slot, slot_state::dirty_empty, slot_state::syncing)) {
						captured.push_back({slot, true});
						break;
					}
					continue;
				}
				if (state == slot_state::materializing) {
					(void) table.wait_changed(slot, state);
					continue;
				}
				if (state == slot_state::poisoned) {
					restore_range(0, captured.size());
					return fail(errc::region_poisoned, "a slot is dead after a failed protection change");
				}
				// clean and empty are already persisted; a freeing slot
				// resolves to dirty_empty and belongs to the next epoch
				break;
			}
		}

		// Freeze the write captures: one downgrade (and one TLB shootdown)
		// per contiguous run. When mprotect returns, no core holds a stale
		// writable entry, so the content is frozen.
		for (size_t i = 0; i < captured.size();) {
			if (captured[i].empty) {
				++i;
				continue;
			}
			size_t j = i + 1;
			while (j < captured.size() && !captured[j].empty &&
				   captured[j].slot == captured[j - 1].slot + 1) {
				++j;
			}
			size_t const run_slots = captured[j - 1].slot - captured[i].slot + 1;
			if (::mprotect(slot_addr(captured[i].slot), run_slots * block_size, PROT_READ) != 0) {
				int const freeze_errno = errno;
				for (size_t k = i; k < j; ++k) {
					table.publish(captured[k].slot, slot_state::poisoned);
					table.sub_dirty();
				}
				st.hot->error.store(1, std::memory_order_release);
				restore_range(0, i);
				restore_range(j, captured.size());
				errno = freeze_errno;
				return fail_errno(errc::io_error, "freeze captured slots");
			}
			i = j;
		}
		commit_phase_done(1);

		// Phase 2: write-out with per-slot release. A writer parked on a
		// captured slot resumes as soon as its own slot publishes.
		for (size_t idx = 0; idx < captured.size(); ++idx) {
			auto const &cap = captured[idx];
			auto &entry = st.rec.entries[cap.slot];
			if (cap.empty) {
				if (entry.size != 0) {
					st.store->drop_reference(entry);
					entry = block_digest{};
				}
				// the fresh anonymous zero mapping is already in place
				table.publish(cap.slot, slot_state::empty);
				continue;
			}
			std::span<std::byte const> const content{slot_addr(cap.slot), block_size};
			auto const name = hash_block(st.rec.algorithm, content);
			if (name != entry) {
				if (auto published = st.store->publish(name, content); !published) {
					restore_range(idx, captured.size());
					return std::unexpected{published.error()};
				}
				if (entry.size != 0) {
					st.store->drop_reference(entry);
				}
				st.store->add_reference(name);
				entry = name;
			}
			// One MAP_FIXED call replaces the private pages with the named
			// block file; a concurrent reader either reads the old identical
			// bytes or blocks inside its fault, never an unmapped window.
			if (auto mapped = map_block_file(slot_addr(cap.slot), block_size, st.store->block_path(entry));
				!mapped) {
				table.publish(cap.slot, slot_state::poisoned);
				table.sub_dirty();
				st.hot->error.store(1, std::memory_order_release);
				restore_range(idx + 1, captured.size());
				return std::unexpected{mapped.error()};
			}
			table.publish(cap.slot, slot_state::clean);
			table.sub_dirty();
		}
		commit_phase_done(2);

		// Phase 3: the durability barrier. Covers this commit's blocks and
		// every name inherited from earlier non-durable commits; the store
		// skips names already in the durable set.
		if (durable) {
			std::vector<block_digest> referenced;
			for (auto const &entry : st.rec.entries) {
				if (entry.size != 0) {
					referenced.push_back(entry);
				}
			}
			if (auto synced = st.store->make_durable(referenced); !synced) {
				return std::unexpected{synced.error()};
			}
			commit_phase_done(3);
		}

		// Phase 4: the atomic commit point.
		st.rec.size = captured_size;
		if (auto committed = st.rec.commit(st.segment_dir, durable); !committed) {
			return std::unexpected{committed.error()};
		}
		commit_phase_done(4);

		// Phase 5: reclaim. Errors are non-fatal and logged inside; a failed
		// name stays a candidate, and the open-time sweep is the backstop.
		if (durable) {
			st.store->reclaim();
			commit_phase_done(5);
		}
		return {};
	}

	namespace detail_region {

		slot_table &table_of(region &reg) noexcept {
			return reg.state_->hot->table;
		}

		bool deliver_fault(region &reg, uintptr_t addr, int signo) noexcept {
			return region_on_fault(reg.state_->hot->record, addr, signo);
		}

	}  // namespace detail_region

}  // namespace privateer
