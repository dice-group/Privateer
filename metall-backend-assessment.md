# Privateer as a metall segment-storage backend: assessment

Assessed 2026-07-21. Sources: this repo (dice-group/Privateer, all branches, main at `dcb5a80`, v0.1-dev at `a15bbf0`), metall fork at `/Users/bigerl/Code/metall` branch `feature/durable-file-handling` (`2a5afdf6`), the design notes in that repo (`cow-snapshot-backends.md`, `privateer-umap-investigation.md`, `file-handling-review.md`, `pr-durable-file-handling.md`), and the downstream repos tentris-lib, hypertrie, rdf4cpp, tentris-storage (each on `origin/develop`).

## 1. Goal

Privateer becomes a production segment-storage backend for metall. It must:

- deliver at least the performance of the default metall backend for read, write, and read-write Tentris workloads,
- support the full metall API surface used downstream, in particular snapshots and deletion,
- satisfy the durability contract of metall's `feature/durable-file-handling` branch,
- run on x86_64 and aarch64 Linux; ARM64 Darwin is desirable.

The chosen fault mechanism is the single-phase write barrier over file-backed private mappings (rank 1 in `cow-snapshot-backends.md`): each materialized block file is mapped `MAP_PRIVATE` at its slot with `PROT_READ`. Reads run at page-cache speed with no userspace involvement. Any fault on a readable page is a write. The handler does mprotect plus a dirty bit, no I/O, no per-arch fault decoding.

## 2. How downstream uses metall

All findings from `origin/develop` of each repo.

- Metall is consumed only through metall-ffi (conan package `metall-ffi/0.2.9`, wrapping upstream LLNL `metall/0.32` from conan-center). `dice::metall_ffi::internal::metall_manager` is an alias for `metall::manager`. No repo sets any `METALL_*` macro: block size 256 MiB, capacity reservation 8 TiB, concurrency support on, file-space freeing on.
- rdf4cpp is allocator-generic and has no metall dependency. The metall-backed rdf4cpp node storage lives in tentris-lib (`libs/metall-node-storage`). tentris-storage does not use metall; it is an independent CoW page store with snapshots and a WAL, and serves as an in-house design reference (`src/tentris/storage/Allocator.hpp`, `Manager.hpp`).
- APIs used: `create_only`/`open_only`/`open_read_only`, `construct`/`find`/`destroy`, `get_allocator`, `metall::container` types, `snapshot`, `remove`. C API in metall-ffi exposes the same set.
- Persistent data: the whole hypertrie node tree, the rdf4cpp node-storage backends and their maps, and a binary-format version stamp. Reads are random pointer chases through offset-pointer graphs. Writes arrive as bulk loads and batched update transactions producing many small allocations.

### Versioning model

The MVCC lives in the application, inside one live metall datastore: `RawMultiVersionHypertrieContext` keeps a version list of hypertrie roots. Writes always create a new hypertrie version. Readers serve queries from the live segment. The frontend turns committed hypertrie versions into durable checkpoints: it creates a metall snapshot together with a WAL that covers commits since that snapshot. Consequences for the backend:

- Snapshot creation is frequent (per commit batch). It must cost O(dirty data), not O(datastore).
- Snapshot deletion (retention of a bounded checkpoint set) is equally frequent and must reclaim space.
- Snapshots are taken while reader threads are active. The Rust binding tests exercise exactly this.

### Concurrency model

- One serialized writer (hypertrie `writer_mutex_`).
- A background garbage-collector thread frees retired hypertrie versions. Deallocation, and therefore `free_region`, runs concurrently with the writer and the readers.
- Many reader threads read mapped memory concurrently with all of the above.
- Downstream relies on metall's internal synchronization (`METALL_DISABLE_CONCURRENCY` is not set).

## 3. The metall contract

Branch `feature/durable-file-handling`, metall v0.32 base. The backend is a compile-time template parameter of `basic_manager`: a `segment_storage` class plus a `storage` path-manager class. Reference: `include/metall/kernel/segment_storage.hpp`, `storage.hpp`; call sites in `manager_kernel_impl.ipp` (MKI below).

Required members (signatures from the reference backend):

| Member | Semantics, call sites |
|---|---|
| `path_type` (= `std::filesystem::path`), `segment_header_type` | type plumbing |
| default ctor, move ctor/assign, dtor | `page_size()` must be valid right after default construction (MKI:675) |
| `bool create(path, capacity)` | reserve VM, map header, materialize first block (MKI:967) |
| `bool open(path, capacity, read_only)` | map existing blocks (MKI:894) |
| `bool extend(size)` | grow within reserved VM, called from the allocator on demand (`segment_allocator.hpp:515`) |
| `bool release()` | unmap, close; return value checked in `close()` (MKI:74) |
| `bool sync(bool sync)` | flush segment; `sync(true)` must be durable (MKI:70, 95) |
| `bool free_region(offset, nbytes)` | uncommit on deallocation, page-aligned args, best-effort, return ignored (`segment_allocator.hpp:643,662`) |
| `bool snapshot(dest_path, clone, threads)` | produce a segment tree in metall's temp staging dir (MKI:1119) |
| `void *get_segment()`, `segment_header_type &get_segment_header()` | stable base address for offset pointers |
| `size_t size()`, `size_t page_size()`, `bool read_only()`, `bool is_open()`, `bool check_sanity()` | state queries |
| `static bool copy(src, dst, clone, threads)` | datastore copy into staging dir (MKI:1218) |

The `storage` class supplies `get_path`, `root_path`, `create` (destroys and recreates the root), `remove`.

Durability and locking rules that the rework introduces:

- `close()` writes the properly-closed mark only if serialize, `sync(true)`, and `release()` all succeeded (MKI:58-90). `consistent()` is honest only if `sync(true) == true` implies the data is on stable storage.
- Locking is owned by metall: a persistent lockfile `mds_lock` with flock, exclusive for writers, shared for readers, taken before any backend call. The backend does not lock. A read-only open must not mutate shared files.
- `snapshot()` and `copy()` write into `<destination>/.tmp_datastore`. Metall fsyncs the tree and publishes with one atomic rename. The backend only builds a self-consistent segment subtree there.
- The default backend hole-punches freed regions live (`MADV_REMOVE` where supported).

The existing adapter `include/metall/ext/privateer.hpp` does not compile against this kernel (void vs bool returns of `sync`/`release`, `std::string` vs `path_type` parameters), ignores the `sync(bool)` flag, stubs `extend` and `free_region`, and predates the staging model. The metall `privateer2.0` branch is v0.25-era with an incompatible interface shape. The adapter is a rewrite, not a refresh.

## 4. Privateer today

### Branch map

All post-2024 work is by one developer. A shared feature chain grows off `main` and forks at `ff86a40` (2025-08) into three siblings:

- `feature/compress_stash` (2025-11): stash compression, madvise on eviction, spdlog, read-only guard, GTest suite. Tip is a WIP checkpoint.
- `merge/tmp_smartcache` (2025-08) and its ancestor `feature/mt_smartcache` (2025-06): SmartCache tier merged with the chain.
- `v0.1-dev` (2026-07, active): the same feature code reorganized into a factory-based architecture with real translation units.

`feature/uffd` (2023) and `feature/s3` (2023) are stale prototypes; the uffd code was forward-ported into the chain. `feature/sigaction` is fully merged into `main`. `main` itself is the frozen 2024 upstream snapshot.

### v0.1-dev architecture

- `Privateer` facade over `virtual_memory_manager_base` with two implementations selected at compile time: `sigaction_virtual_memory_manager` (642 lines, working, fault decoding via x86-only `REG_ERR`, one process-global handler mutex) and `uffd_virtual_memory_manager` (671 lines, working, Linux-only, arch-neutral fault decoding, multi-threaded handlers).
- `block_storage_base` with `posix_block_storage` (working content-addressed store: SHA-256 via OpenSSL, 1024 sharded subdirs, mkstemp plus rename publication, optional zstd, stash tier for evicted dirty blocks) and `smartcache_block_storage` (stub).
- On-disk format: `blocks/<shard>/<64-hex-sha256>` plus `blocks/_granularity`; a version is a directory with `_metadata` (flat array of 64-byte hashes, one per slot), `_blocks_path`, `_capacity`. No version registry, no lineage, no refcounts.
- Tests: 16 parametrized GoogleTest cases; the suite hard-requires MPI and GTest and does not link in CI.
- Build: CMake 3.5-era, C++17, OpenSSL and OpenMP required, Boost (uuid), spdlog and Boost via FetchContent, zstd optional.

### Engineering state relative to the goal

1. Durability: zero fsync, fdatasync, or msync(2) calls in any active branch. Blocks are published with pwrite plus rename, the recipe with a single in-place pwrite. Crash consistency does not exist. This directly conflicts with the mark honesty rule of section 3.
2. Error handling: 234 `exit(-1)` sites on v0.1-dev, several inside the signal handler. One `throw` in the whole codebase. Unusable inside a database process.
3. Portability: the sigaction path is x86_64-only (`gregs[REG_ERR]`). The uffd path is arch-neutral but Linux-only, needs seccomp allowances in containers, and uffd write-protect cannot cover the planned file-backed private mappings at all.
4. Missing lifecycle: no extend or resize (capacity fixed at create), no free_region, no snapshot deletion, no GC (the block store grows monotonically), no hole punching.
5. Concurrency: the sigaction dispatcher serializes all faults behind one global mutex. The uffd msync iterates `blocks_ids` unlocked against concurrent handler threads. Per-block locks are commented out. The stash maps race under the OpenMP msync loop. Nothing here matches the reader-writer-GC concurrency of section 2.
6. Read path cost: every cold read faults into the userspace handler including fetch I/O. The Tentris read workload is random pointer chasing, so this is the number one performance risk against the default backend, and the main reason the single-phase write barrier design replaces both existing fault engines.
7. Integration: the fixed-capacity, pre-sized region model does not fit metall's extend-on-demand allocator. The stash and LRU eviction tier duplicates what the kernel page cache does for file-backed clean pages.
8. Hygiene: C API declares functions `static` inside `extern "C"` (breaks linking), debug printf in the uffd hot loop, stale REFACTORING and README docs, LLNL cluster RPATHs in test CMake.

### What is worth keeping

- The content-addressed block store concept and the posix store implementation as the base for a durable rewrite (hashing, sharding, temp-plus-rename publication).
- The recipe-per-version on-disk model (`_metadata` slot array), extended by durability and a deletion story.
- The factory split (`virtual_memory_manager_base`, `block_storage_base`) as the seam for the new fault engine.
- The GTest suite as a starting corpus, stripped of MPI.
- zstd stash compression as an optional feature later; spdlog as the logging layer.

## 5. Starting point decision

Start from `v0.1-dev`.

- It is the only actively maintained line (2026 commits) and already has the factory decomposition the new fault engine plugs into.
- It carries all sibling features (compression, uffd, tests, spdlog) in their most consolidated form.
- `main` is only valuable as a readable single-file reference of the original algorithm.
- The other branches are salvage donors, not bases: nothing on them is ahead of v0.1-dev in a way that matters for this goal.

The new work replaces both existing VMM backends with the single-phase write barrier engine, rewrites the storage layer for durability, adds the missing lifecycle operations, and adds a new metall adapter targeting `feature/durable-file-handling`.

## 6. Gap list the architecture must close

1. A fault engine with reads at page-cache speed and O(1 mprotect) write faults, portable across x86_64 and aarch64 Linux and ARM64 Darwin, safe under concurrent reader, writer, GC, and snapshot threads.
2. A durable block store: fsync ordering for blocks, recipes, and directories; atomic recipe commit; honest `sync(true)`.
3. Growth: `extend` within a reserved VM region.
4. Reclamation: `free_region` that returns disk space, and snapshot deletion that reclaims unshared blocks.
5. Snapshots into metall staging directories, O(dirty data), concurrent with readers, self-contained result.
6. Library behavior: error propagation instead of process exit, no allocation or I/O in the signal handler, multi-datastore support in one process.
7. Modern build: conan-first dependencies, current CMake, C++20 or later, CI for the full platform matrix, tests without MPI.
8. Performance parity with the default backend on Tentris workloads, verified by A/B benchmarks (hash algorithm, block size, compression, addressing scheme).
