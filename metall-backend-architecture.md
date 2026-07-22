# Privateer metall backend: architecture (v2)

Companion to `metall-backend-assessment.md`. This document describes the target design. Version 1 (`metall-backend-architecture-v1.md`) is the reviewed baseline; version 2 integrates three additions: background write-back of cold dirty slots, a memory governor with dirty and resident budgets, and an asio-based execution model. The engine replaces both existing fault backends (sigaction, uffd) with a single-phase write barrier over file-backed private mappings, and rewrites the storage layer for durability and reclamation.

## 1. Design principles

1. Warm reads never enter userspace and are never blocked. Clean data is served by kernel demand paging from immutable block files. The page cache is the only cache; there is no library-level LRU, stash, or eviction tier. Cold-read faults are kernel-internal (major faults) and can briefly serialize behind commit map operations on the address-space lock; they never reach the signal handler.
2. The signal handler does the minimum: registry lookup, one state CAS, one mprotect, or one futex wait. No I/O, no allocation, no mutexes, no non-signal-safe libc. Every data structure it touches is lock-free-atomic and resident (mlocked). It is identical on all target platforms; there is no fault-type decoding.
3. All I/O and all policy (write-back, budgets, trimming) run on an internal executor. The handler only ever waits; it never does the work it waits for, and the work it waits for never takes application locks, so every handler wait is a bounded stall, not a deadlock risk.
4. Block files are immutable. A block file, once published under its name, never changes and is only ever opened read-only. All sharing (dedup within a store, hard links across snapshots) is safe because nothing writes through a shared file. Mutation happens only in private anonymous copies (kernel CoW) and produces new files at commit.
5. `sync(true)` is honest: it returns true only after every block the recipe references, the recipe itself, and the directory entries are on stable storage. This is what makes metall's properly-closed mark trustworthy. Crash recovery of a live store is all-or-nothing on that mark: a store that crashed while open does not reopen (metall refuses without the mark); recovery flows through snapshots plus the frontend's WAL. The engine's crash-ordering rules exist so that a committed recipe never references a missing or torn block, which is what makes snapshots and clean closes safe recovery points.
6. Deletion is plain file semantics. Snapshots are self-contained datastores whose block files are hard links. Deleting any datastore is `rm -r`; the link count frees shared space. There is no global registry and no cross-datastore garbage collector.
7. Errors propagate. No `exit()`. The engine is a library inside a database process.

## 2. Memory model

A region is one contiguous VM reservation:

```
[ segment header | slot 0 | slot 1 | ... | slot N-1 ]   (VM reservation = header + capacity)
```

- The reservation is created once with `mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)`. Its base address never changes while open; all metall offset pointers hang off it.
- The header is a private anonymous read-write mapping (metall's `segment_header`, volatile, never persisted by the engine).
- Each slot is `block_size` bytes. `block_size` is a datastore constant, set at create, a multiple of the system page size. Default 8 MiB, subject to A/B tuning (section 20). `page_size()` reports the system page size (metall validates it against its 2 MiB chunk size at create and open, so it must never report `block_size`).
- `size` (the extended size) grows in whole slots up to `capacity`. Slots beyond `size` stay `PROT_NONE`. `size()` reports the slot-rounded value; the handler gates on the same atomic variable, so there is exactly one authority.

### Slot states

Each slot has one atomic state byte (`std::atomic<uint8_t>`, statically asserted lock-free, array mlocked). Terminal states:

| State | Mapping | Protection | Meaning |
|---|---|---|---|
| `empty` | anonymous, `MAP_NORESERVE` | READ | reads zeros; recipe holds the empty sentinel |
| `clean` | block file, `MAP_PRIVATE` | READ | matches the recipe entry |
| `dirty` | same VMA, private CoW pages | READ+WRITE | modified since the last commit or clean |
| `dirty_empty` | fresh anonymous, `MAP_NORESERVE` | READ | freed since the last commit; recipe entry stale |

Transient claim states, each owned by exactly one actor until it publishes a terminal state: `materializing` (a faulting writer installing write access), `syncing` (a commit-mutex holder: the committer or the cleaner), `freeing` (a `free_region` caller). One more terminal state, `poisoned`, marks a slot whose protection change failed (section 3 step 4): every actor that encounters it reports an error instead of waiting, so a failed slot can never park a waiter forever. Slots beyond `size` have no meaningful state; the handler never consults their bytes because the `size` gate comes first.

Two ordering laws make the state byte trustworthy:

- **L1 (publish after protect)**: a terminal state is stored (release) only after the mprotect or mmap that establishes its protection has returned. `dirty` therefore implies the page is writable; `clean`, `empty`, `dirty_empty` imply the mapping is in place.
- **L2 (claim before touch)**: whoever changes a slot's protection or mapping first CASes the state to its transient (acq_rel), then issues the syscall. There is never a window where the page is read-only while the state still claims writable.
- **L3 (publish after remap)**: the special case of L1 for mapping replacement, called out because it is easy to get wrong: the terminal state after a `MAP_FIXED` remap (`clean` after commit write-out, `dirty_empty` after a free) is stored (release) only after the mmap syscall has returned.

### Budget accounting

The dirty budget counter (atomic, slot-granular upper bound: slots in `dirty` or `materializing`, times `block_size`) is incremented by the handler when it wins a `materializing` CAS and decremented by whoever moves a slot out of `dirty` or `materializing` (committer or cleaner storing `clean` or `empty`, freer storing `dirty_empty`, the handler's own error path storing `poisoned`). `dirty_empty` holds no private pages (fresh anonymous zero mapping) and does not count. Every decrease, on every one of these paths, bumps the governor futex word and wakes it, so blocked writers recheck (section 6); this wake is the single most load-bearing one in the design and is restated at each site.

### VMA budget

VMA count is bounded by the number of materialized slots, not by capacity. 100 GiB of live data at 8 MiB blocks is 12,800 VMAs; distinct block files never merge (different inodes), adjacent empty anonymous slots do. Linux `vm.max_map_count` defaults to 65,530. VMA exhaustion is a hard failure mode: `mprotect` in the handler fails with ENOMEM and the fault becomes a crash. The handler cannot refuse a write, so the budget is enforced where the engine can fail cleanly: `extend` fails with a clear error when the worst case of the requested size (every slot within `size` becomes one VMA) would cross the budget read from the sysctl at create or open, minus configurable headroom for the rest of the process. This is conservative (empty slots that are never written cost no VMA) but guarantees a granted extend can always be faulted fully. Darwin has no equivalent limit. The deployment guide documents raising the sysctl for large stores.

## 3. The write barrier

### Signals and installation

Linux delivers protection-violation write faults as SIGSEGV (`SEGV_ACCERR`); the engine registers only SIGSEGV there. Darwin delivers them as SIGBUS (`KERN_PROTECTION_FAILURE`); the engine registers SIGBUS and SIGSEGV. The handler is installed process-wide once, `SA_SIGINFO`, previous disposition saved.

Forwarding to the previous disposition follows the standard chaining protocol: a saved `SA_SIGINFO` handler is called with all three arguments; a plain handler is called with the signal number; `SIG_DFL` is forwarded by restoring the default disposition and returning, so the retried instruction re-faults into the default action (core dump); `SIG_IGN` for a synchronous fault is treated like `SIG_DFL`.

Darwin caveat: Mach exception ports take precedence over BSD signals. Under lldb or a crash-reporter that claims `EXC_BAD_ACCESS`, the barrier does not fire. This is documented as a development-time limitation; a Mach-exception-port variant is a possible later addition, not part of this design.

Sanitizers: ASan installs its own SIGSEGV and SIGBUS handlers at process init, before the engine ever opens a datastore, so the engine's later `sigaction` puts the barrier first in the chain and genuine faults still reach the sanitizer through the forwarding protocol. Sanitizer runs require `ASAN_OPTIONS=allow_user_segv_handler=1` so ASan tolerates the barrier consuming its own faults. TSan cannot see the handler's raw futex syscalls, so the barrier's acquire and release edges carry `__tsan_acquire` and `__tsan_release` annotations in TSan builds, and TSan must not own the fault-signal dispositions.

aarch64 pointer tags: the engine never sets `SA_EXPOSE_TAGBITS`, so the kernel delivers `si_addr` untagged, and the handler additionally masks the top byte before the registry search as belt and suspenders (TBI is on by default for user pointers; a tagged fault address must not miss the region lookup and turn a barrier fault into a forwarded crash). When the host build enables branch protection (BTI), the engine is compiled with the same flag so the handler entry carries its landing pad.

The handler runs on a per-thread `sigaltstack` (`SA_ONSTACK`), and the alternate stacks are mlocked like the state arrays. The same nested-fault argument that mlocks the data also covers code and stack: the engine's handler text is mlocked at install (its page range, not the whole process), because a reclaimed text or stack page touched while the fault signal is masked kills the process. The `RLIMIT_MEMLOCK` budget in the registry section includes one alternate stack per application thread that can fault.

### Fault flow

1. Increment the process-global lookup gate (lock-free), load the registry pointer (acquire), binary-search the fault address; on a hit, increment the found region's in-flight counter, then decrement the gate. The gate is held only for the lookup (nanoseconds), so it drains fast; the region counter is held for the whole handler run. Not found: decrement the gate, forward (genuine crash).
2. Found, but the region is read-only, marked closing, the fault is in the header, or the address is at or beyond `size` (acquire): decrement, forward.
3. Load the slot state (acquire) and loop:
   - `empty`, `clean`, `dirty_empty`: if the governor is enabled and the dirty budget is at or above its hard watermark, futex-wait on the governor word **with a timeout**, rechecking in a loop (also woken by closing, which then forwards); after a bounded total wait the write proceeds anyway and overshoots the budget by one block. A writer is therefore never parked on drain latency longer than the configured timeout, and the hard mark is a bounded target, not an invariant: the ceiling is `hard + writers * block_size`. Then CAS the state to `materializing` (acq_rel). Winner: add `block_size` to the dirty counter, `mprotect(slot, block_size, PROT_READ | PROT_WRITE)`, store `dirty` (release, L1), futex-wake the state word, return (the retried store succeeds). CAS loser: reload and re-loop.
   - `materializing`, `syncing`, `freeing`: futex-wait on the state word with the observed value (compare-value form closes the lost-wakeup race; EINTR and spurious wakeups re-loop), then **return and retry the faulting instruction**. Never re-run the state machine after a wait: the retry classifies itself. A read that faulted against a transient window (see remap) now succeeds against the restored `PROT_READ` mapping; a write re-faults and takes the CAS path. This keeps "every fault the handler acts on is a write" true even across transient not-present windows.
   - `dirty`: return and retry. By L1, `dirty` implies the page is writable, so this is a stale-TLB spurious fault or a benign race with a fresh transition; by L2, any protection downgrade was preceded by a visible transient, which the retry will observe and wait on. Termination is guaranteed by L1 and L2 together.
   - `poisoned`: store the error flag, decrement, forward. The slot is dead (see step 4).
4. `mprotect` failure after a won `materializing` CAS: subtract `block_size` from the dirty counter, bump and wake the governor word, store `poisoned` (release) so no waiter parks on the slot and commit capture fails it with an error instead of waiting, then store the error flag (lock-free atomic, release), decrement the in-flight counter, forward. A corrupt state value (no CAS won, no counter to balance): error flag, decrement, forward. `check_sanity()` and all later commits report failure, so `close()` withholds the mark.

The governor wait shares the safety argument of the transient waits: the drain (cleaner write-back) runs on the executor, takes no application locks, and progresses independently of the blocked writer, so the wait is bounded backpressure, not a deadlock risk. The hard watermark therefore requires the cleaner to be enabled; configuration validation enforces that.

The handler waits on Linux with the futex syscall (raw syscall, signal-safe, compare-value form, timed where the wait target is I/O-bound). Timed waits bound the **total** wait: the handler takes one `clock_gettime(CLOCK_MONOTONIC)` deadline (vDSO, async-signal-safe) and recomputes the relative timeout on every EINTR or spurious-wakeup re-loop; `FUTEX_CLOCK_REALTIME` is never used (wall-clock jumps would break the bound). On Darwin the in-handler wait is a spin loop with `nanosleep` backoff (`nanosleep` is on the async-signal-safe list) against a `mach_absolute_time` deadline; `os_sync_wait_on_address` is not documented signal-safe and is only used from non-handler contexts. The handler's `sa_mask` adds nothing beyond the delivered signal, so asynchronous signals (SIGTERM) still interrupt a waiting handler; waits re-loop on EINTR.

On Linux the barrier signal cannot be confused with I/O or memory-pressure faults: truncated-file access and strict-overcommit allocation failures arrive as SIGBUS, which the engine does not register on Linux and which therefore crashes honestly. The engine additionally validates at map time (open, extend of the mapping set, commit remap) that every block file's size is exactly `block_size`, so a short file is an open or commit error, never a runtime SIGBUS.

### Region registry

A process-global sorted array of `(start, end, region*)` published through one atomic pointer; register and unregister build a new array and swap (release), serialized against each other by a registry mutex (normal thread context only, never the handler), so concurrent closes flip the lookup-gate epoch one at a time. Retired arrays are placed on a never-freed retirement list: registrations happen at datastore open and close, so the leak is bounded to a few hundred bytes per open in the process lifetime, and the handler can dereference any array it loaded without any reclamation protocol. Region structs themselves are reclaimed only after the in-flight counter quiesces (see shutdown). All registry arrays, region structs, and state arrays are mlocked: the handler must never take a page fault of its own, because the fault signal is masked while it runs and a nested synchronous fault would kill the process.

mlock is subject to `RLIMIT_MEMLOCK`, whose common default (64 KiB) is smaller than the state array of a large store (one byte per slot of `capacity`: 1 MiB for 8 TiB at 8 MiB blocks, shared with other locked-memory consumers such as io_uring). At init the engine raises the soft limit toward the hard limit; if the requirement still exceeds the hard limit, open fails with a clear error naming the limit, because running with an unlockable state array is a correctness hazard (a reclaimed state page touched in the handler is a process kill), not a degradation. An explicit override option exists for swapless deployments where anonymous pages cannot be reclaimed anyway. The deployment guide documents the sizing next to `vm.max_map_count`.

## 4. The commit path

`commit(durable)` implements `sync(bool)` and is the write half of `snapshot`. One commit runs at a time (commit mutex). Readers run concurrently throughout.

Precondition (same as upstream metall, stated honestly): the barrier only intercepts the first write into a slot per epoch. A writer that already holds write access to a `dirty` slot and races the capture mprotect can be frozen mid-store, so a consistent cut requires the application not to write concurrently with `sync` and `snapshot`. Upstream metall turns such writers into crashes via its own mprotect trick; this engine makes first-touch writers wait and in-flight writers tear the cut exactly as upstream would. Tentris serializes its writer around checkpoints, which satisfies the precondition. A rogue write that slips between a commit and `close()` lands in private pages that `release()` discards; committed state is never corrupted by it, only the diagnostic (a crash) is lost relative to upstream.

`commit()` on a read-only region returns true immediately without touching anything: metall's `flush()` and the reference backend's destructor call `sync` without a read-only guard and rely on the backend to self-guard, and a read-only open must never mutate files held under the shared lock (phase 4 would otherwise rewrite `_recipe` even with an empty dirty set).

Phases:

1. **Capture.** Record `size` for this epoch (a concurrent `extend` may grow the region during the commit; the grown tail belongs to the next epoch and is persisted by the next commit). Scan the state array up to the captured size. For each `dirty` slot: CAS `dirty -> syncing` (L2), then `mprotect(PROT_READ)`. For each `dirty_empty` slot: CAS to `syncing`, remember it as an empty commit. Contiguous captured runs are downgraded with one `mprotect` call per run (one TLB shootdown per run instead of per slot). Correctness depends on `mprotect`'s synchronous global TLB shootdown: when it returns, no core holds a stale writable entry, so the content is frozen. Slots that a concurrent fault holds in `materializing` are waited for (futex) and re-examined. A slot a concurrent `free_region` holds in `freeing` is **skipped, not waited for**: it resolves to `dirty_empty` and belongs to the next epoch, so this epoch's recipe keeps the pre-free name for a slot that is already zeros in memory. That is sound for the same reason the sub-slot divergence is (section 10): the allocator never reads freed memory expecting content. A `poisoned` slot fails the commit with an error immediately; the store is already marked insane.
2. **Write-out and per-slot release** (parallel workers on the executor, count from the caller or `hardware_concurrency`). Per `syncing` slot, pipelined so waiters are released per slot, not per batch:
   a. Hash the frozen content (reads mix private CoW pages and page-cache pages; both are the frozen bytes).
   b. If the hash equals the committer's recipe-table entry, the change was value-identical: skip the write. Otherwise the committer updates its recipe table for this slot to the new name. The recipe table and `size` serialized in phase 4 are exactly the state captured in phase 1 plus these per-slot updates; phase 4 never re-scans live slot states, so slots re-dirtied or freed after their release in step e do not leak into this epoch's recipe.
   c. Otherwise write a new block file: `O_TMPFILE` plus `linkat` into the shard directory (mkstemp plus rename fallback; both publish atomically). If the name already exists (dedup), byte-compare against the existing file (opened read-only) and drop the new one on a match; a mismatch is a fatal hash-collision error under a weak hash, prevented by this very check.
   d. Remap the slot with a single `mmap(MAP_PRIVATE | MAP_FIXED, PROT_READ)` of the (existing or new) named block file. One syscall, never munmap-then-mmap: the kernel replaces the VMA atomically under the address-space lock, so a concurrent reader either reads the old identical bytes or blocks on the lock inside its fault, never observes an unmapped window. Bytes are identical by construction, so readers observe no change.
   e. Store `clean` (release, L3: after the mmap returned), decrement the dirty counter, futex-wake the state word. Empty commits store `empty` and **futex-wake the state word too** (their anonymous mapping is already in place from `free_region`, and no counter is decremented because `dirty_empty` never counted, but a waiter parked on the `syncing` word, such as an overlapping free, must be released; state-word waits are untimed, so every transient-to-terminal transition wakes).
   A writer that faulted on this slot therefore waits only for steps a to e of its own slot, not for the whole commit, bounding the stall it can impose while holding application locks.
3. **Durability barrier** (durable commits only). The engine keeps a **durable-name set**: a block name enters it only after its file was fdatasynced **and** the shard directory holding its entry was fsynced; both halves are required, because `fdatasync` alone leaves the name losable. Durability is a property of the name, not of a slot: a dedup reference from any slot to a name outside the set inherits the fsync obligation, so a slot can never certify durability another slot's non-durable write does not have. Publications outside a durable barrier (non-durable commits, the cleaner's non-durable mode) produce names that are simply not yet in the set. Entries leave the set only when their file is unlinked (phase 5). At open, the set is initialized to every name the verified recipe references: a verified mark certifies exactly that those blocks are durable. The barrier: `fdatasync` every referenced block file whose name is not in the set, `fsync` the affected shard directories, add the names. This covers blocks written in this commit, blocks inherited from earlier non-durable commits or cleaning, and the case of an empty dirty set. Empty sentinel entries carry no obligation (self-describing in the recipe, no file). Without the inherited-name rule, a `flush(false)` followed by `close()` would write a durable mark over non-durable data; this rule is what makes the mark honest.
4. **Recipe commit.** Serialize the recipe, write to a temp file, `fdatasync` (durable), `rename` over `_recipe`, `fsync` the segment directory (durable). The recipe serializes the full in-memory table, including entries the cleaner updated for slots not captured this epoch; that is the mechanism by which a pre-cleaned slot's old name leaves the on-disk recipe. This is the atomic commit point; it strictly follows phase 3, so a committed recipe only ever references durably named, durably written blocks.
5. **Reclaim** (durable commits only). Unlink block files referenced neither by the just-committed recipe nor by any slot's live mapping. Commit-mutex holders own all of this bookkeeping: the name refcount map (names referenced by the recipe table, multiple slots may share a name) and the unlink candidate list. Candidates are derived only under the commit mutex, from the recipe table, when a commit or a cleaning pass replaces or empties a slot's entry; `free_region` never touches this state, it only flips slot states, and the freed slot's old name stays in the recipe table (and therefore stays referenced and safe) until the next commit-mutex holder captures the `dirty_empty` slot and retires the name itself. A slot can retire two names within one epoch (a cleaner-replaced name and the name emptied by a later free); the candidate list is name-keyed and holds both. Every unlinked name is also removed from the durable-name set: a later re-publication of the same content creates a fresh file and directory entry that must earn durability again, and a stale set entry would let the barrier skip that fsync. (While a file still exists, dedup re-publication reuses it and the set entry stays valid, so removal exactly at unlink time is sufficient.) Hard links from snapshots keep shared inodes alive; unlinking here only drops this datastore's name. `fsync` affected shard directories. Reclaim is the one phase whose errors are non-fatal: a failed unlink or reclaim-side directory fsync is logged and the name stays on the candidate list; the commit is already durable and only garbage remains. These swallowed errors, plus mkstemp leftovers, are what actually feed the open-time sweep. A store that crashes mid-commit never reopens at all (the mark is gone), so crash garbage dies with the store's `rm -r`, not through the sweep.

`commit(durable=false)` runs phases 1, 2, and 4 without any fsync: page-cache atomicity, no durability promise, matching metall's `sync(false)`. It never unlinks (a lost rename must be able to resurface the old recipe with all its blocks intact); the names it publishes are outside the durable-name set until a durable barrier adds them.

Fsync flavor: `fdatasync` on Linux; on Darwin `fcntl(F_FULLFSYNC)` for files and directories alike when the durability flag is on (plain `fsync` does not flush the drive cache on APFS, and directory durability without it is not established).

File-descriptor discipline: block-file fds are closed immediately after `mmap` (the mapping holds the inode; the fd is never needed again for the map), so the steady-state fd count does not grow with mapped slots. Transient fds (write-out, dedup byte-compare, the durability barrier's re-opens, directory fds) are bounded by the worker count; the timer pool additionally holds three persistent reactor fds (epoll, eventfd, timerfd, section 7). The deployment guide notes raising `RLIMIT_NOFILE` on macOS, whose 256 default is reachable by the commit fanout plus the host process.

## 5. Background write-back

A cleaner task runs commit phases 1 and 2 for individual cold dirty slots ahead of any commit, under the commit mutex per batch: CAS `dirty -> syncing`, freeze, hash, write the block file, remap, publish `clean`, decrement the dirty counter and bump and wake the governor word (this wake is what releases a writer blocked at the hard mark), and update the recipe table. The on-disk recipe is untouched, so crash consistency is unchanged; prematurely written files that end up unreferenced are ordinary reclaim and sweep garbage.

Correctness comes from the barrier itself: any write after a slot was cleaned re-dirties it, so the next real commit recaptures it with its final content. A cleaned slot that never re-dirties is by definition unchanged. The recipe a commit renames therefore always reflects each block's state at commit time, and the consistency precondition of section 4 is not weakened. A background freeze that races an active writer produces a superseded block version, never a referenced torn one, because the writer's next store re-dirties the slot.

Victim selection: hash the blocks least likely to be written again. Use recency is not directly observable (reads never fault, and repeat writes to a dirty slot never fault), so the policy uses the signals the barrier provides: candidates are ordered by first-dirty time, and a slot that re-dirties soon after being cleaned is backed off exponentially, so hot slots stop being cleaned. The exponential is capped, and at the hard dirty watermark the cleaner ignores backoff entirely and takes the coldest dirty slot unconditionally, so at least one drain per cycle is guaranteed while any dirty slot exists; without this override, a writer blocked at the hard mark would wait out backoff timers instead of cleaner throughput. Page-table access-bit sampling is a possible Linux-only refinement, not part of the design.

In eager-durable mode the cleaner additionally performs the full durable-name contract of section 4 phase 3 for what it writes: `fdatasync` the block file **and** `fsync` its shard directory (batched per cleaning batch), then add the name to the durable-name set. Both halves are mandatory; a name entered after only the file fsync would let a later durable commit skip the directory fsync, and a crash could then lose the name of a block the committed recipe references. With eager-durable cleaning, a later `sync(true)` pays only for recently dirtied slots plus the recipe rename. Costs, all bounded: wasted block versions for mispredicted slots, one extra fault plus a per-slot wait for a writer touching a slot mid-clean, and TLB shootdowns spread over time instead of clustered at commit.

## 6. The memory governor

Two budgets with different strengths, matching what each can actually enforce:

- **Dirty budget**: exact, portable, enforceable. Counted as in section 2. Soft watermark: the handler bumps and wakes the governor word when its increment crosses the soft mark (one extra atomic plus a futex wake, signal-safe), and the cleaner waits on that word with the sweep interval as its timeout fallback, so activation is edge-triggered by writer activity, not only timer-driven; a blocked writer spuriously woken by the crossing wake just rechecks and re-waits (compare-value form), and the cleaner's wake handling is by-value in the same way: woken by any change, it rescans the budget and goes back to waiting when nothing sits above the soft mark. The cleaner writes back cold-first until the low target. Hard watermark: a write fault that would take the total above the hard mark waits in the handler on the governor word with a timeout (section 3); on timeout the write proceeds and overshoots by one block. Bounded backpressure: the wait is released by any counter decrease (cleaner, committer, freer, all of which wake the word), never lasts longer than the timeout, and drains independently of the blocked writer because dirty bytes are monotonically drainable and readers cannot re-inflate them. The hard watermark requires the cleaner; configuration validation enforces that.
- **Resident budget**: advisory, soft watermark and low target only, **no hard watermark**. Residency is reader-inflatable (a scan re-materializes clean pages through kernel minor faults the engine can neither observe nor block), so blocking any mutator on it has no forward-progress guarantee, and failing `extend` on it would surface as nondeterministic `bad_alloc` driven by reader behavior. The sweep converges toward the target; the robust bound for total residency is the kernel's own reclaim under cgroup or memcg limits, which evicts clean file pages for free. The governor exists for self-imposed predictability, not as a replacement.

Resident mechanics, Linux only: accounting reads `/proc/self/smaps_rollup` (Pss and Private_Dirty) for the absolute number, one coarse read per sweep. That number is process-global (it includes non-engine memory and every open region), so the resident budget is a process-level target by construction: one resident sweep runs per process, serving all regions, with the trim effort split across regions by their materialized size. Per-region resident targets would double-trim against the shared number and are not offered; per-slot `mincore` is used only for victim selection, because mincore reports page-cache residency rather than this process's mapped footprint (it over-counts pages cached by other openers and under-counts nothing). Trimming of `clean` and `empty` slots uses `MADV_PAGEOUT` (kernel 5.4+, own process, no capability). PAGEOUT is non-destructive under every race: if the state read is stale and the slot just turned `dirty`, PAGEOUT pushes fresh private pages to swap and they swap back in on access, wasted I/O but never a lost write, so no claim state is needed. `MADV_DONTNEED` and `posix_fadvise` are not used at all: DONTNEED on a private file mapping is destructive under the same race (it would revert just-written pages to stale file content, invariant (d) in section 8), and fadvise skips still-mapped pages anyway. One PAGEOUT caveat is probed, not assumed: depending on kernel version, shared folios (mapcount above one: deduped blocks mapped at several slots, blocks also mapped by read-only openers or snapshot readers) are either reclaimed globally (evicting them under other mappings, a latency effect on those readers) or skipped entirely (making dedup-heavy stores partially untrimmable); the deployment guide records the measured behavior per target kernel.

On Darwin there is no reliable userspace trim: `posix_fadvise` does not exist, `MADV_DONTNEED` is advisory and non-reclaiming there, and the destructive `MADV_FREE_REUSABLE` would reintroduce the lost-write race. The resident budget is therefore Linux-only; Darwin relies on the kernel's memory-pressure reclaim of clean file pages. The dirty budget works on Darwin unchanged (it is pure bookkeeping plus the handler wait).

Recency for reclaim ordering uses the signals available: first-dirty time with re-dirty backoff for dirty slots, least recently cleaned or materialized for clean slots, optionally sharpened on Linux by accessed-bit sampling (page_idle) folded into the same periodic sweep. Darwin's `mincore` vec bytes carry multiple flags; code masks `MINCORE_INCORE` explicitly.

The governor applies to read-write regions only; read-only opens have no dirty pages and rely on kernel reclaim.

## 7. Execution model

The engine owns one internal executor per process (standalone asio), shared by all regions. Every task except the resident sweep is owned by exactly one region, and that region's shutdown cancels and joins all of its tasks, including any posted asynchronous commit, before its memory is freed. The resident sweep is the one process-level task (section 6); a region's shutdown removes the region from the sweep's set and waits until the sweep no longer touches it. The executor itself stops at library teardown; an abrupt process exit relies on OS reclamation, which is safe because nothing the executor does can corrupt committed state (only a recipe rename commits, and that is a single atomic step inside a task).

Concretely the executor is two `asio::thread_pool`s, split for **starvation isolation**: a timer expiry must never sit in the same queue that an fdatasync storm saturates, and asio cannot reserve a worker for timers inside one pool. The **work pool** (sized from the caller or `hardware_concurrency`, deliberately oversubscribable because its tasks block in `write` and `fdatasync`) runs the commit workers, the cleaner, and posted commits; it is post-only and therefore genuinely reactor-free (no epoll, eventfd, or timerfd). The **timer pool** (one thread) owns the `steady_timer`s of the governor sweep and the cleaner's interval fallback; constructing a timer instantiates asio's epoll reactor on that pool (an epoll fd, an eventfd, and a timerfd against `RLIMIT_NOFILE`, the same cost a one-thread `io_context` would pay), which is accepted and counted in the fd budget; the timer thread services the reactor and never blocks on I/O. Commit fan-out is a bounded parallel-for: `post` one task per **worker**, not per slot, and each worker pulls the next captured slot by atomic `fetch_add` until the set is exhausted, then decrements the join counter. This keeps handler allocations and queue hops at O(workers) instead of O(slots) while preserving the per-slot pipelined release (a worker publishes `clean` and wakes waiters slot by slot as it goes). The per-slot stages are blocking syscalls and hashing, so coroutines, channels, and strands would add machinery without creating any overlap, and the commit mutex stays a plain mutex because it is held across blocking syscalls, which a strand cannot model. The cleaner's futex-word edge trigger stays a raw futex wait, since asio has no futex primitive.

The public API stays synchronous: metall's contract is blocking, and `sync(true)` must not return before durability. `sync(false)` may optionally be posted to the executor and return early, which metall's contract explicitly permits (it is void and promises nothing); a posted commit that fails records the error flag like every other executor task, so `check_sanity()` and the next `sync(true)` report it and `close()` withholds the mark. The default remains synchronous because the blocking form is the memory backpressure of last resort.

Nothing the executor runs is reachable from the signal handler, and the handler never posts work; it communicates only through slot states and the governor word. Executor tasks take no application locks, which is what makes every handler wait bounded. Every task body is wrapped in a catch-all that records the error flag and returns: an exception escaping a pool task terminates the process, and section 13's failure contract is the flag, not exceptions. Cancellation reflects what asio actually provides: a queued `post` cannot be cancelled (cancellation slots apply to asynchronous operations only, and `thread_pool::stop` is pool-wide, and both pools are shared across all regions, so stopping either stops every region), so region shutdown relies on the closing-flag no-op plus the per-region outstanding-task counter it joins on (this in turn requires that library teardown never stops the pools while any region is still draining, which the teardown-after-all-regions-close ordering guarantees); `steady_timer::cancel` does complete pending waits with `operation_aborted` and is how the region's timers are stopped; `stop` and `join` on the pools happen only at library teardown.

The engine is **fork-unsafe**, stated openly: fork does not inherit memory locks (the child's state arrays are present but unlocked, re-arming the nested-fault kill), duplicates no executor threads (nothing drains budgets or runs commits in the child), and copies any held mutex in its locked state. A child must `execve` immediately or must never touch an open datastore or the API; `execve` itself is clean (fresh address space, dispositions reset). A `pthread_atfork` child handler sets the error flag on every open region so misuse fails loudly instead of hanging or corrupting. Executor threads keep SIGSEGV and SIGBUS unblocked (a genuine fault on an executor thread must reach the barrier's chain), and no `asio::signal_set` is ever created for the barrier signals. Batched `fdatasync` via io_uring (liburing directly) for the durability barrier of large commits is a measured option on Linux, not a baseline commitment; it competes for the same `RLIMIT_MEMLOCK` budget as the state arrays and is disabled by seccomp in some sandboxes, so the portable blocking-pool barrier stays the shippable default. asio's own file classes are not used at all: they require io_uring on Linux, do not exist on Darwin, cannot model the `O_TMPFILE` plus `linkat` publication, and expose no asynchronous fsync (only blocking `sync_data`), so they would add nothing anywhere the engine touches a file.

## 8. On-disk format

One datastore, as seen inside metall's layout (`<root>/mds/`):

```
mds/
  segment/
    _recipe                 committed recipe (atomic rename target)
    blocks/
      <shard 00..ff>/
        <block name>        immutable block file, exactly block_size bytes
  management/ ...           metall's, not ours
  properly_closed_mark      metall's
mds_lock                    metall's
```

- **Block name**: lowercase hex of the content hash (algorithm per A/B, section 20; raw bytes in the recipe). Shard = first 8 bits, 256 subdirectories, created eagerly at datastore creation and fsynced there once (directory skeleton durable up front; commits never create directories).
- **Recipe** (binary, little-endian): header (magic, format version, `block_size`, capacity, size, slot count, hash algorithm id, header checksum), one fixed-width entry per slot within `size` (raw hash bytes or the all-zero empty sentinel), a trailing xxh3 checksum over the entries. A torn or corrupt recipe fails open with a clear error, and so does an intact recipe whose header the build cannot serve: a format version newer than the build, or a hash algorithm id the binary was not compiled with. `block_size` and the hash algorithm are adopted from the header at open (metall's open surface carries no such options). At 32-byte entries, 1 TiB extended at 8 MiB blocks is a 4 MiB recipe; rewriting it per commit is cheap.
- `_blocks_path` (external block stores) and the stash tier are removed. `_granularity`, `_capacity`, `_metadata` are folded into the recipe.
- **Invariants**: (a) a committed recipe references only block files durably named before the recipe rename (durable commits) or at least atomically published (non-durable); (b) block files never change after publication and are only opened read-only everywhere, including the dedup byte-compare; (c) `blocks/` may contain unreferenced files (crash leftovers), never a missing referenced file, except after a crash mid `commit(false)`, which metall already reports as inconsistent via the missing mark; (d) `MADV_DONTNEED` is never issued on a slot in `dirty` state (it reverts private pages to stale file content).

## 9. Snapshot and copy

`snapshot(staging_base, clone, threads)` (the adapter derives the segment subtree as `storage::get_path(staging_base, "segment")`, exactly like metall's own backend, never a hardcoded path):

Steps 1 and 2 run under the commit mutex, held across both: a durable commit between them could reclaim a block the just-committed recipe still references, and the link pass would hit ENOENT.

1. `commit(durable=true)` (the mutex is already held; the commit runs inline).
2. Build the staged segment: shard skeleton, hard-link every block file referenced by the step 1 recipe (`link(2)`), write the recipe copy and `fdatasync` it (`F_FULLFSYNC` on Darwin). Metall's publication step fsyncs directories only, so the engine owns the durability of the staged recipe's content; the hard-linked blocks' contents are durable already (phase 3), and their new directory entries are covered by metall's directory-tree fsync.
3. Return. Metall fsyncs the staged tree and publishes the datastore with one atomic rename.

Per-file fallback when `link` fails: `copy_file_range`, `clonefile` on Darwin, plain copy (EXDEV across devices; EMLINK when a block's link count is exhausted, ext4 caps around 65,000, so only reachable with tens of thousands of retained checkpoints; the fallback then unshares that block, trading space for correctness).

Cost: O(referenced blocks) metadata operations plus one recipe write; no block data copied on the same filesystem. The result is a fully self-contained metall datastore. Writes after a snapshot fault `clean -> dirty` in memory and produce new files at the next commit; the snapshot's links keep old files alive.

`static copy(src_base, dst_staging_base, clone, threads)`: same hard-link publication, reading the source recipe from disk. Metall holds a shared lock on the source (no writer), and `link` only bumps inode link counts, so the source is never mutated.

## 10. Deletion and reclamation

- **Datastore and snapshot deletion**: metall's `storage::remove` deletes the tree; link counts release shared space. Frequent checkpoint retention cycles are `rm -r` cheap. Nothing else to do.
- **`free_region(offset, nbytes)`** (page-aligned, best-effort, called concurrently from the GC thread):
  - Fully covered slots, per slot: CAS `{empty, clean, dirty, dirty_empty} -> freeing` (loop with futex-wait while the state is transient, exactly like the handler; a `poisoned` slot ends the call with an error instead, per the section 2 rule), remap to a fresh anonymous `MAP_NORESERVE` `PROT_READ` mapping with one `mmap(MAP_FIXED)` (the same mapping kind as `empty`, so the state table holds on every path), store `dirty_empty` (release, L3), then, if the prior state was `dirty`, decrement the dirty counter and bump and wake the governor word, and wake the slot-state waiters. The claim-before-remap order is what prevents a lost write: a reallocating writer that races the free either faults before the claim (its write lands in the old pages, which the free then discards deliberately, the range was freed) or faults after `dirty_empty` is published and takes the materializing path into the fresh zero pages. No write can land during the remap itself because any first-touch waits on `freeing` and in-flight writers cannot exist for a freed range (the allocator freed it). Reads observe zeros afterwards, like upstream's hole punch. The old block file becomes reclaimable at the next durable commit, derived under the commit mutex (section 4 phase 5).
  - Partially covered slots: no action. `madvise(MADV_DONTNEED)` is forbidden here (on a private file mapping it resurrects stale file content under live CoW data), hole-punching the block file is forbidden by immutability (snapshots share the inode). Consequence, stated openly: sub-slot frees reclaim no disk space and freed sub-ranges keep their old bytes in memory, where upstream metall would punch and zero them at page granularity. Metall's allocator never reads freed memory expecting zeros (it tracks free space in its own bitsets), so this is a space and determinism divergence, not a correctness one. The block-size A/B (section 20) weighs 2 MiB blocks, where every whole-chunk deallocation of metall's large-object path maps to a full slot, against fault and VMA costs.
  - Shutdown interlock: `free_region` participates in quiescence like the handler does. It increments the region's free in-flight counter, then re-reads the closing flag; if closing became set, it decrements and returns false. Both sides of this handshake are seq_cst (the increment and the closing re-read on the free side, the closing store and the counter read on the close side): with weaker orderings the two stores could each miss the other's flag (the store-buffer case) and free would proceed into teardown. Shutdown waits this counter to zero (section 12 step 5) before unmapping.
  - If the region is closing or read-only: return false without touching anything.
- **Open-time sweep** (read-write open only, after metall verified the mark): read the recipe, unlink every file in `blocks/` not referenced by it, including temp leftovers. Safe because a verified mark implies the on-disk recipe is exactly the last durable one (close ends with a durable commit), so nothing the sweep removes can be needed by any resurfacing state. Read-only opens never mutate anything.

## 11. Growth

`extend(request_size)`:

1. Round up to whole slots; fail cleanly if beyond `capacity`, beyond the VMA budget (section 2), or read-only. The governor never gates `extend`: its budgets are either drainable without failing allocations (dirty, handled at the fault) or advisory (resident), and an `extend` failure surfaces as `bad_alloc` in the application, which must never depend on reader-inflatable state.
2. Under the region mutex: `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, PROT_READ, MAP_FIXED)` over the new slot range (one VMA), store `empty` for each new slot (release), then advance `size` (release). The handler loads `size` (acquire) before it loads any state, so a slot is only ever examined after its mapping and state are visible. The allocator never touches memory beyond what `extend` returned (metall extends before it allocates from the new range).

No file activity: an extend that is never written costs nothing. `size` is persisted in the recipe at the next commit. A store that comes back from a durable state whose recipe predates a later extend (a snapshot, or a clean close whose final commit captured the smaller size) simply opens at the recorded size, and metall re-extends on demand; a store that crashed while open does not reopen at all (principle 5).

## 12. Concurrency model

Actors, matching downstream reality:

| Actor | Operations | Blocking behavior |
|---|---|---|
| readers (many) | read mapped memory | warm reads never blocked; cold faults are kernel-internal and can briefly serialize behind commit map calls |
| writer (one, app-serialized) | write mapped memory | one barrier fault per slot per epoch; waits at most for one slot's write-out (commit or clean), one remap (free), or the governor drain at the hard mark |
| GC thread | `free_region` | waits per slot on transients |
| control thread | `sync`, `snapshot`, `extend`, `close` | commit mutex serializes commits and snapshots; region mutex serializes extend and close bookkeeping |
| cleaner (executor task) | background write-back of cold dirty slots | takes the commit mutex per batch; a slot-level committer, nothing more |
| resident sweep (one process-level executor task) | smaps_rollup and mincore accounting, reclaim hints on clean slots of every open region | no slot state changes except through the cleaner; regions leave its set at shutdown |

Synchronization inventory and required orderings:

| Variable | Written by | Discipline |
|---|---|---|
| slot state bytes | handler, commit-mutex holders, free, extend | CAS acq_rel; terminal stores release after the protecting syscall (L1, L3); all loads acquire; statically asserted lock-free |
| `size` | extend | store release after mappings and states; loads acquire |
| registry pointer | register, unregister | store release, load acquire; arrays never freed |
| error flag | handler, executor tasks | store release, loads acquire, lock-free |
| handler in-flight counter, free in-flight counter, lookup-gate epoch pair | handler and `free_region` entry and exit | lock-free; close flips the gate epoch and waits all three to drain |
| resident sweep region set | sweep and region shutdown | region shutdown removes itself and waits until the sweep no longer touches it |
| dirty counter, governor word | handler, commit-mutex holders, free | lock-free; every decrease, and the handler's soft-mark-crossing increase, wakes the governor word |
| refcount map, unlink candidates, durable-name set, recipe table | commit-mutex holders only (committer or cleaner) | no other reader or writer, ever; `free_region` communicates only through slot states |

The handler reads only the registry, `size`, the state bytes, the governor word, and the in-flight and error atomics. mprotect and mmap on distinct slots serialize on the kernel address-space lock; that is a latency coupling, not a correctness one.

### Shutdown

`release()` (called by metall's close after `sync(true)`, and by the failure paths):

1. Store the closing flag (seq_cst: the `free_region` handshake in section 10 needs the StoreLoad edge, a release store would permit the store-buffer race) and wake the governor word and all slot-state waiters. New faults on the region forward as crashes (the app must have quiesced readers and writers; metall guarantees no API calls after close begins); a woken governor waiter re-checks, sees closing, and forwards; `free_region` and `commit` return false. A writer still parked at the governor hard mark when close begins is by definition not quiesced: it is woken and crashes with a forwarded fault, which is the documented consequence of the contract violation, not a defect. Shutdown must not race a hard-watermark stall.
2. Join every executor task this region owns: the cleaner and any posted asynchronous commit. Started tasks check the closing flag at batch boundaries and exit; still-queued tasks cannot be removed from the executor's queue (asio cannot cancel a plain post), so they run as no-ops whose first action is reading the closing flag, and the per-region outstanding-task counter that the join waits on covers queued and started tasks alike. Cancel the region's timers (`steady_timer::cancel` completes their waits with `operation_aborted`). Remove the region from the process-level resident sweep's set and wait until the sweep no longer touches it.
3. Take the commit and region mutexes (waits out a straggling commit).
4. Unregister the region from the registry (swap; the retired array is kept, never freed).
5. Drain the lookup gate, then wait until this region's handler in-flight counter and free in-flight counter both reach zero (every handler and every `free_region` call that took a reference has left). The gate is an epoch pair, not a single counter: handlers increment the current epoch's counter for the duration of the lookup; unregister flips the epoch and waits only for the old epoch's counter to drain. This bounds the wait to the handlers that could have loaded the pre-swap array, instead of requiring the instantaneous global zero that a fault storm on another region could indefinitely postpone.
6. Unmap the reservation, close fds, then free the region struct and state array.

This closes the use-after-free windows between a late fault, a late `free_region`, or a straggling executor task and teardown.

## 13. Error handling

- Public API returns `bool` where metall's contract wants bool, with a queryable `last_error()` (code plus message) on the region; internal functions return a result type. No exceptions cross the adapter boundary; no `exit()` anywhere.
- The handler's only failure action is the error flag plus forwarding (and, on the mprotect path, balancing the dirty counter and poisoning the slot, section 3 step 4). Executor tasks (cleaner, governor, posted asynchronous commits) record failures in the same flag and stop; a failed cleaner degrades to commit-time write-back, never to data loss. Every later `sync` and `release` return false on the flag, which is what actually withholds the mark (metall's close checks those returns; its `check_sanity` is assert-only and advisory). Fail-safe direction is always "not consistent".
- Out-of-memory honesty: private dirty pages are committed anonymous memory, and the CoW copy happens on the retried store as a kernel-internal minor fault that never re-enters the barrier (it is not a protection fault). A CoW allocation failure is therefore handled entirely by the kernel (OOM killer, or the refused charge under strict overcommit), outside the engine, and can never be mislabeled as a barrier fault. The handler's own `mprotect` to read-write keeps `MAP_NORESERVE`, so it does not fail on overcommit accounting, only on the VMA budget, which is handled (poisoned). The dirty budget's hard watermark keeps the exposure bounded before the kernel is ever involved.
- Logging goes through the metall logger interface: metall's extern-C logger hook is the swap point, so whatever implementation the host application installs there receives the engine's logs too. Privateer ships a simple default implementation for standalone use. There is no separate logging dependency.

## 14. Platform matrix

| Concern | Linux x86_64 / aarch64 | Darwin ARM64 |
|---|---|---|
| barrier signal | SIGSEGV (`SEGV_ACCERR`) | SIGBUS (`KERN_PROTECTION_FAILURE`), SIGSEGV also registered |
| fault decode | none (single-phase) | none |
| in-handler wait | futex, compare-value, timed | spin with nanosleep backoff |
| temp files | `O_TMPFILE` + `linkat`, mkstemp fallback | mkstemp |
| data barrier | `fdatasync` | `fcntl(F_FULLFSYNC)`, files and directories |
| remap | one `mmap(MAP_FIXED)` call | one `mmap(MAP_FIXED)` call |
| hard links | all POSIX filesystems | APFS |
| VMA limit | `vm.max_map_count` budgeted | none |
| resident trim | `MADV_PAGEOUT` (5.4+; shared-folio behavior probed) | none; kernel pressure reclaim only |
| residency accounting | `smaps_rollup` for the budget, `mincore` for victims | not applicable (no resident budget) |
| mlock limit | `RLIMIT_MEMLOCK` raised or open refused | same |
| fd discipline | close after mmap; transient fds bounded by workers; three persistent timer-pool reactor fds | same, plus `RLIMIT_NOFILE` raised (256 default) |
| access-bit sampling (optional) | page_idle | none |

Platform assumptions that ship with runtime or CI probes rather than faith: Darwin `MAP_FIXED` replacement atomicity against concurrent readers (stress probe); Darwin directory-entry durability under `F_FULLFSYNC` (crash probe); Linux mprotect downgrade shootdown synchrony (two-thread probe, validates the phase 1 freeze); `MADV_PAGEOUT` behavior on shared folios (double-mapping probe: map one file twice, PAGEOUT one range, compare residency, run on each target kernel); `mincore` counting cache rather than RSS (fault, DONTNEED an unrelated test mapping, re-mincore); aarch64 tagged-pointer faults (fault through a TBI-tagged pointer, assert the barrier still classifies it); overlayfs directory-fsync durability (crash probe on an overlay upperdir); barrier functionality under a debugger on Darwin (documented limitation, Mach ports win over signals).

Filesystem rules: durable mode requires the datastore on a real filesystem. Overlayfs upperdirs (the container default) have historically weak directory-fsync durability, so a store there is supported only where the overlayfs crash probe passes on the target kernel; `O_TMPFILE` may silently fall back to mkstemp there as well (a perf note, not a correctness one). On tmpfs everything works and `sync(true)` honestly returns true, but the medium itself does not survive reboot; the mark certifies the recipe-block ordering, not the medium. All page-size-dependent buffers (`mincore` vecs and friends) are sized from the runtime page size, never a constant: targets span 4 KiB (Linux default), 16 KiB (Apple Silicon), and 64 KiB (some aarch64 kernels), and the design is page-size-clean because `block_size` must be a page multiple and metall's 2 MiB chunk is a multiple of all three. Note on `O_TMPFILE`: the unprivileged `linkat` idiom goes through `/proc/self/fd`, so procfs must be mounted; the mkstemp fallback triggers on `O_TMPFILE` open failure (EOPNOTSUPP on NFS and others) and on `linkat` failure alike. No userfaultfd, no Mach exception ports, no `/proc` parsing in the core beyond `smaps_rollup` for the optional resident budget (page_idle sampling is an optional Linux refinement), no mremap.

## 15. Metall adapter

New `metall/ext/privateer.hpp` in the metall fork (branch `feature/durable-file-handling`), implementing the current contract:

| metall calls | engine call |
|---|---|
| `create(base, capacity)` | `region::create(storage::get_path(base, "segment"), capacity)`; reserves VM, maps header, extends to one slot, fsyncs the shard skeleton |
| `open(base, capacity, ro)` | `region::open` / `open_read_only`: validate recipe checksum and every block file size, map blocks `PROT_READ`; read-write also registers the barrier and runs the sweep |
| `extend(n)` | `region::extend(n)` |
| `sync(flag)` | `region::commit(flag)` |
| `free_region(off, n)` | `region::free(off, n)` |
| `snapshot(tmp_base, clone, t)` | `region::snapshot_to(storage::get_path(tmp_base, "segment"), t)` |
| `static copy(src, dst, clone, t)` | `region::copy(get_path(src, "segment"), get_path(dst, "segment"), t)` |
| `release()` | `region::close()` (no implicit sync; metall's close calls `sync(true)` first; returns false if the error flag is set) |
| `get_segment`, `get_segment_header`, `size`, `read_only`, `is_open`, `check_sanity` | direct queries |
| `page_size()` | system page size, valid from default construction |

The `storage` template parameter stays metall's default. The adapter never relies on the segment_storage destructor for cleanup (metall's destructor semantics are quirky there); metall's `close()` drives the sequence. Read-only opens map blocks without registering the region: a stray write is an immediate honest crash, and shared files are never mutated under the shared flock.

## 16. Performance against the default backend

| Path | metall default (MAP_SHARED) | this engine |
|---|---|---|
| warm read | native | native (same page cache) |
| cold read | kernel major fault, file I/O | identical |
| warm write | native | native |
| first write to a block per epoch | native (first-touch minor fault) | signal + mprotect, about 1 to 3 us, once per `block_size` dirtied |
| `sync(true)` | msync whole segment in place | hash plus write of dirty blocks to new files, fsync, recipe; skips value-identical, already-durable, and pre-cleaned blocks |
| snapshot | full copy, or reflink where the filesystem has it | O(dirty since last commit or clean) plus O(blocks) hard links, on any filesystem |
| free, whole chunks | hole punch, immediate | remap to zeros now, unlink at next durable commit |
| free, sub-chunk | hole punch, immediate | none (documented divergence, section 10) |
| dirty pages under memory pressure | kernel writes back to segment files | cleaner writes back to the block store; without it, swap only |
| crash safety of `consistent()` | honest since the durability rework | honest, same rules |

Expected outcome for Tentris: reads identical; bulk-load writes near-identical (a 10 GiB load at 8 MiB blocks pays about 1,280 barrier faults, milliseconds in total), with the cleaner keeping peak memory bounded during loads; frequent checkpoints are where this engine wins, paying O(dirty) instead of O(datastore) on every filesystem, further reduced by eager-durable cleaning. Memory overhead: dirty private pages exist alongside stale page-cache pages between fault and write-back, bounded by the dirty budget.

Risk ledger, each with a measurement in the plan: TLB shootdown IPIs against many readers during write-heavy phases and commit capture; VMA count at small block sizes; hash throughput on aarch64; commit latency tail from the per-slot pipeline under a concurrent writer; cleaner write amplification on mispredicted slots; `MADV_PAGEOUT` re-fault latency for readers after trimming; writer stall time at the governor's hard mark; sub-chunk free divergence on long-running stores; recipe rewrite size for multi-TiB stores.

## 17. Configuration

Datastore constants (recipe header, set at create): `block_size`, hash algorithm. Runtime knobs (API, not silently via environment): commit worker count, VMA budget headroom, Darwin `F_FULLFSYNC` policy, cleaner mode (off, non-durable, eager-durable) and governor on or off, dirty budget (soft watermark, low target, hard watermark with its wait timeout; the hard watermark requires the cleaner), resident budget (soft watermark and low target, Linux only), cleaner backoff parameters and cap, governor sweep interval, the unlocked-state-array override for swapless deployments. A mismatch between recipe header and requested options fails open with an error.

## 18. Implementation constraints

C++23 against libstdc++ on every platform, never libc++. Linux uses the tentris dev container toolchain: gcc 15 and clang 21, clang built against libstdc++. Darwin uses Homebrew gcc 15 (AppleClang is not used; it would mean libc++). The compatibility floor is gcc 14.3. When the host build enables aarch64 branch protection (BTI or PAC), the engine is built with the same flags, so the signal-handler entry carries its landing pad. Dependencies via conan 2: standalone asio (executor; 1.38.x on conan-center; built with `ASIO_STANDALONE`, `ASIO_SEPARATE_COMPILATION` (one implementation TU, declarations elsewhere), and `ASIO_NO_DEPRECATED`; granular includes only (`thread_pool`, `post`, `steady_timer`), never `<asio.hpp>`; the io_uring defines stay off), Boost headers for `boost::unordered_flat_map` (refcount map, name and slot bookkeeping; header-only, no compiled Boost libraries), the hash candidates (blake3, xxhash, openssl) until A/B 1 resolves (xxhash stays for the recipe checksum), gtest for tests, optional liburing on Linux. Logging has no dependency: it goes through the metall logger interface (section 13) with a simple default implementation in Privateer. Metall itself stays C++17 headers and compiles inside downstream C++23 translation units.

## 19. Removed features

- The sigaction (x86-only) and uffd fault engines: replaced by the barrier. uffd write-protect does not cover file-backed private mappings, and the sigaction engine does I/O in the handler.
- The stash and eviction tier with its LRU lists: replaced by the page cache for clean data and the cleaner plus governor for dirty data, with the difference that eviction I/O now runs on the executor instead of inside the fault handler. Removes Boost.UUID and the multi-tier path logic.
- Live-block zstd compression: incompatible with mapping files directly. May return as an offline tool for cold snapshots.
- `_blocks_path` external block stores: incompatible with plain-file deletion semantics.
- OpenMP and MPI: the executor and MPI-free tests.

## 20. Decisions delegated to A/B measurements

1. **Hash algorithm**: BLAKE3 (fast, parallel, cryptographic), SHA-256 (baseline), xxh3-128 (fastest, non-cryptographic). The dedup byte-compare (phase 2c) makes a weak hash safe: a collision costs a compare and an error, never corruption. Measured on commit latency at Tentris checkpoint sizes, x86_64 and aarch64.
2. **Addressing scheme**: content-addressed (dedup) versus generation-addressed names (slot plus epoch, no hashing; cross-snapshot sharing still via hard links). Decides whether hashing earns its cost on real Tentris data.
3. **Block size**: 2, 8, 32 MiB. 2 MiB matches metall's chunk size, so the large-object free path reclaims exactly; larger blocks cut VMA count, fault count, and recipe size.
4. **Commit parallelism**: worker count scaling on both architectures.
5. **Background write-back**: off, on, eager-durable. Measured on bulk-load peak memory, checkpoint latency, and write amplification.
6. **Batched fdatasync via io_uring** versus a thread pool over plain fdatasync (Linux), on checkpoint latency at realistic dirty-block counts.
7. **Governor tuning**: watermark placement and sweep interval, on peak RSS, writer stall time at the hard mark, and reader latency impact of trimming.
