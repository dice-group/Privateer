#include <privateer/region.hpp>

#include <privateer/block_store.hpp>
#include <privateer/executor.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/handler_text.hpp>
#include <privateer/logger.hpp>
#include <privateer/recipe.hpp>
#include <privateer/region_registry.hpp>
#include <privateer/rlimits.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>
#include <privateer/word_wait.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <boost/unordered/unordered_flat_set.hpp>

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

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_region {

		int (*mprotect_fn)(void *, size_t, int) = ::mprotect;
		void (*commit_phase_hook)(int) = nullptr;
		int (*link_fn)(char const *, char const *) = ::link;

	}  // namespace detail_region
#endif

	namespace {

		// the fault path's protection change; tests reroute it through the seam
		PRIVATEER_HANDLER_TEXT int protect_slot_for_write(void *addr, size_t len) {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::mprotect_fn(addr, len, PROT_READ | PROT_WRITE);
#else
			return ::mprotect(addr, len, PROT_READ | PROT_WRITE);
#endif
		}

		void commit_phase_done([[maybe_unused]] int phase) {
#ifdef PRIVATEER_TEST_HOOKS
			if (detail_region::commit_phase_hook != nullptr) {
				detail_region::commit_phase_hook(phase);
			}
#endif
		}

		int link_for_staging(char const *from, char const *to) {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::link_fn(from, to);
#else
			return ::link(from, to);
#endif
		}

		// Stages a self-contained segment: the shard skeleton, one hard link
		// per referenced block (per-file copy where link fails: EXDEV across
		// devices, EMLINK on an exhausted link count; the fallback unshares
		// the block, trading space for correctness), and the synced recipe
		// copy. The skeleton and links are not fsynced here: metall fsyncs
		// the whole staged tree before it publishes the datastore; the
		// recipe copy's content is the engine's own obligation.
		result<> stage_segment(recipe const &rec, block_store const &src_store, fs::path const &dst_dir) {
			std::error_code ec;
			fs::create_directories(dst_dir, ec);
			if (ec) {
				return std::unexpected{error{errc::io_error, ec.value(), "create the staging directory"}};
			}
			auto dst_store = block_store::create(dst_dir, false);
			if (!dst_store) {
				return std::unexpected{dst_store.error()};
			}
			boost::unordered_flat_set<block_digest, block_digest_hash> staged;
			for (auto const &entry : rec.entries) {
				if (entry.size == 0 || !staged.insert(entry).second) {
					continue;
				}
				fs::path const src = src_store.block_path(entry);
				fs::path const dst = dst_store->block_path(entry);
				if (link_for_staging(src.c_str(), dst.c_str()) != 0) {
					fs::copy_file(src, dst, ec);
					if (ec) {
						return std::unexpected{error{errc::io_error, ec.value(), "stage a block copy"}};
					}
				}
			}
			return rec.commit(dst_dir, true);
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
						if (protect_slot_for_write(slot_addr, hot.block_size) != 0) {
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

		// Set in the fork child. The executor pools' threads do not survive
		// a fork, so a region opened in the child must never wait on a
		// posted task; its commits run on the committing thread.
		std::atomic<bool> g_fork_child{false};

		// The engine is fork-unsafe: memory locks are not inherited, no
		// executor thread survives into the child, and held mutexes stay
		// locked. The child handler marks every open region so misuse fails
		// loudly instead of hanging or corrupting. Lock-free, because the
		// parent may fork while another thread holds the registry mutex.
		void poison_regions_in_fork_child() noexcept {
			g_fork_child.store(true, std::memory_order_release);
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
		size_t commit_workers = 1;
		bool read_only = false;
		std::mutex region_mutex;  // serializes extend and close bookkeeping
		std::mutex commit_mutex;  // one commit at a time; owns the recipe table and the store bookkeeping

		// Detached tasks the region owns on the executor: counted here,
		// joined at close. The commit write-out workers are not detached;
		// the commit itself joins them before it returns. The counter is
		// heap-shared with every task: the final decrement releases the
		// joiner, so the wake after it must touch memory the task co-owns,
		// not the region state the joiner may already be destroying.
		std::shared_ptr<std::atomic<uint32_t>> outstanding_tasks{
				std::make_shared<std::atomic<uint32_t>>(0)};
		std::mutex timer_mutex;
		std::vector<std::shared_ptr<asio::steady_timer>> timers;

		// the task's last action; only the co-owned counter is touched
		static void count_task_out(std::shared_ptr<std::atomic<uint32_t>> const &counter) {
			if (counter->fetch_sub(1, std::memory_order_acq_rel) == 1) {
				word_wake_all(*counter);
			}
		}

		// Posts a region-owned task on the work pool. A task that starts
		// after closing is set runs as a no-op; a throwing body records the
		// error flag. Both ways the task is counted out, so close never
		// waits on a task that will not signal.
		void post_task(std::function<void()> fn) {
			outstanding_tasks->fetch_add(1, std::memory_order_relaxed);
			try {
				asio::post(work_pool(), [this, counter = outstanding_tasks, fn = std::move(fn)] {
					if (hot->closing.load(std::memory_order_acquire) == 0) {
						try {
							fn();
						} catch (...) {
							hot->error.store(1, std::memory_order_release);
						}
					}
					count_task_out(counter);
				});
			} catch (...) {
				count_task_out(outstanding_tasks);
				throw;
			}
		}

		// Arms a one-shot timer on the timer pool. The handler always runs
		// exactly once and is counted like a task; aborted is true when the
		// wait was cancelled or closing began before the handler ran, and
		// the handler must not touch the region beyond its own bookkeeping
		// then.
		void start_timer(std::chrono::nanoseconds delay, std::function<void(bool aborted)> handler) {
			auto timer = std::make_shared<asio::steady_timer>(timer_pool(), delay);
			{
				std::lock_guard const lock{timer_mutex};
				if (hot->closing.load(std::memory_order_acquire) != 0) {
					return;  // close has already cancelled the registered timers
				}
				timers.push_back(timer);
			}
			outstanding_tasks->fetch_add(1, std::memory_order_relaxed);
			try {
				timer->async_wait([this, timer, counter = outstanding_tasks,
								   handler = std::move(handler)](std::error_code const &ec) {
					bool const aborted =
							static_cast<bool>(ec) || hot->closing.load(std::memory_order_acquire) != 0;
					try {
						handler(aborted);
					} catch (...) {
						hot->error.store(1, std::memory_order_release);
					}
					{
						std::lock_guard const lock{timer_mutex};
						std::erase(timers, timer);
					}
					count_task_out(counter);
				});
			} catch (...) {
				{
					std::lock_guard const lock{timer_mutex};
					std::erase(timers, timer);
				}
				count_task_out(outstanding_tasks);
				throw;
			}
		}

		// Cancels every registered timer. The cancel is posted to the timer
		// pool because a timer object is not safe against concurrent calls;
		// the one timer thread serializes the cancel with the handlers.
		void cancel_timers() {
			std::vector<std::shared_ptr<asio::steady_timer>> snapshot;
			{
				std::lock_guard const lock{timer_mutex};
				snapshot = timers;
			}
			for (auto const &timer : snapshot) {
				asio::post(timer_pool(), [timer] { timer->cancel(); });
			}
		}

		void join_tasks() {
			for (;;) {
				uint32_t const outstanding = outstanding_tasks->load(std::memory_order_acquire);
				if (outstanding == 0) {
					return;
				}
				(void) word_wait(*outstanding_tasks, outstanding);
			}
		}

		~state() {
			if (hot != nullptr) {
				// New faults forward as crashes from here on; the
				// application has quiesced its readers and writers.
				hot->closing.store(1, std::memory_order_seq_cst);
				word_wake_all(hot->table.governor_word());
				// Tasks that have not started yet run as no-ops; timer
				// handlers complete with aborted set. The join waits both
				// kinds out, so nothing touches the region past this point.
				cancel_timers();
				join_tasks();
				if (registered) {
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
		st.commit_workers = options.commit_workers != 0
									? options.commit_workers
									: std::max<size_t>(1, std::thread::hardware_concurrency());
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

			// Construct the process-wide pools before the region exists, so
			// a region owned by a static object constructed after this call
			// finds them alive when it is destroyed at static teardown.
			(void) work_pool();
			(void) timer_pool();

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

	result<> region::free_region(uint64_t offset, uint64_t nbytes) {
		auto &st = *state_;
		if (st.read_only) {
			return fail(errc::invalid_argument, "free_region on a read-only region");
		}

		// The shutdown handshake. Both sides are seq_cst: with weaker
		// orderings the counter increment and the closing store could each
		// miss the other (the store-buffer case) and the free would proceed
		// into teardown. Close waits this counter to zero before unmapping.
		st.hot->record.free_in_flight.fetch_add(1, std::memory_order_seq_cst);
		struct counter_guard {
			std::atomic<uint32_t> &counter;
			~counter_guard() { counter.fetch_sub(1, std::memory_order_release); }
		} const guard{st.hot->record.free_in_flight};
		if (st.hot->closing.load(std::memory_order_seq_cst) != 0) {
			return fail(errc::shutting_down, "free_region on a closing region");
		}

		auto &table = st.hot->table;
		uint64_t const block_size = st.rec.block_size;
		uint64_t const size = table.extended_size();
		if (offset >= size || nbytes == 0) {
			return {};
		}
		nbytes = std::min(nbytes, size - offset);
		// only slots fully inside the range; partial slots stay untouched
		uint64_t const first_slot = (offset + block_size - 1) / block_size;
		uint64_t const end_slot = (offset + nbytes) / block_size;
		auto *const segment_base = static_cast<std::byte *>(segment());

		for (uint64_t slot = first_slot; slot < end_slot; ++slot) {
			slot_state prior;
			for (;;) {
				slot_state const state = table.load(slot);
				if (state == slot_state::poisoned) {
					return fail(errc::region_poisoned, "a slot is dead after a failed protection change");
				}
				if (is_transient(state)) {
					(void) table.wait_changed(slot, state);
					continue;
				}
				if (table.try_claim(slot, state, slot_state::freeing)) {
					prior = state;
					break;
				}
			}
			// The claim precedes the remap, so no write can land during the
			// replacement: a first touch waits on freeing, and in-flight
			// writers cannot exist for a freed range (the allocator freed it).
			if (auto mapped = map_anonymous(segment_base + slot * block_size, block_size, page_access::read);
				!mapped) {
				table.publish(slot, slot_state::poisoned);
				if (prior == slot_state::dirty) {
					table.sub_dirty();
				}
				st.hot->error.store(1, std::memory_order_release);
				return std::unexpected{mapped.error()};
			}
			table.publish(slot, slot_state::dirty_empty);
			if (prior == slot_state::dirty) {
				table.sub_dirty();
			}
		}
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
		return commit_impl(durable);
	}

	result<> region::commit_impl(bool durable) {
		auto &st = *state_;
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

		// Phase 2: write-out with per-slot release, fanned out over the
		// commit workers. Each worker pulls the next captured slot, hashes,
		// publishes, remaps, and releases it; a writer parked on a captured
		// slot resumes as soon as its own slot publishes. A worker writes
		// only its own slot's recipe entry; the reference bookkeeping is
		// recorded per worker and applied after the join, so the store's
		// bookkeeping keeps a single owner.
		struct ref_delta {
			block_digest drop{};  // size zero: nothing to drop
			block_digest add{};   // size zero: nothing to add
		};
		struct worker_result {
			std::vector<ref_delta> deltas;
			std::optional<error> err;
		};
		// The pools' threads do not survive a fork; in a fork child the
		// write-out stays on the committing thread.
		size_t const configured_workers =
				g_fork_child.load(std::memory_order_acquire) ? 1 : st.commit_workers;
		size_t const worker_count = std::max<size_t>(1, std::min(configured_workers, captured.size()));
		std::vector<worker_result> results(worker_count);
		std::atomic<size_t> next_capture{0};
		std::atomic<uint32_t> write_out_failed{0};
		// Heap-shared join counter: the final decrement releases the
		// committing thread, whose frame may be gone by the time the wake
		// after it runs, so nothing past the decrement may touch the frame.
		auto const workers_left =
				std::make_shared<std::atomic<uint32_t>>(static_cast<uint32_t>(worker_count));
		auto const count_worker_out = [workers_left] {
			if (workers_left->fetch_sub(1, std::memory_order_acq_rel) == 1) {
				word_wake_all(*workers_left);
			}
		};

		auto const write_out = [&](worker_result &res) {
			for (;;) {
				if (write_out_failed.load(std::memory_order_acquire) != 0) {
					return;
				}
				size_t const idx = next_capture.fetch_add(1, std::memory_order_relaxed);
				if (idx >= captured.size()) {
					return;
				}
				auto const &cap = captured[idx];
				auto &entry = st.rec.entries[cap.slot];
				if (cap.empty) {
					if (entry.size != 0) {
						res.deltas.push_back({entry, block_digest{}});
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
						res.err = published.error();
						write_out_failed.store(1, std::memory_order_release);
						return;
					}
					res.deltas.push_back({entry, name});
					entry = name;
				}
				// One MAP_FIXED call replaces the private pages with the
				// named block file; a concurrent reader either reads the old
				// identical bytes or blocks inside its fault, never an
				// unmapped window.
				if (auto mapped =
							map_block_file(slot_addr(cap.slot), block_size, st.store->block_path(entry));
					!mapped) {
					table.publish(cap.slot, slot_state::poisoned);
					table.sub_dirty();
					st.hot->error.store(1, std::memory_order_release);
					res.err = mapped.error();
					write_out_failed.store(1, std::memory_order_release);
					return;
				}
				table.publish(cap.slot, slot_state::clean);
				table.sub_dirty();
			}
		};
		auto const run_worker = [&](worker_result &res) {
			try {
				write_out(res);
			} catch (...) {
				// allocation failure in the delta recording; the slot in
				// flight stays syncing and is restored after the join
				if (!res.err) {
					res.err = error{errc::io_error, ENOMEM, "commit write-out worker"};
				}
				write_out_failed.store(1, std::memory_order_release);
			}
		};

		for (size_t w = 1; w < worker_count; ++w) {
			asio::post(work_pool(), [&run_worker, &results, w, count_worker_out] {
				run_worker(results[w]);
				count_worker_out();
			});
		}
		run_worker(results[0]);
		count_worker_out();
		for (;;) {
			uint32_t const left = workers_left->load(std::memory_order_acquire);
			if (left == 0) {
				break;
			}
			(void) word_wait(*workers_left, left);
		}

		for (auto const &res : results) {
			for (auto const &delta : res.deltas) {
				if (delta.drop.size != 0) {
					st.store->drop_reference(delta.drop);
				}
				if (delta.add.size != 0) {
					st.store->add_reference(delta.add);
				}
			}
		}
		if (write_out_failed.load(std::memory_order_acquire) != 0) {
			// Restore every captured slot the write-out did not release, so
			// no waiter parks forever; released slots keep their release.
			// Only this commit moves a slot out of syncing, so the state
			// identifies the unreleased ones exactly.
			for (auto const &cap : captured) {
				if (table.load(cap.slot) != slot_state::syncing) {
					continue;
				}
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
			for (auto const &res : results) {
				if (res.err) {
					return std::unexpected{*res.err};
				}
			}
			return fail(errc::io_error, "commit write-out failed");
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

	result<> region::snapshot_to(fs::path const &staging_segment_dir) {
		auto &st = *state_;
		// Held across the commit and the link pass: a durable commit in
		// between could reclaim a block the just-committed recipe still
		// references, and the links would hit ENOENT.
		std::lock_guard const commit_lock{st.commit_mutex};
		if (!st.read_only) {
			if (auto committed = commit_impl(true); !committed) {
				return committed;
			}
		}
		return stage_segment(st.rec, *st.store, staging_segment_dir);
	}

	result<> region::copy(fs::path const &src_segment_dir, fs::path const &dst_segment_dir) {
		auto rec = recipe::load(src_segment_dir);
		if (!rec) {
			return std::unexpected{rec.error()};
		}
		auto src_store = block_store::open(src_segment_dir);
		if (!src_store) {
			return std::unexpected{src_store.error()};
		}
		if (auto validated = validate_blocks(*rec, *src_store); !validated) {
			return std::unexpected{validated.error()};
		}
		return stage_segment(*rec, *src_store, dst_segment_dir);
	}

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_region {

		slot_table &table_of(region &reg) noexcept {
			return reg.state_->hot->table;
		}

		bool deliver_fault(region &reg, uintptr_t addr, int signo) noexcept {
			return region_on_fault(reg.state_->hot->record, addr, signo);
		}

		void post_task(region &reg, std::function<void()> fn) {
			reg.state_->post_task(std::move(fn));
		}

		void start_timer(region &reg, std::chrono::nanoseconds delay,
						 std::function<void(bool aborted)> handler) {
			reg.state_->start_timer(delay, std::move(handler));
		}

	}  // namespace detail_region
#endif

}  // namespace privateer
