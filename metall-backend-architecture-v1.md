> Frozen v1 reference. Superseded by metall-backend-architecture.md (v2), which integrates the background write-back, the memory governor, and the asio execution model.

# Privateer metall backend: architecture

Companion to `metall-backend-assessment.md`. This document describes the target design. The engine replaces both existing fault backends (sigaction, uffd) with a single-phase write barrier over file-backed private mappings, and rewrites the storage layer for durability and reclamation. The design passed three adversarial reviews (OS semantics, crash consistency and contract fit, concurrency); the protocols below incorporate the fixes.

## 1. Design principles

1. Warm reads never enter userspace and are never blocked. Clean data is served by kernel demand paging from immutable block files. The page cache is the only cache; there is no library-level LRU, stash, or eviction tier. Cold-read faults are kernel-internal (major faults) and can briefly serialize behind commit map operations on the address-space lock; they never reach the signal handler.
2. The signal handler does the minimum: registry lookup, one state CAS, one mprotect, or one futex wait. No I/O, no allocation, no mutexes, no non-signal-safe libc. Every data structure it touches is lock-free-atomic and resident (mlocked). It is identical on all target platforms; there is no fault-type decoding.
3. Block files are immutable. A block file, once published under its name, never changes and is only ever opened read-only. All sharing (dedup within a store, hard links across snapshots) is safe because nothing writes through a shared file. Mutation happens only in private anonymous copies (kernel CoW) and produces new files at commit.
4. `sync(true)` is honest: it returns true only after every block the recipe references, the recipe itself, and the directory entries are on stable storage. This is what makes metall's properly-closed mark trustworthy. Crash recovery of a live store is all-or-nothing on that mark: a store that crashed while open does not reopen (metall refuses without the mark); recovery flows through snapshots plus the frontend's WAL. The engine's crash-ordering rules exist so that a committed recipe never references a missing or torn block, which is what makes snapshots and clean closes safe recovery points.
5. Deletion is plain file semantics. Snapshots are self-contained datastores whose block files are hard links. Deleting any datastore is `rm -r`; the link count frees shared space. There is no global registry and no cross-datastore garbage collector.
6. Errors propagate. No `exit()`. The engine is a library inside a database process.

## 2. Memory model

A region is one contiguous VM reservation:

```
[ segment header | slot 0 | slot 1 | ... | slot N-1 ]   (VM reservation = header + capacity)
```

- The reservation is created once with `mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)`. Its base address never changes while open; all metall offset pointers hang off it.
- The header is a private anonymous read-write mapping (metall's `segment_header`, volatile, never persisted by the engine).
- Each slot is `block_size` bytes. `block_size` is a datastore constant, set at create, a multiple of the system page size. Default 8 MiB, subject to A/B tuning (section 16). `page_size()` reports the system page size (metall validates it against its 2 MiB chunk size at create and open, so it must never report `block_size`).
- `size` (the extended size) grows in whole slots up to `capacity`. Slots beyond `size` stay `PROT_NONE`. `size()` reports the slot-rounded value; the handler gates on the same atomic variable, so there is exactly one authority.

### Slot states

Each slot has one atomic state byte (`std::atomic<uint8_t>`, statically asserted lock-free, array mlocked). Terminal states:

| State | Mapping | Protection | Meaning |
|---|---|---|---|
| `empty` | anonymous, `MAP_NORESERVE` | READ | reads zeros; recipe holds the empty sentinel |
| `clean` | block file, `MAP_PRIVATE` | READ | matches the recipe entry |
| `dirty` | same VMA, private CoW pages | READ+WRITE | modified since the last commit |
| `dirty_empty` | fresh anonymous | READ | freed since the last commit; recipe entry stale |

Transient claim states, each owned by exactly one actor until it publishes a terminal state: `materializing` (a faulting writer installing write access), `syncing` (the committer), `freeing` (a `free_region` caller). Slots beyond `size` have no meaningful state; the handler never consults their bytes because the `size` gate comes first.

Two ordering laws make the state byte trustworthy:

- **L1 (publish after protect)**: a terminal state is stored (release) only after the mprotect or mmap that establishes its protection has returned. `dirty` therefore implies the page is writable; `clean`, `empty`, `dirty_empty` imply the mapping is in place.
- **L2 (claim before touch)**: whoever changes a slot's protection or mapping first CASes the state to its transient (acq_rel), then issues the syscall. There is never a window where the page is read-only while the state still claims writable.

### VMA budget

VMA count is bounded by the number of materialized slots, not by capacity. 100 GiB of live data at 8 MiB blocks is 12,800 VMAs; distinct block files never merge (different inodes), adjacent empty anonymous slots do. Linux `vm.max_map_count` defaults to 65,530. VMA exhaustion is a hard failure mode: `mprotect` in the handler fails with ENOMEM and the fault becomes a crash. The handler cannot refuse a write, so the budget is enforced where the engine can fail cleanly: `extend` fails with a clear error when the worst case of the requested size (every slot within `size` becomes one VMA) would cross the budget read from the sysctl at create or open, minus configurable headroom for the rest of the process. This is conservative (empty slots that are never written cost no VMA) but guarantees a granted extend can always be faulted fully. Darwin has no equivalent limit. The deployment guide documents raising the sysctl for large stores.

## 3. The write barrier

### Signals and installation

Linux delivers protection-violation write faults as SIGSEGV (`SEGV_ACCERR`); the engine registers only SIGSEGV there. Darwin delivers them as SIGBUS (`KERN_PROTECTION_FAILURE`); the engine registers SIGBUS and SIGSEGV. The handler is installed process-wide once, `SA_SIGINFO`, previous disposition saved.

Forwarding to the previous disposition follows the standard chaining protocol: a saved `SA_SIGINFO` handler is called with all three arguments; a plain handler is called with the signal number; `SIG_DFL` is forwarded by restoring the default disposition and returning, so the retried instruction re-faults into the default action (core dump); `SIG_IGN` for a synchronous fault is treated like `SIG_DFL`.

Darwin caveat: Mach exception ports take precedence over BSD signals. Under lldb or a crash-reporter that claims `EXC_BAD_ACCESS`, the barrier does not fire. This is documented as a development-time limitation; a Mach-exception-port variant is a possible later addition, not part of this design.

### Fault flow

1. Increment the process-global lookup gate (lock-free), load the registry pointer (acquire), binary-search the fault address; on a hit, increment the found region's in-flight counter, then decrement the gate. The gate is held only for the lookup (nanoseconds), so it drains fast; the region counter is held for the whole handler run. Not found: decrement the gate, forward (genuine crash).
2. Found, but the region is read-only, marked closing, the fault is in the header, or the address is at or beyond `size` (acquire): decrement, forward.
3. Load the slot state (acquire) and loop:
   - `empty`, `clean`, `dirty_empty`: CAS to `materializing` (acq_rel). Winner: `mprotect(slot, block_size, PROT_READ | PROT_WRITE)`, store `dirty` (release, L1), futex-wake the state word, return (the retried store succeeds). CAS loser: reload and re-loop.
   - `materializing`, `syncing`, `freeing`: futex-wait on the state word with the observed value (compare-value form closes the lost-wakeup race; EINTR and spurious wakeups re-loop), then **return and retry the faulting instruction**. Never re-run the state machine after a wait: the retry classifies itself. A read that faulted against a transient window (see remap) now succeeds against the restored `PROT_READ` mapping; a write re-faults and takes the CAS path. This keeps "every fault the handler acts on is a write" true even across transient not-present windows.
   - `dirty`: return and retry. By L1, `dirty` implies the page is writable, so this is a stale-TLB spurious fault or a benign race with a fresh transition; by L2, any protection downgrade was preceded by a visible transient, which the retry will observe and wait on. Termination is guaranteed by L1 and L2 together.
4. `mprotect` failure or a corrupt state value: store the error flag (lock-free atomic, release), decrement, forward. `check_sanity()` and all later commits report failure, so `close()` withholds the mark.

The handler waits on Linux with the futex syscall (raw syscall, signal-safe). On Darwin the in-handler wait is a bounded spin loop with `sched_yield`-free backoff; `os_sync_wait_on_address` is not documented signal-safe and is only used from non-handler contexts.

On Linux the barrier signal cannot be confused with I/O or memory-pressure faults: truncated-file access and strict-overcommit allocation failures arrive as SIGBUS, which the engine does not register on Linux and which therefore crashes honestly. The engine additionally validates at map time (open, extend of the mapping set, commit remap) that every block file's size is exactly `block_size`, so a short file is an open or commit error, never a runtime SIGBUS.

### Region registry

A process-global sorted array of `(start, end, region*)` published through one atomic pointer; register and unregister build a new array and swap (release). Retired arrays are placed on a never-freed retirement list: registrations happen at datastore open and close, so the leak is bounded to a few hundred bytes per open in the process lifetime, and the handler can dereference any array it loaded without any reclamation protocol. Region structs themselves are reclaimed only after the in-flight counter quiesces (see shutdown). All registry arrays, region structs, and state arrays are mlocked: the handler must never take a page fault of its own, because the fault signal is masked while it runs and a nested synchronous fault would kill the process.

## 4. The commit path

`commit(durable)` implements `sync(bool)` and is the write half of `snapshot`. One commit runs at a time (commit mutex). Readers run concurrently throughout.

Precondition (same as upstream metall, stated honestly): the barrier only intercepts the first write into a slot per epoch. A writer that already holds write access to a `dirty` slot and races the capture mprotect can be frozen mid-store, so a consistent cut requires the application not to write concurrently with `sync` and `snapshot`. Upstream metall turns such writers into crashes via its own mprotect trick; this engine makes first-touch writers wait and in-flight writers tear the cut exactly as upstream would. Tentris serializes its writer around checkpoints, which satisfies the precondition. A rogue write that slips between a commit and `close()` lands in private pages that `release()` discards; committed state is never corrupted by it, only the diagnostic (a crash) is lost relative to upstream.

Phases:

1. **Capture.** Record `size` for this epoch (a concurrent `extend` may grow the region during the commit; the grown tail belongs to the next epoch and is persisted by the next commit). Scan the state array up to the captured size. For each `dirty` slot: CAS `dirty -> syncing` (L2), then `mprotect(PROT_READ)`. For each `dirty_empty` slot: CAS to `syncing`, remember it as an empty commit. Contiguous captured runs are downgraded with one `mprotect` call per run (one TLB shootdown per run instead of per slot). Correctness depends on `mprotect`'s synchronous global TLB shootdown: when it returns, no core holds a stale writable entry, so the content is frozen. Slots that a concurrent fault holds in `materializing` are waited for (futex) and re-examined.
2. **Write-out and per-slot release** (parallel worker pool, count from the caller or `hardware_concurrency`). Per `syncing` slot, pipelined so waiters are released per slot, not per batch:
   a. Hash the frozen content (reads mix private CoW pages and page-cache pages; both are the frozen bytes).
   b. If the hash equals the committer's recipe-table entry, the change was value-identical: skip the write. Otherwise the committer updates its recipe table for this slot to the new name. The recipe table and `size` serialized in phase 4 are exactly the state captured in phase 1 plus these per-slot updates; phase 4 never re-scans live slot states, so slots re-dirtied or freed after their release in step e do not leak into this epoch's recipe.
   c. Otherwise write a new block file: `O_TMPFILE` plus `linkat` into the shard directory (mkstemp plus rename fallback; both publish atomically). If the name already exists (dedup), byte-compare against the existing file (opened read-only) and drop the new one on a match; a mismatch is a fatal hash-collision error under a weak hash, prevented by this very check.
   d. Remap the slot with a single `mmap(MAP_PRIVATE | MAP_FIXED, PROT_READ)` of the (existing or new) named block file. One syscall, never munmap-then-mmap: the kernel replaces the VMA atomically under the address-space lock, so a concurrent reader either reads the old identical bytes or blocks on the lock inside its fault, never observes an unmapped window. Bytes are identical by construction, so readers observe no change.
   e. Store `clean` (release, L3: after the mmap returned), futex-wake. Empty commits store `empty` after their anonymous remap is already in place (nothing to do; the mapping was installed by `free_region`).
   A writer that faulted on this slot therefore waits only for steps a to e of its own slot, not for the whole commit, bounding the stall it can impose while holding application locks.
3. **Durability barrier** (durable commits only). The engine keeps one durable bit per slot, cleared whenever a non-durable commit publishes a new block for that slot, set when the block file and its directory entry have been fsynced. The barrier: `fdatasync` every referenced block file whose durable bit is clear (this covers both blocks written in this commit and blocks inherited from earlier non-durable commits, including the case of an empty dirty set), `fsync` every touched shard directory, set the bits. Without the inherited-block rule, a `flush(false)` followed by `close()` would write a durable mark over non-durable data; this rule is what makes the mark honest.
4. **Recipe commit.** Serialize the recipe, write to a temp file, `fdatasync` (durable), `rename` over `_recipe`, `fsync` the segment directory (durable). This is the atomic commit point; it strictly follows phase 3, so a committed recipe only ever references durably named, durably written blocks.
5. **Reclaim** (durable commits only). Unlink block files referenced neither by the just-committed recipe nor by any slot's live mapping. The committer owns all of this bookkeeping: the name refcount map (names referenced by its recipe table, multiple slots may share a name) and the unlink candidate list. Candidates are derived only by the committer, from its own table, when a commit replaces or empties a slot's entry; `free_region` never touches this state, it only flips slot states, and the freed slot's old name stays in the recipe table (and therefore stays referenced and safe) until the next commit captures the `dirty_empty` slot and retires the name itself. Hard links from snapshots keep shared inodes alive; unlinking here only drops this datastore's name. `fsync` affected shard directories. A crash before this phase leaves stale files for the open-time sweep.

`commit(durable=false)` runs phases 1, 2, and 4 without any fsync: page-cache atomicity, no durability promise, matching metall's `sync(false)`. It never unlinks (a lost rename must be able to resurface the old recipe with all its blocks intact) and it clears durable bits for the slots it rewrote.

Fsync flavor: `fdatasync` on Linux; on Darwin `fcntl(F_FULLFSYNC)` for files and directories alike when the durability flag is on (plain `fsync` does not flush the drive cache on APFS, and directory durability without it is not established).

### Background write-back (optional component)

A cleaner task runs commit phases 1 and 2 for individual cold dirty slots ahead of any commit: CAS `dirty -> syncing`, freeze, hash, write the block file, remap, publish `clean`, and update the committer-owned recipe table under the commit mutex. The on-disk recipe is untouched, so crash consistency is unchanged; prematurely written files that end up unreferenced are ordinary reclaim and sweep garbage.

Correctness comes from the barrier itself: any write after a slot was cleaned re-dirties it, so the next real commit recaptures it with its final content. A cleaned slot that never re-dirties is by definition unchanged. The recipe a commit renames therefore always reflects each block's state at commit time, and the consistency precondition of section 4 is not weakened. A background freeze that races an active writer produces a superseded block version, never a referenced torn one, because the writer's next store re-dirties the slot.

Victim selection: hash the blocks least likely to be written again. Use recency is not directly observable (reads never fault, and repeat writes to a dirty slot never fault), so the policy uses the signals the barrier provides: candidates are ordered by first-dirty time, and a slot that re-dirties soon after being cleaned is backed off exponentially, so hot slots stop being cleaned. Page-table access-bit sampling is a possible Linux-only refinement, not part of the design.

The cleaner may also `fdatasync` written blocks and set their durable bits, so a later `sync(true)` pays only for recently dirtied slots plus the recipe rename. The cleaner is the drain half of the memory governor below, which defines the watermark semantics. Costs, all bounded: wasted block versions for mispredicted slots, one extra fault plus a per-slot wait for a writer touching a slot mid-clean, and TLB shootdowns spread over time instead of clustered at commit.

### Memory governor (optional component)

Two budgets, each with a soft watermark (reclaim starts), a low target (reclaim stops), and a hard watermark (the mutator blocks):

- **Dirty budget**, exact and portable. Accounting is slot-granular (dirty and materializing slots times `block_size`, an upper bound). Soft: wake the cleaner, which writes back cold-first until the low target. Hard: a write fault that would take the total above the hard mark futex-waits in the handler on the governor, exactly like a wait on a `syncing` slot, and proceeds once the cleaner has drained below the hard mark. This is safe for the same reason the `syncing` wait is: the drain runs on the executor, takes no application locks, and makes progress independently of the blocked writer. It is the same backpressure as the synchronous flush, applied per fault instead of per call.
- **Resident budget**, best-effort. Accounting: a periodic `mincore` sweep over materialized slots (portable, one byte per page, executor task). Eviction of clean slots: `MADV_PAGEOUT` on Linux (reclaims in one call), `MADV_DONTNEED` plus `posix_fadvise(POSIX_FADV_DONTNEED)` on the block file as the fallback and on Darwin, where reclaim hints are weaker. Dirty slots are written back first (cleaner), then trimmed as clean. Soft and hard semantics as above; the hard mark blocks writers and `extend` only. Reads cannot be blocked or observed: a scan re-materializes clean pages through kernel minor faults, so the resident budget is a target the sweep converges to, not an invariant. The robust bound for total residency is the kernel's own reclaim under cgroup or memcg limits, which evicts clean file pages for free; the governor exists for self-imposed predictability, not as a replacement.

Recency for reclaim ordering uses the signals available: first-dirty time with re-dirty backoff for dirty slots, least recently cleaned or materialized for clean slots, optionally sharpened on Linux by accessed-bit sampling (page_idle) folded into the same periodic sweep. `MADV_DONTNEED` on clean slots of the private mapping is safe (there are no private pages to lose; the next access refaults from the immutable file), in contrast to its documented prohibition on dirty slots (section 7).

### Execution model

The engine owns a small internal executor (standalone asio, no Boost) running the commit worker pool, the background cleaner, the threshold trigger, and timers. The public API stays synchronous: metall's contract is blocking, and `sync(true)` must not return before durability. `sync(false)` may optionally be posted to the executor and return early, which metall's contract explicitly permits; the default remains synchronous because the blocking form is the memory backpressure mechanism. Nothing the executor runs is reachable from the signal handler, and the handler never posts work; it communicates only through slot states. Batched `fdatasync` via io_uring (liburing) for the durability barrier of large commits is a measured option on Linux, not a baseline commitment.

## 5. On-disk format

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

- **Block name**: lowercase hex of the content hash (algorithm per A/B, section 16; raw bytes in the recipe). Shard = first 8 bits, 256 subdirectories, created eagerly at datastore creation and fsynced there once (directory skeleton durable up front; commits never create directories).
- **Recipe** (binary, little-endian): header (magic, format version, `block_size`, capacity, size, slot count, hash algorithm id, header checksum), one fixed-width entry per slot within `size` (raw hash bytes or the all-zero empty sentinel), a trailing xxh3 checksum over the entries. A torn or corrupt recipe fails open with a clear error. At 32-byte entries, 1 TiB extended at 8 MiB blocks is a 4 MiB recipe; rewriting it per commit is cheap.
- `_blocks_path` (external block stores) and the stash tier are removed. `_granularity`, `_capacity`, `_metadata` are folded into the recipe.
- **Invariants**: (a) a committed recipe references only block files durably named before the recipe rename (durable commits) or at least atomically published (non-durable); (b) block files never change after publication and are only opened read-only everywhere, including the dedup byte-compare; (c) `blocks/` may contain unreferenced files (crash leftovers), never a missing referenced file, except after a crash mid `commit(false)`, which metall already reports as inconsistent via the missing mark.

## 6. Snapshot and copy

`snapshot(staging_base, clone, threads)` (the adapter derives the segment subtree as `storage::get_path(staging_base, "segment")`, exactly like metall's own backend, never a hardcoded path):

Steps 1 and 2 run under the commit mutex, held across both: a durable commit between them could reclaim a block the just-committed recipe still references, and the link pass would hit ENOENT.

1. `commit(durable=true)` (the mutex is already held; the commit runs inline).
2. Build the staged segment: shard skeleton, hard-link every block file referenced by the step 1 recipe (`link(2)`), write the recipe copy and `fdatasync` it (`F_FULLFSYNC` on Darwin). Metall's publication step fsyncs directories only, so the engine owns the durability of the staged recipe's content; the hard-linked blocks' contents are durable already (phase 3), and their new directory entries are covered by metall's directory-tree fsync.
3. Return. Metall fsyncs the staged tree and publishes the datastore with one atomic rename.

Per-file fallback when `link` fails: `copy_file_range`, `clonefile` on Darwin, plain copy (EXDEV across devices; EMLINK when a block's link count is exhausted, ext4 caps around 65,000, so only reachable with tens of thousands of retained checkpoints; the fallback then unshares that block, trading space for correctness).

Cost: O(referenced blocks) metadata operations plus one recipe write; no block data copied on the same filesystem. The result is a fully self-contained metall datastore. Writes after a snapshot fault `clean -> dirty` in memory and produce new files at the next commit; the snapshot's links keep old files alive.

`static copy(src_base, dst_staging_base, clone, threads)`: same hard-link publication, reading the source recipe from disk. Metall holds a shared lock on the source (no writer), and `link` only bumps inode link counts, so the source is never mutated.

## 7. Deletion and reclamation

- **Datastore and snapshot deletion**: metall's `storage::remove` deletes the tree; link counts release shared space. Frequent checkpoint retention cycles are `rm -r` cheap. Nothing else to do.
- **`free_region(offset, nbytes)`** (page-aligned, best-effort, called concurrently from the GC thread):
  - Fully covered slots, per slot: CAS `{empty, clean, dirty, dirty_empty} -> freeing` (loop with futex-wait while the state is transient, exactly like the handler), remap to a fresh anonymous `MAP_NORESERVE` `PROT_READ` mapping with one `mmap(MAP_FIXED)` (the same mapping kind as `empty`, so the state table holds on every path), store `dirty_empty` (release, L3), wake. The claim-before-remap order is what prevents a lost write: a reallocating writer that races the free either faults before the claim (its write lands in the old pages, which the free then discards deliberately, the range was freed) or faults after `dirty_empty` is published and takes the materializing path into the fresh zero pages. No write can land during the remap itself because any first-touch waits on `freeing` and in-flight writers cannot exist for a freed range (the allocator freed it). Reads observe zeros afterwards, like upstream's hole punch. The old block file becomes an unlink candidate for the next durable commit.
  - Partially covered slots: no action. `madvise(MADV_DONTNEED)` is forbidden here (on a private file mapping it resurrects stale file content under live CoW data), hole-punching the block file is forbidden by immutability (snapshots share the inode). Consequence, stated openly: sub-slot frees reclaim no disk space and freed sub-ranges keep their old bytes in memory, where upstream metall would punch and zero them at page granularity. Metall's allocator never reads freed memory expecting zeros (it tracks free space in its own bitsets), so this is a space and determinism divergence, not a correctness one. The block-size A/B (section 16) weighs 2 MiB blocks, where every whole-chunk deallocation of metall's large-object path maps to a full slot, against fault and VMA costs.
  - If the region is closing or read-only: return false without touching anything.
- **Open-time sweep** (read-write open only, after metall verified the mark): read the recipe, unlink every file in `blocks/` not referenced by it, including temp leftovers. Safe because a verified mark implies the on-disk recipe is exactly the last durable one (close ends with a durable commit), so nothing the sweep removes can be needed by any resurfacing state. Read-only opens never mutate anything.

## 8. Growth

`extend(request_size)`:

1. Round up to whole slots; fail cleanly if beyond `capacity`, beyond the VMA budget (section 2), or read-only.
2. Under the region mutex: `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, PROT_READ, MAP_FIXED)` over the new slot range (one VMA), store `empty` for each new slot (release), then advance `size` (release). The handler loads `size` (acquire) before it loads any state, so a slot is only ever examined after its mapping and state are visible. The allocator never touches memory beyond what `extend` returned (metall extends before it allocates from the new range).

No file activity: an extend that is never written costs nothing. `size` is persisted in the recipe at the next commit. A store that comes back from a durable state whose recipe predates a later extend (a snapshot, or a clean close whose final commit captured the smaller size) simply opens at the recorded size, and metall re-extends on demand; a store that crashed while open does not reopen at all (principle 4).

## 9. Concurrency model

Actors, matching downstream reality:

| Actor | Operations | Blocking behavior |
|---|---|---|
| readers (many) | read mapped memory | warm reads never blocked; cold faults are kernel-internal and can briefly serialize behind commit map calls |
| writer (one, app-serialized) | write mapped memory | one barrier fault per slot per epoch; waits at most for one slot's write-out (commit) or remap (free) |
| GC thread | `free_region` | waits per slot on transients |
| control thread | `sync`, `snapshot`, `extend`, `close` | commit mutex serializes commits; region mutex serializes extend and close bookkeeping |
| cleaner (executor task, optional) | background write-back of cold dirty slots | takes the commit mutex per batch; a slot-level committer, nothing more |

Synchronization inventory and required orderings:

| Variable | Written by | Discipline |
|---|---|---|
| slot state bytes | handler, committer, free, extend | CAS acq_rel; terminal stores release after the protecting syscall (L1, L3); all loads acquire; statically asserted lock-free |
| `size` | extend | store release after mappings and states; loads acquire |
| registry pointer | register, unregister | store release, load acquire; arrays never freed |
| error flag | handler | store release, loads acquire, lock-free |
| in-flight counters | handler entry and exit | lock-free increments; close waits for zero |
| refcount map, unlink candidates, durable bits, recipe table | commit-mutex holders only (committer or cleaner) | no other reader or writer, ever; `free_region` communicates only through slot states |

The handler reads only the registry, `size`, the state bytes, and the in-flight and error atomics. mprotect and mmap on distinct slots serialize on the kernel address-space lock; that is a latency coupling, not a correctness one.

### Shutdown

`release()` (called by metall's close after `sync(true)`, and by the failure paths):

1. Store the closing flag (release). New faults on the region forward as crashes (the app must have quiesced readers and writers; metall guarantees no API calls after close begins); `free_region` and `commit` return false.
2. Take the commit and region mutexes (waits out a straggling commit).
3. Unregister the region from the registry (swap; the retired array is kept, never freed).
4. Wait until the global lookup gate has been observed at zero once (no handler is still between loading the old registry and taking a region reference; the gate covers only the lookup, so this drains in microseconds even under fault storms on other regions), then wait until this region's in-flight counter reaches zero (every handler that took a reference has left; the app has quiesced its threads per metall's close contract, so no handler can be parked on a transient here).
5. Unmap the reservation, close fds, then free the region struct and state array.

This closes the use-after-free windows between a late fault or a late `free_region` and teardown.

## 10. Error handling

- Public API returns `bool` where metall's contract wants bool, with a queryable `last_error()` (code plus message) on the region; internal functions return a result type. No exceptions cross the adapter boundary; no `exit()` anywhere.
- The handler's only failure action is the error flag plus forwarding. `check_sanity()` and every later `sync` report failure, `close()` refuses the mark, metall reports the store inconsistent. Fail-safe direction is always "not consistent".
- Out-of-memory honesty: private dirty pages are committed anonymous memory. Under strict overcommit (`vm.overcommit_memory=2`) a CoW allocation failure surfaces as SIGBUS on Linux, which the engine does not intercept, so it is an honest crash, not a mislabeled barrier fault. The deployment guide documents overcommit expectations; a dirty-bytes high-water knob that triggers an early flush is a config option, not a correctness mechanism.
- Log via a callback interface, default binding spdlog, so metall or the host application can capture engine logs.

## 11. Platform matrix

| Concern | Linux x86_64 / aarch64 | Darwin ARM64 |
|---|---|---|
| barrier signal | SIGSEGV (`SEGV_ACCERR`) | SIGBUS (`KERN_PROTECTION_FAILURE`), SIGSEGV also registered |
| fault decode | none (single-phase) | none |
| in-handler wait | futex, compare-value | bounded spin |
| temp files | `O_TMPFILE` + `linkat`, mkstemp fallback | mkstemp |
| data barrier | `fdatasync` | `fcntl(F_FULLFSYNC)`, files and directories |
| remap | one `mmap(MAP_FIXED)` call | one `mmap(MAP_FIXED)` call |
| hard links | all POSIX filesystems | APFS |
| VMA limit | `vm.max_map_count` budgeted | none |

Platform assumptions that ship with runtime or CI probes rather than faith: Darwin `MAP_FIXED` replacement atomicity against concurrent readers (stress probe); Darwin directory-entry durability under `F_FULLFSYNC` (crash probe); Linux mprotect downgrade shootdown synchrony (two-thread probe, validates the phase 1 freeze); barrier functionality under a debugger on Darwin (documented limitation, Mach ports win over signals). No userfaultfd, no Mach exception ports, no `/proc` parsing, no mremap.

## 12. Metall adapter

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

## 13. Performance against the default backend

| Path | metall default (MAP_SHARED) | this engine |
|---|---|---|
| warm read | native | native (same page cache) |
| cold read | kernel major fault, file I/O | identical |
| warm write | native | native |
| first write to a block per epoch | native (first-touch minor fault) | signal + mprotect, about 1 to 3 us, once per `block_size` dirtied |
| `sync(true)` | msync whole segment in place | hash plus write of dirty blocks to new files, fsync, recipe; skips value-identical blocks and already-durable blocks |
| snapshot | full copy, or reflink where the filesystem has it | O(dirty since last commit) plus O(blocks) hard links, on any filesystem |
| free, whole chunks | hole punch, immediate | remap to zeros now, unlink at next durable commit |
| free, sub-chunk | hole punch, immediate | none (documented divergence, section 7) |
| crash safety of `consistent()` | honest since the durability rework | honest, same rules |

Expected outcome for Tentris: reads identical; bulk-load writes near-identical (a 10 GiB load at 8 MiB blocks pays about 1,280 barrier faults, milliseconds in total); frequent checkpoints are where this engine wins, paying O(dirty) instead of O(datastore) on every filesystem. Memory overhead: dirty private pages exist alongside stale page-cache pages between fault and commit, bounded by checkpoint cadence.

Risk ledger, each with a measurement in the plan: TLB shootdown IPIs against many readers during write-heavy phases and commit capture; VMA count at small block sizes; hash throughput on aarch64; commit latency tail from the per-slot pipeline under a concurrent writer; sub-chunk free divergence on long-running stores; recipe rewrite size for multi-TiB stores.

## 14. Configuration

Datastore constants (recipe header, set at create): `block_size`, hash algorithm. Runtime knobs (API, not silently via environment): commit worker count, VMA budget headroom, Darwin `F_FULLFSYNC` policy, cleaner and governor on or off, dirty budget and resident budget each as a soft watermark, low target, and hard watermark, cleaner backoff parameters, governor sweep interval. A mismatch between recipe header and requested options fails open with an error.

## 15. Removed features

- The sigaction (x86-only) and uffd fault engines: replaced by the barrier. uffd write-protect does not cover file-backed private mappings, and the sigaction engine does I/O in the handler.
- The stash and eviction tier with its LRU lists: clean memory pressure is the page cache's job; dirty memory is bounded by commit cadence. Removes Boost.UUID and the multi-tier path logic.
- Live-block zstd compression: incompatible with mapping files directly. May return as an offline tool for cold snapshots.
- `_blocks_path` external block stores: incompatible with plain-file deletion semantics.
- OpenMP and MPI: a std::thread worker pool and MPI-free tests.

## 16. Decisions delegated to A/B measurements

1. **Hash algorithm**: BLAKE3 (fast, parallel, cryptographic), SHA-256 (baseline), xxh3-128 (fastest, non-cryptographic). The dedup byte-compare (phase 2c) makes a weak hash safe: a collision costs a compare and an error, never corruption. Measured on commit latency at Tentris checkpoint sizes, x86_64 and aarch64.
2. **Addressing scheme**: content-addressed (dedup) versus generation-addressed names (slot plus epoch, no hashing; cross-snapshot sharing still via hard links). Decides whether hashing earns its cost on real Tentris data.
3. **Block size**: 2, 8, 32 MiB. 2 MiB matches metall's chunk size, so the large-object free path reclaims exactly; larger blocks cut VMA count, fault count, and recipe size.
4. **Commit parallelism**: worker count scaling on both architectures.
