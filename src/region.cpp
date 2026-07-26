#include <privateer/region.hpp>

#include <privateer/block_store.hpp>
#include <privateer/executor.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/handler_text.hpp>
#include <privateer/logger.hpp>
#include <privateer/recipe.hpp>
#include <privateer/region_registry.hpp>
#include <privateer/resident.hpp>
#include <privateer/rlimits.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>
#include <privateer/word_wait.hpp>

#include <algorithm>
#include <cerrno>
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

		// Runs its action when the enclosing scope leaves, on every path
		// including an exception. Used where a failure must not skip a
		// release: a claimed slot state, or a join counter that a waiter
		// depends on. The action must not throw.
		template<typename F>
		struct scope_guard {
			explicit scope_guard(F fn) : action{std::move(fn)} {}
			~scope_guard() { action(); }
			scope_guard(scope_guard const &) = delete;
			scope_guard &operator=(scope_guard const &) = delete;

		private:
			F action;
		};

		template<typename F>
		scope_guard(F) -> scope_guard<F>;

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
			// Count of slots in poisoned state, the recovery cue: the handler
			// increments when it poisons, every heal decrements. The governed
			// cleaner runs a batch whenever it is nonzero, so recovery does
			// not wait for the dirty budget.
			std::atomic<uint64_t> poisoned{0};
			uintptr_t segment_start = 0;
			uint64_t block_size = 0;
			// Dirty budget thresholds the handler reads on every fault.
			// gov_soft_cross is the dirty-slot count whose reach crosses the
			// soft watermark; 0 means no dirty budget. gov_hard_bytes is the
			// hard watermark; 0 means writers never wait.
			uint64_t gov_soft_cross = 0;
			uint64_t gov_hard_bytes = 0;
			int64_t gov_hard_timeout_ns = 0;
			// longest a writer parked on a poisoned slot waits for recovery
			int64_t poison_timeout_ns = 0;
		};

		// the resident sweep's default probe: the process Pss
		result<uint64_t> resident_pss_bytes() {
			auto const usage = read_resident_usage();
			if (!usage) {
				return std::unexpected{usage.error()};
			}
			return usage->pss;
		}

		// The resident sweep's default trim syscall. PAGEOUT is
		// non-destructive under every race: at worst it pushes pages of a
		// just-dirtied slot to swap, wasted I/O but never a lost write.
		int pageout_range([[maybe_unused]] void *addr, [[maybe_unused]] size_t len) {
#ifdef __linux__
			return ::madvise(addr, len, MADV_PAGEOUT);
#else
			return 0;
#endif
		}

	}  // namespace

#ifdef PRIVATEER_TEST_HOOKS
	namespace detail_region {

		int (*mprotect_fn)(void *, size_t, int) = ::mprotect;
		void (*commit_phase_hook)(int) = nullptr;
		int (*link_fn)(char const *, char const *) = ::link;
		int64_t (*clock_fn)() = monotonic_now_ns;
		void (*cleaner_slot_hook)(size_t) = nullptr;
		result<uint64_t> (*resident_bytes_fn)() = resident_pss_bytes;
		int (*pageout_fn)(void *, size_t) = pageout_range;
		bool (*commit_post_fails_fn)(size_t) = nullptr;
		bool (*cleaner_write_fails_fn)(size_t) = nullptr;
		bool (*cleaner_durability_fails_fn)() = nullptr;

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

		// the cleaner's time source; tests replace it for deterministic backoff
		int64_t cleaner_now_ns() {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::clock_fn();
#else
			return monotonic_now_ns();
#endif
		}

		// Whether posting the commit write-out worker with this index must
		// fail; tests reroute it through the seam to exercise the fan-out's
		// failure path. The engine always posts.
		bool commit_post_fails([[maybe_unused]] size_t worker) {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::commit_post_fails_fn != nullptr &&
				   detail_region::commit_post_fails_fn(worker);
#else
			return false;
#endif
		}

		// Whether the cleaner's block write for this slot must fail, the way
		// ENOSPC would; tests reroute it through the seam.
		bool cleaner_write_fails([[maybe_unused]] size_t slot) {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::cleaner_write_fails_fn != nullptr &&
				   detail_region::cleaner_write_fails_fn(slot);
#else
			return false;
#endif
		}

		// Whether the cleaner's eager durability barrier must fail, the way
		// a failed fsync would; tests reroute it through the seam.
		bool cleaner_durability_fails() {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::cleaner_durability_fails_fn != nullptr &&
				   detail_region::cleaner_durability_fails_fn();
#else
			return false;
#endif
		}

		void cleaner_slot_done([[maybe_unused]] size_t slot) {
#ifdef PRIVATEER_TEST_HOOKS
			if (detail_region::cleaner_slot_hook != nullptr) {
				detail_region::cleaner_slot_hook(slot);
			}
#endif
		}

		// the resident sweep's probe and trim; tests reroute both seams
		result<uint64_t> resident_bytes_now() {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::resident_bytes_fn();
#else
			return resident_pss_bytes();
#endif
		}

		int pageout(void *addr, size_t len) {
#ifdef PRIVATEER_TEST_HOOKS
			return detail_region::pageout_fn(addr, len);
#else
			return pageout_range(addr, len);
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
			// One deadline each bounds the total hard-mark wait and the total
			// poisoned-recovery wait of this fault, across re-loops and
			// spurious wakeups.
			int64_t gate_deadline = -1;
			int64_t poison_deadline = -1;
			for (;;) {
				slot_state const state = hot.table.load(slot);
				switch (state) {
					case slot_state::empty:
					case slot_state::clean:
					case slot_state::dirty_empty: {
						// The hard-mark gate: materializing this slot must not
						// take dirty bytes above the hard watermark. The wait
						// parks on the governor word, which every counter
						// decrease bumps; the drain (cleaner, committer,
						// freer) runs independently of this thread, so the
						// wait is backpressure, not a deadlock risk. After
						// the timeout the write proceeds and overshoots by
						// one block.
						while (hot.gov_hard_bytes != 0) {
							// value first, condition second: a decrease after
							// this load changes the word and the wait returns
							uint32_t const observed =
									hot.table.governor_word().load(std::memory_order_acquire);
							if (hot.closing.load(std::memory_order_acquire) != 0) {
								return false;  // close released the waiters; not quiesced
							}
							uint64_t const dirty = hot.table.dirty_slots();
							if ((dirty + 1) * hot.block_size <= hot.gov_hard_bytes) {
								break;
							}
							int64_t const now = monotonic_now_ns();
							if (gate_deadline < 0) {
								gate_deadline = now + hot.gov_hard_timeout_ns;
							} else if (now >= gate_deadline) {
								break;  // bounded stall: overshoot by one block
							}
							(void) word_wait_for(hot.table.governor_word(), observed, gate_deadline - now);
						}
						if (!hot.table.try_claim(slot, state, slot_state::materializing)) {
							continue;  // the slot moved; re-examine
						}
						uint64_t const dirty_now = hot.table.add_dirty();
						void *const slot_addr =
								reinterpret_cast<void *>(hot.segment_start + slot * hot.block_size);
						if (protect_slot_for_write(slot_addr, hot.block_size) != 0) {
							// VMA exhaustion, a process-wide condition that can
							// clear; the failed syscall changed nothing, so the
							// mapping and content are intact. The handler
							// cannot retry (signal context, nothing it waits on
							// drains VMAs), so it hands the slot over: balance
							// the count, publish poisoned, and wake the
							// governor word as the recovery request (the
							// cleaner waits on that word; the wake comes after
							// the publish so a woken cleaner finds the slot
							// poisoned). Then park below like any waiter.
							hot.poisoned.fetch_add(1, std::memory_order_release);
							hot.table.sub_dirty();
							hot.table.publish(slot, slot_state::poisoned);
							hot.table.wake_governor();
							continue;
						}
						hot.table.publish(slot, slot_state::dirty);
						if (hot.gov_soft_cross != 0 && dirty_now >= hot.gov_soft_cross) {
							// At or above the soft watermark every new dirty
							// slot wakes the governor word; the first of
							// these is the crossing that activates the
							// cleaner. The wake comes after the terminal
							// publish: a cleaner that scanned this slot as
							// materializing and found nothing to write back
							// either parks after this bump and is woken, or
							// parks before it and its compare-value wait
							// refuses; both ways its rescan sees the slot
							// dirty. A wake before the publish would leave a
							// window where the cleaner parks for a full
							// interval while counted dirt exists but no slot
							// reads dirty, and writers blocked at the hard
							// mark would then drain only through their
							// timeouts.
							hot.table.wake_governor();
						}
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
					case slot_state::poisoned: {
						// Recovery runs where retrying is possible: the
						// cleaner and commit capture retry the protection
						// change under the commit mutex and republish dirty;
						// free_region heals by remap. Park with a timeout, so
						// a slot whose recovery never succeeds cannot hold
						// this writer forever; on timeout the write is lost
						// and the segment may be torn mid-operation, so the
						// terminal flag is set and the fault forwards.
						int64_t const now = monotonic_now_ns();
						if (poison_deadline < 0) {
							poison_deadline = now + hot.poison_timeout_ns;
						} else if (now >= poison_deadline) {
							hot.error.store(1, std::memory_order_release);
							return false;
						}
						(void) hot.table.wait_changed_for(slot, state, poison_deadline - now);
						continue;  // re-examine; recovery may have republished dirty
					}
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

		// The process-level resident sweep (defined after region::state).
		// Registration keys on the owning state object.
		void resident_sweeper_register(void const *owner, region_hot *hot,
									   governor_options const &governor);
		void resident_sweeper_unregister(void const *owner) noexcept;
		void touch_resident_sweeper();

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
		cleaner_options cleaner;
		governor_options governor;
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

		[[nodiscard]] std::byte *segment_base() const noexcept {
			return static_cast<std::byte *>(reservation.addr()) + header_bytes;
		}

		// Per-slot write-back bookkeeping, owned by the cleaner and guarded
		// by the commit mutex. Times come from the cleaner's clock.
		struct cleaner_slot_meta {
			int64_t first_dirty_ns = -1;  // when the sweep first saw the slot dirty; -1 while not dirty
			int64_t cleaned_at_ns = -1;   // when the cleaner last wrote the slot back; -1 never
			int64_t backoff_ns = 0;       // the current re-dirty backoff; 0 none
			int64_t eligible_at_ns = 0;   // not written back before this time, unless overridden
		};
		std::vector<cleaner_slot_meta> cleaner_meta;

		// Batch-level failure bookkeeping. The counter and the backoff time
		// are guarded by the commit mutex; the disabled flag is also read by
		// the scheduling paths outside it.
		size_t cleaner_failures = 0;        // consecutive failed batches
		int64_t cleaner_backoff_until = 0;  // no batch before this cleaner-clock time
		std::atomic<uint32_t> cleaner_off{0};

		// Retries the protection change a poisoned slot failed. The caller
		// holds the commit mutex, so retrying is possible here (normal thread
		// context, and the condition behind the failure can have cleared). On
		// success the block counts dirty again, dirty is republished, and the
		// parked writer's retried store lands. On failure the slot stays
		// poisoned and the next cycle retries. Returns whether the slot was
		// healed; false also when a concurrent free_region claimed the slot
		// first (its remap heals instead).
		bool recover_poisoned(size_t slot) {
			auto &table = hot->table;
			if (!table.try_claim(slot, slot_state::poisoned, slot_state::syncing)) {
				return false;
			}
			auto *const addr = segment_base() + slot * rec.block_size;
			if (protect_slot_for_write(addr, rec.block_size) != 0) {
				PRIVATEER_LOG(log_level::warning, "recovery of poisoned slot {} failed (errno {})", slot,
							  errno);
				table.publish(slot, slot_state::poisoned);
				return false;
			}
			table.add_dirty();
			hot->poisoned.fetch_sub(1, std::memory_order_release);
			table.publish(slot, slot_state::dirty);
			PRIVATEER_LOG(log_level::info, "poisoned slot {} recovered", slot);
			return true;
		}

		// One write-back batch: scans for dirty slots, orders them coldest
		// first, and runs the commit's capture and write-out for them under
		// one commit-mutex hold. The batch writes every victim's block file
		// first and remaps only after that (in eager-durable mode, after the
		// batch's durability barrier), so any failure can still unwind: the
		// recipe-table entry and the references revert and the slot returns
		// to dirty while its private pages exist. The on-disk recipe is
		// untouched; the recipe table entries the batch updates are picked
		// up by the next commit, so a crash anywhere in here leaves only
		// sweepable block files behind. A failed batch backs the cleaner
		// off, and repeated failures disable it (failure_limit); only a slot
		// the batch cannot restore is poisoned. override_backoff takes every
		// dirty slot regardless of its re-dirty backoff: the hard-watermark
		// drain, where cleaner throughput must not wait out backoff timers.
		// Returns the number of slots written back.
		size_t clean_batch(bool override_backoff) {
			std::lock_guard const commit_lock{commit_mutex};
			if (hot->error.load(std::memory_order_acquire) != 0 ||
				cleaner_off.load(std::memory_order_acquire) != 0) {
				return 0;
			}
			auto &table = hot->table;
			uint64_t const block_size = rec.block_size;
			int64_t const now = cleaner_now_ns();
			if (now < cleaner_backoff_until) {
				return 0;  // backing off after a failed batch
			}
			size_t const slots = table.extended_size() / block_size;
			if (cleaner_meta.size() < slots) {
				cleaner_meta.resize(slots);
			}
			if (rec.entries.size() < slots) {
				rec.entries.resize(slots);
			}

			// The sweep: record first-dirty times, apply the re-dirty rule,
			// and collect the eligible victims. A slot that turns dirty again
			// within backoff_cap of its last write-back waits backoff_base
			// before it is eligible, doubling per repeat up to backoff_cap;
			// a longer quiet period clears the backoff.
			std::vector<size_t> victims;
			for (size_t slot = 0; slot < slots; ++slot) {
				auto &meta = cleaner_meta[slot];
				slot_state const state = table.load(slot);
				if (state == slot_state::poisoned) {
					// The cleaner is the recovery actor: the handler's wake
					// after poisoning lands in the cleaner's governor-word
					// wait, and each cycle retries here. A healed slot is
					// dirty now and stays out of this batch; the writer's
					// parked store lands first.
					(void) recover_poisoned(slot);
					meta.first_dirty_ns = -1;
					continue;
				}
				if (state != slot_state::dirty) {
					meta.first_dirty_ns = -1;
					continue;
				}
				if (meta.first_dirty_ns < 0) {
					meta.first_dirty_ns = now;
					if (meta.cleaned_at_ns >= 0 && now - meta.cleaned_at_ns < cleaner.backoff_cap.count()) {
						meta.backoff_ns = meta.backoff_ns == 0
												  ? cleaner.backoff_base.count()
												  : std::min(meta.backoff_ns * 2, cleaner.backoff_cap.count());
					} else {
						meta.backoff_ns = 0;
					}
					meta.eligible_at_ns = now + meta.backoff_ns;
				}
				if (override_backoff || now >= meta.eligible_at_ns) {
					victims.push_back(slot);
				}
			}
			std::sort(victims.begin(), victims.end(), [&](size_t a, size_t b) {
				return cleaner_meta[a].first_dirty_ns < cleaner_meta[b].first_dirty_ns;
			});
			if (victims.size() > cleaner.batch_slots) {
				victims.resize(cleaner.batch_slots);
			}

			// A slot the batch captured but has not released yet. prior is
			// the recipe-table entry before the batch; replaced says the
			// entry now names the new block.
			struct pending_slot {
				size_t slot;
				block_digest prior;
				bool replaced;
			};
			std::vector<pending_slot> pendings;
			std::vector<block_digest> written;      // entries the barrier must cover
			std::vector<block_digest> fresh_names;  // block files this batch created
			// Claims are held from the capture loop on, so the engine's own
			// bookkeeping must not allocate with a claim in hand (the same
			// rule as the commit's capture); the loops below only push into
			// this space.
			pendings.reserve(victims.size());
			written.reserve(victims.size());
			fresh_names.reserve(victims.size());
			bool failed = false;

			// Restores one pending slot: the recipe-table entry and the
			// references revert, the freeze reverses, and the slot returns
			// to dirty. Only a failed reversal poisons; the caller wakes the
			// governor word once for the batch.
			auto const unwind = [&](pending_slot const &p) {
				auto &entry = rec.entries[p.slot];
				if (p.replaced) {
					store->drop_reference(entry);
					if (p.prior.size != 0) {
						store->add_reference(p.prior);
					}
					entry = p.prior;
				}
				auto *const addr = segment_base() + p.slot * block_size;
				if (::mprotect(addr, block_size, PROT_READ | PROT_WRITE) != 0) {
					hot->poisoned.fetch_add(1, std::memory_order_release);
					table.publish(p.slot, slot_state::poisoned);
					table.sub_dirty();
					hot->error.store(1, std::memory_order_release);
					PRIVATEER_LOG(log_level::error, "cleaner cannot restore slot {} (errno {})", p.slot,
								  errno);
					return;
				}
				table.publish(p.slot, slot_state::dirty);
			};

			// Capture and write: claim, freeze, hash, write the block file.
			for (size_t const slot : victims) {
				if (hot->closing.load(std::memory_order_acquire) != 0) {
					break;
				}
				if (!table.try_claim(slot, slot_state::dirty, slot_state::syncing)) {
					continue;  // the slot moved since the scan; its new owner has it
				}
				auto *const addr = segment_base() + slot * block_size;
				// freeze: when mprotect returns, no core holds a stale
				// writable entry, so the content below is what gets hashed
				if (::mprotect(addr, block_size, PROT_READ) != 0) {
					// a failed downgrade may have covered part of the range;
					// the unwind reverses it before the slot goes back to dirty
					PRIVATEER_LOG(log_level::warning, "cleaner cannot freeze slot {} (errno {})", slot,
								  errno);
					unwind({slot, {}, false});
					failed = true;
					break;
				}
				std::span<std::byte const> const content{addr, block_size};
				auto const name = hash_block(rec.algorithm, content);
				auto &entry = rec.entries[slot];
				if (name == entry) {
					pendings.push_back({slot, entry, false});
					written.push_back(entry);
					continue;
				}
				auto const published =
						cleaner_write_fails(slot)
								? result<bool>{std::unexpected{error{errc::io_error, ENOSPC,
																	 "write a block file"}}}
								: store->publish(name, content);
				if (!published) {
					// Nothing is lost: the slot goes back to dirty and the
					// next commit retries the write-out and reports the
					// failure to a caller.
					PRIVATEER_LOG(log_level::warning, "cleaner cannot write slot {} back: {}", slot,
								  to_string(published.error()));
					unwind({slot, {}, false});
					failed = true;
					break;
				}
				if (*published) {
					fresh_names.push_back(name);
				}
				pendings.push_back({slot, entry, true});
				if (entry.size != 0) {
					store->drop_reference(entry);
				}
				store->add_reference(name);
				entry = name;
				written.push_back(name);
			}

			// Eager durability: the full durable-name contract for what the
			// batch wrote, both halves batched (the file contents, then the
			// shard directory entries), before any of the batch's remaps. A
			// name recorded after only the file sync would let a later
			// durable commit skip the directory sync. A failed barrier
			// unwinds the whole batch while the private pages still exist,
			// and drops the files this batch created: a later publish of the
			// same content must not dedup against a file whose sync failed,
			// because a re-synced file cannot be trusted; the rewrite
			// through a fresh file is the trustworthy retry.
			if (cleaner.mode == cleaner_mode::eager_durable && !written.empty()) {
				auto const synced = cleaner_durability_fails()
											? result<>{std::unexpected{error{errc::io_error, EIO,
																			 "sync a block file"}}}
											: store->make_durable(written);
				if (!synced) {
					PRIVATEER_LOG(log_level::warning, "cleaner durability barrier failed: {}",
								  to_string(synced.error()));
					for (auto const &p : pendings) {
						unwind(p);
					}
					for (auto const &name : fresh_names) {
						store->discard_unreferenced(name);
					}
					pendings.clear();
					failed = true;
				}
			}

			// Remap and release. A failure here unwinds this slot and the
			// rest of the batch; the slots already released keep their
			// release.
			size_t cleaned = 0;
			for (size_t i = 0; i < pendings.size(); ++i) {
				auto const &p = pendings[i];
				auto *const addr = segment_base() + p.slot * block_size;
				if (auto mapped = map_block_file(addr, block_size, store->block_path(rec.entries[p.slot]));
					!mapped) {
					PRIVATEER_LOG(log_level::warning, "cleaner cannot remap slot {}: {}", p.slot,
								  to_string(mapped.error()));
					for (size_t k = i; k < pendings.size(); ++k) {
						unwind(pendings[k]);
					}
					failed = true;
					break;
				}
				table.publish(p.slot, slot_state::clean);
				table.sub_dirty();
				auto &meta = cleaner_meta[p.slot];
				meta.cleaned_at_ns = now;
				meta.first_dirty_ns = -1;
				++cleaned;
				cleaner_slot_done(p.slot);
			}

			if (failed) {
				// Republished dirt with no counter change: wake a parked
				// cleaner wait and any writer at the hard mark, the same
				// reason the commit's restore paths wake.
				table.wake_governor();
				++cleaner_failures;
				if (cleaner.failure_limit != 0 && cleaner_failures >= cleaner.failure_limit) {
					cleaner_off.store(1, std::memory_order_release);
					PRIVATEER_LOG(log_level::error,
								  "cleaner disabled after {} consecutive failed batches; "
								  "write-back degrades to commits",
								  cleaner_failures);
				} else if (cleaner.backoff_base.count() > 0) {
					auto const shift = static_cast<int64_t>(std::min<size_t>(cleaner_failures - 1, 20));
					cleaner_backoff_until =
							now + std::min(cleaner.backoff_base.count() << shift,
										   cleaner.backoff_cap.count());
				}
			} else if (cleaned > 0) {
				cleaner_failures = 0;
				cleaner_backoff_until = 0;
			}
			return cleaned;
		}

		// The background write-back chain: a timer tick posts one batch on
		// the work pool, and the batch re-arms the timer. Both links refuse
		// to run once closing is set, so the chain ends at close, and every
		// link is counted and joined there. This is the cleaner without a
		// dirty budget; with one, governor_cleaner_loop replaces it.
		void schedule_cleaner() {
			start_timer(cleaner.interval, [this](bool aborted) {
				if (aborted) {
					return;
				}
				post_task([this] {
					(void) clean_batch(false);
					if (cleaner_off.load(std::memory_order_acquire) == 0) {
						schedule_cleaner();
					}
				});
			});
		}

		// The cleaner under a dirty budget: a self-re-posting work-pool
		// task. Each step parks on the governor word, so the handler's
		// soft-mark crossing wake activates it immediately, with the
		// cleaner interval as the timeout fallback for the periodic sweep.
		// Above the soft watermark it drains cold-first until the low
		// target; while a new fault would block at the hard mark it ignores
		// the re-dirty backoff, so drain throughput is cleaner throughput,
		// never backoff timers. One step blocks its pool thread for at most
		// one wait plus one batch, then re-posts, so queued work (commit
		// write-out workers) is never starved behind the cleaner. Close
		// bumps and wakes the governor word and the re-post runs as a
		// counted no-op, so the chain ends at close and the join covers it.
		void governor_cleaner_step(bool draining) {
			// value first, closing second, budget third: a close or a
			// counter change after the value load wakes the wait below
			// immediately, and closing stored before the close-side bump is
			// visible here
			uint32_t const observed = hot->table.governor_word().load(std::memory_order_acquire);
			if (hot->closing.load(std::memory_order_acquire) != 0) {
				return;
			}
			if (cleaner_off.load(std::memory_order_acquire) != 0) {
				// Self-disabled after repeated failures: the chain ends,
				// write-back degrades to commits, and hard-mark waits drain
				// through their timeouts.
				return;
			}
			uint64_t const block_size = rec.block_size;
			uint64_t const dirty_bytes = hot->table.dirty_slots() * block_size;
			// A poisoned slot runs a batch regardless of the budget: the batch
			// is where recovery retries, and the handler's wake after
			// poisoning is what woke this step.
			bool const have_poisoned = hot->poisoned.load(std::memory_order_acquire) != 0;
			if (have_poisoned || dirty_bytes > (draining ? governor.dirty_low : governor.dirty_soft)) {
				bool const writers_blocked =
						governor.dirty_hard != 0 && dirty_bytes + block_size > governor.dirty_hard;
				if (clean_batch(writers_blocked) == 0) {
					// nothing eligible (transients, backoff below the hard
					// mark, or the error flag): wait for a change instead
					// of spinning
					(void) word_wait_for(hot->table.governor_word(), observed, cleaner.interval.count());
				}
				post_task([this] { governor_cleaner_step(true); });
				return;
			}
			if (word_wait_for(hot->table.governor_word(), observed, cleaner.interval.count()) == observed) {
				// a full interval without governor activity: the periodic
				// backoff-respecting sweep
				(void) clean_batch(false);
			}
			post_task([this] { governor_cleaner_step(false); });
		}

		~state() {
			if (hot != nullptr) {
				// New faults forward as crashes from here on; the
				// application has quiesced its readers and writers. The
				// governor wake bumps the word, so a waiter about to park on
				// a stale value re-checks instead of sleeping through it; a
				// writer still parked at the hard mark wakes, sees closing,
				// and crashes with the forwarded fault (the documented
				// contract violation).
				hot->closing.store(1, std::memory_order_seq_cst);
				hot->table.wake_governor();
				// Tasks that have not started yet run as no-ops; timer
				// handlers complete with aborted set. The join waits both
				// kinds out, so nothing touches the region past this point.
				cancel_timers();
				join_tasks();
				resident_sweeper_unregister(this);
				if (registered) {
					global_registry().remove(hot->record);
				}
				hot->~region_hot();
				hot = nullptr;
			}
		}
	};

	namespace {

		// resident page count of one slot, via mincore; the resident budget
		// registers on Linux only, so elsewhere this never runs
		uint64_t resident_bytes_of([[maybe_unused]] void *addr, [[maybe_unused]] size_t len,
								   [[maybe_unused]] std::vector<unsigned char> &vec) {
#ifdef __linux__
			size_t const psize = page_size();
			vec.resize(len / psize);
			if (::mincore(addr, len, vec.data()) != 0) {
				return 0;
			}
			uint64_t pages = 0;
			for (unsigned char const flags : vec) {
				pages += flags & 1;
			}
			return pages * psize;
#else
			return 0;
#endif
		}

		// The process-level resident sweep: one periodic task serving every
		// region with a resident budget. One mutex guards the entry set and
		// the whole pass, so a region's close removing its entry under the
		// mutex is the shutdown handshake: once unregister returns, the
		// sweep no longer touches the region.
		//
		// The budget is advisory, soft watermark and low target only:
		// residency is reader-inflatable, so the sweep converges toward the
		// target instead of enforcing it. Accounting is the process Pss,
		// one smaps_rollup read per pass; mincore is used only to pick
		// victims (it counts page-cache residency, an over-count under
		// other openers). Victims are resident pages of clean and empty
		// slots, taken in slot order, and the trim effort is split across
		// regions in proportion to their trimmable resident bytes.
		struct resident_sweeper {
			struct entry {
				void const *owner;
				region_hot *hot;
				uint64_t soft;
				uint64_t low;
				int64_t interval_ns;
			};

			std::mutex mutex;
			std::vector<entry> entries;
			bool armed = false;
			// Set when the residency probe fails: procfs can be restricted
			// in containers, and a budget that cannot measure cannot trim.
			// Only the resident budget dies; the store stays healthy. A new
			// registration gives the probe another chance.
			bool disabled = false;

			static resident_sweeper &instance() {
				static resident_sweeper sweeper;
				return sweeper;
			}

			void register_entry(void const *owner, region_hot *hot, governor_options const &governor) {
				std::lock_guard const lock{mutex};
				entries.push_back({owner, hot, governor.resident_soft, governor.resident_low,
								   governor.sweep_interval.count()});
				disabled = false;
				if (!armed) {
					armed = true;
					arm(min_interval_ns());
				}
			}

			void unregister_entry(void const *owner) noexcept {
				std::lock_guard const lock{mutex};
				std::erase_if(entries, [owner](entry const &e) { return e.owner == owner; });
			}

			[[nodiscard]] int64_t min_interval_ns() const {
				int64_t interval = INT64_MAX;
				for (auto const &e : entries) {
					interval = std::min(interval, e.interval_ns);
				}
				return interval;
			}

			// One-shot timer; the pass re-arms while entries exist. The
			// timer is owned by its own completion handler, the same
			// pattern as the region timers; nothing cancels it, and a fire
			// after the last unregister finds the set empty and stops the
			// chain. On allocation failure the chain ends and the kernel's
			// own reclaim remains (the budget is advisory).
			void arm(int64_t delay_ns) {
				try {
					auto timer = std::make_shared<asio::steady_timer>(timer_pool(),
																	  std::chrono::nanoseconds{delay_ns});
					timer->async_wait([timer](std::error_code const &ec) {
						if (ec) {
							return;
						}
						try {
							asio::post(work_pool(), [] { instance().pass(true); });
						} catch (...) {
							std::lock_guard const lock{instance().mutex};
							instance().armed = false;
						}
					});
				} catch (...) {
					armed = false;
				}
			}

			// one sweep pass; returns the bytes it asked the kernel to push out
			uint64_t pass(bool rearm) {
				std::lock_guard const lock{mutex};
				if (entries.empty() || disabled) {
					armed = false;
					return 0;
				}
				uint64_t const asked = sweep_locked();
				if (disabled) {
					armed = false;
					return asked;
				}
				if (rearm) {
					arm(min_interval_ns());
				}
				return asked;
			}

			uint64_t sweep_locked() {
				uint64_t soft = UINT64_MAX;
				uint64_t low = UINT64_MAX;
				for (auto const &e : entries) {
					soft = std::min(soft, e.soft);
					low = std::min(low, e.low);
				}
				auto const resident = resident_bytes_now();
				if (!resident) {
					disabled = true;
					PRIVATEER_LOG(log_level::error,
								  "resident sweep cannot read residency, resident budget disabled: {}",
								  to_string(resident.error()));
					return 0;
				}
				if (*resident <= soft) {
					return 0;
				}
				uint64_t const needed = *resident - low;

				// pass 1: the trimmable resident bytes per region, victims
				// in slot order
				struct victim {
					void *addr;
					uint64_t bytes;
				};
				struct plan {
					uint64_t block_size;
					uint64_t trimmable = 0;
					std::vector<victim> victims;
				};
				std::vector<plan> plans(entries.size());
				std::vector<unsigned char> vec;
				uint64_t total_trimmable = 0;
				for (size_t i = 0; i < entries.size(); ++i) {
					auto const &e = entries[i];
					auto &p = plans[i];
					p.block_size = e.hot->block_size;
					size_t const slots = e.hot->table.extended_size() / p.block_size;
					for (size_t slot = 0; slot < slots; ++slot) {
						slot_state const state = e.hot->table.load(slot);
						if (state != slot_state::clean && state != slot_state::empty) {
							continue;
						}
						auto *const addr =
								reinterpret_cast<void *>(e.hot->segment_start + slot * p.block_size);
						uint64_t const bytes = resident_bytes_of(addr, p.block_size, vec);
						if (bytes == 0) {
							continue;
						}
						p.victims.push_back({addr, bytes});
						p.trimmable += bytes;
					}
					total_trimmable += p.trimmable;
				}
				if (total_trimmable == 0) {
					return 0;
				}

				// pass 2: PAGEOUT until each region's share of the needed
				// trim is reached. A stale state race is harmless: PAGEOUT
				// on a just-dirtied slot pushes fresh private pages to swap
				// and they swap back in on access, wasted I/O, never a lost
				// write.
				uint64_t asked = 0;
				for (auto const &p : plans) {
					auto const share = static_cast<uint64_t>(
							(static_cast<unsigned __int128>(needed) * p.trimmable + total_trimmable - 1) /
							total_trimmable);
					uint64_t trimmed = 0;
					for (auto const &v : p.victims) {
						if (trimmed >= share) {
							break;
						}
						if (pageout(v.addr, p.block_size) == 0) {
							trimmed += v.bytes;
						}
					}
					asked += trimmed;
				}
				return asked;
			}
		};

		void resident_sweeper_register(void const *owner, region_hot *hot,
									   governor_options const &governor) {
			resident_sweeper::instance().register_entry(owner, hot, governor);
		}

		void resident_sweeper_unregister(void const *owner) noexcept {
			resident_sweeper::instance().unregister_entry(owner);
		}

		void touch_resident_sweeper() {
			(void) resident_sweeper::instance();
		}

	}  // namespace

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
		if (options.poison_timeout.count() <= 0) {
			return fail(errc::invalid_argument, "poison_timeout must be positive");
		}
		if (options.cleaner.mode != cleaner_mode::off) {
			if (options.cleaner.interval.count() <= 0 || options.cleaner.batch_slots == 0) {
				return fail(errc::invalid_argument, "the cleaner needs a positive interval and batch size");
			}
			if (options.cleaner.backoff_base.count() < 0 ||
				options.cleaner.backoff_base > options.cleaner.backoff_cap) {
				return fail(errc::invalid_argument, "cleaner backoff_base must lie within [0, backoff_cap]");
			}
		}
		auto const &gov = options.governor;
		if (gov.dirty_soft != 0) {
			if (options.cleaner.mode == cleaner_mode::off) {
				return fail(errc::invalid_argument,
							"the dirty budget requires the cleaner: nothing else drains between commits");
			}
			if (gov.dirty_low > gov.dirty_soft) {
				return fail(errc::invalid_argument, "dirty_low must not exceed dirty_soft");
			}
			if (gov.dirty_hard != 0 && gov.dirty_hard < gov.dirty_soft) {
				return fail(errc::invalid_argument, "dirty_hard must be at least dirty_soft");
			}
			if (gov.dirty_hard != 0 && gov.hard_timeout.count() <= 0) {
				return fail(errc::invalid_argument, "the hard watermark needs a positive hard_timeout");
			}
		} else if (gov.dirty_hard != 0 || gov.dirty_low != 0) {
			return fail(errc::invalid_argument, "the dirty budget needs a soft watermark");
		}
		if (gov.resident_soft != 0) {
#ifndef __linux__
			return fail(errc::invalid_argument, "the resident budget is Linux-only");
#else
			if (gov.resident_low > gov.resident_soft) {
				return fail(errc::invalid_argument, "resident_low must not exceed resident_soft");
			}
			if (gov.sweep_interval.count() <= 0) {
				return fail(errc::invalid_argument, "the resident budget needs a positive sweep_interval");
			}
#endif
		} else if (gov.resident_low != 0) {
			return fail(errc::invalid_argument, "the resident budget needs a soft watermark");
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
		st.cleaner = options.cleaner;
		st.governor = options.governor;
		st.read_only = read_only;
		st.store = std::move(*store);
		st.hot->poison_timeout_ns = options.poison_timeout.count();
		if (!read_only && st.governor.dirty_soft != 0) {
			// the dirty-slot count whose reach crosses the soft watermark
			st.hot->gov_soft_cross = st.governor.dirty_soft / st.rec.block_size + 1;
			st.hot->gov_hard_bytes = st.governor.dirty_hard;
			st.hot->gov_hard_timeout_ns = st.governor.hard_timeout.count();
		}

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

			// Construct the process-wide state before the region exists, so
			// a region owned by a static object constructed after this call
			// finds it alive when it is destroyed at static teardown. The
			// sweeper comes first: it must outlive the pools, because a
			// sweep task queued at teardown still locks its mutex.
			touch_resident_sweeper();
			// Starting the pools starts their threads. A host with no thread
			// budget left fails here instead of later: without a work thread
			// nothing drains the region's tasks, and close would wait on a
			// task that can never run.
			if (work_pool_size() == 0 || timer_pool_size() == 0) {
				return fail(errc::io_error, "the executor could not start a thread");
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

			if (st.cleaner.mode != cleaner_mode::off) {
				if (st.governor.dirty_soft != 0) {
					st.post_task([&st] { st.governor_cleaner_step(false); });
				} else {
					st.schedule_cleaner();
				}
			}
			if (st.governor.resident_soft != 0) {
				resident_sweeper_register(&st, st.hot, st.governor);
			}
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
				if (is_transient(state)) {
					(void) table.wait_changed(slot, state);
					continue;
				}
				// A poisoned slot is claimed like any terminal state: the
				// anonymous remap replaces the mapping wholesale and does not
				// need the protection change that failed, so the free heals
				// the slot as a side effect.
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
				// A failed MAP_FIXED mmap can leave the range unmapped, so
				// restorability is in doubt: terminal flag.
				if (prior != slot_state::poisoned) {
					st.hot->poisoned.fetch_add(1, std::memory_order_release);
				}
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
			} else if (prior == slot_state::poisoned) {
				// the handler balanced the dirty counter at poisoning time
				st.hot->poisoned.fetch_sub(1, std::memory_order_release);
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
		// Claims are held from here on, so no allocation may happen with a
		// claim in hand: the capture loop below only pushes into this space.
		captured.reserve(captured_slots);

		// Restores every captured slot the write-out did not release, on
		// every path out of this function. A slot left in syncing parks every
		// writer that touches it for as long as the region lives, so the
		// restore must also run when an allocation or a worker post throws.
		// Only this commit moves a slot out of syncing, so the state
		// identifies the unreleased ones exactly; on the paths where every
		// slot was released this is a scan that changes nothing. A write
		// capture may already be read-only, so it must be writable again
		// before dirty reappears, and re-protecting a still-writable page is
		// harmless. The governor wake covers the republished dirt: it changed
		// no counter, so without the wake a parked cleaner would sleep a full
		// interval on it.
		scope_guard const restore_captured{[&]() noexcept {
			bool restored = false;
			for (auto const &cap : captured) {
				if (table.load(cap.slot) != slot_state::syncing) {
					continue;
				}
				if (cap.empty) {
					table.publish(cap.slot, slot_state::dirty_empty);
					restored = true;
					continue;
				}
				if (::mprotect(slot_addr(cap.slot), block_size, PROT_READ | PROT_WRITE) != 0) {
					st.hot->poisoned.fetch_add(1, std::memory_order_release);
					table.publish(cap.slot, slot_state::poisoned);
					table.sub_dirty();
					st.hot->error.store(1, std::memory_order_release);
					continue;
				}
				table.publish(cap.slot, slot_state::dirty);
				restored = true;
			}
			if (restored) {
				table.wake_governor();
			}
		}};

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
					// Recovery under the mutex this commit already holds: on
					// success the slot is dirty and the re-examination
					// captures it normally. An unhealed slot keeps its recipe
					// entry and the commit proceeds without it: no store has
					// landed on the slot, so its content is unchanged since
					// the entry named it. (A slot poisoned out of dirty_empty
					// keeps its pre-free name for content that is zeros in
					// memory, the same divergence as the freeing skip below,
					// and sound for the same reason.)
					if (!st.recover_poisoned(slot) && table.load(slot) == slot_state::poisoned) {
						break;
					}
					continue;  // healed, or a concurrent free claimed it; re-examine
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
				// The captured slots stay syncing; the guard above reverses
				// any partial downgrade of this run and republishes dirty,
				// so the commit unwinds completely and a retry can succeed.
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
		// It counts worker 0, which runs below on this thread; every posted
		// worker adds itself, so a post that fails leaves no count behind
		// and the join always ends.
		auto const workers_left = std::make_shared<std::atomic<uint32_t>>(1);
		auto const count_worker_out = [workers_left] {
			if (workers_left->fetch_sub(1, std::memory_order_acq_rel) == 1) {
				word_wake_all(*workers_left);
			}
		};
		auto const join_workers = [workers_left] {
			for (;;) {
				uint32_t const left = workers_left->load(std::memory_order_acquire);
				if (left == 0) {
					return;
				}
				(void) word_wait(*workers_left, left);
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
				block_digest const prior = entry;
				if (name != entry) {
					if (auto published = st.store->publish(name, content); !published) {
						res.err = published.error();
						write_out_failed.store(1, std::memory_order_release);
						return;
					}
					entry = name;
				}
				// One MAP_FIXED call replaces the private pages with the
				// named block file; a concurrent reader either reads the old
				// identical bytes or blocks inside its fault, never an
				// unmapped window.
				if (auto mapped =
							map_block_file(slot_addr(cap.slot), block_size, st.store->block_path(entry));
					!mapped) {
					// The mapping is untouched by the failed call, so the
					// slot unwinds: the entry reverts, the restore guard
					// republishes dirty, and a retried commit rehashes the
					// same private pages. A published file stays sweepable
					// garbage.
					entry = prior;
					res.err = mapped.error();
					write_out_failed.store(1, std::memory_order_release);
					return;
				}
				if (name != prior) {
					res.deltas.push_back({prior, name});
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

		// The posted workers hold this frame by reference, so nothing may
		// leave it while one of them still runs. On an exception this stops
		// the write-out and joins them before the frame dies; the join is
		// idempotent, so the normal path below joins where it needs the
		// results. It is declared after everything the workers touch, so it
		// runs before any of that is destroyed.
		std::optional<error> post_err;
		scope_guard const join_posted_workers{[&]() noexcept {
			write_out_failed.store(1, std::memory_order_release);
			join_workers();
		}};

		for (size_t w = 1; w < worker_count; ++w) {
			workers_left->fetch_add(1, std::memory_order_relaxed);
			try {
				if (commit_post_fails(w)) {
					throw std::bad_alloc{};  // the injected failure of the seam
				}
				asio::post(work_pool(), [&run_worker, &results, w, count_worker_out] {
					run_worker(results[w]);
					count_worker_out();
				});
			} catch (...) {
				// This worker never runs, so its count must go back or the
				// join would wait for it forever. The captured slots it would
				// have written stay with the workers that do run, and the
				// commit reports the failure.
				count_worker_out();
				post_err = error{errc::io_error, ENOMEM, "post a commit write-out worker"};
				write_out_failed.store(1, std::memory_order_release);
				break;
			}
		}
		run_worker(results[0]);
		count_worker_out();
		join_workers();

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
			// The captured slots the write-out did not release are restored
			// on the way out of this function; released slots keep their
			// release. A worker's own error names the failure best.
			for (auto const &res : results) {
				if (res.err) {
					return std::unexpected{*res.err};
				}
			}
			if (post_err) {
				return std::unexpected{*post_err};
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

		size_t run_cleaner_batch(region &reg, bool override_backoff) {
			return reg.state_->clean_batch(override_backoff);
		}

		bool cleaner_disabled(region &reg) noexcept {
			return reg.state_->cleaner_off.load(std::memory_order_acquire) != 0;
		}

		uint64_t poisoned_slots(region &reg) noexcept {
			return reg.state_->hot->poisoned.load(std::memory_order_acquire);
		}

		uint64_t run_resident_sweep() {
			return resident_sweeper::instance().pass(false);
		}

	}  // namespace detail_region
#endif

}  // namespace privateer
