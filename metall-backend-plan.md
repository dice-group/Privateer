# Privateer metall backend: implementation plan

Executes the design in `metall-backend-architecture.md`. Assessment in `metall-backend-assessment.md`. Base branch: `v0.1-dev`; work branch: `rewrite`. Every step ends with a green build and tests on all CI targets; A/B decisions are resolved by measurement before the on-disk format freezes.

## Working mode

Every change is a PR into `rewrite`, and Alex reviews every PR before it merges. Merges are squash merges into `rewrite` after approval. Commit messages are a single line with no AI trailers. PR titles are short; the description is one or two sentences.

## Targets and toolchain

- Platforms: Linux x86_64, Linux aarch64 (required), Darwin ARM64 (desirable; its unresolved probes gate only the Darwin CI leg, never the Linux work).
- Language: C++23 for the engine, against a recent libstdc++. Metall stays C++17 headers; downstream compiles at C++23 (tentris-lib, hypertrie), covered by CI.
- CMake minimum 3.24. Conan 2 for all dependencies; no FetchContent.
- Compilers in CI: libstdc++ on every platform, never libc++. Linux: the dev container toolchain, gcc 15 and clang 21 (clang against libstdc++). Darwin: Homebrew gcc 15 (AppleClang is not used; it would mean libc++). Compatibility floor gcc 14.3 (present as Homebrew gcc@14). TSan runs with clang only (gcc's libtsan SIGILLs on aarch64 under the Apple-silicon Docker VM). When downstream builds enable aarch64 branch protection (BTI or PAC), the engine builds with the same flags (the signal-handler entry needs its landing pad); the CI matrix includes one branch-protected aarch64 leg. TSan jobs must not let TSan own the fault-signal dispositions.
- Dependencies after the rework: standalone asio 1.38.x (internal executor: two thread_pools, work plus timers, per architecture section 7; `ASIO_STANDALONE`, `ASIO_SEPARATE_COMPILATION`, `ASIO_NO_DEPRECATED`, granular includes; its io_uring backend and file classes are not used), Boost headers for `boost::unordered_flat_map` (refcount map, recipe-name and slot bookkeeping; header-only, no compiled Boost libs), gtest (tests only), and the hash candidates blake3, xxhash, openssl (all on conan-center; all three present until A/B 1 resolves, then one remains, xxhash stays for the recipe checksum). Optional, Linux, benchmark-gated: liburing for batched fdatasync in the durability barrier. Removed: Boost.UUID usage, OpenMP, MPI, zstd (may return later for offline snapshot compression).

## Step 0: groundwork (about 1 week)

- Branch `rewrite` off `v0.1-dev`.
- Conan 2 recipe and CMake modernization: static library plus tests and benchmarks, options for the hash backends. The conan asio package is header-only with no options and already injects `ASIO_STANDALONE`; the build adds one dedicated `asio_src.cpp` translation unit that includes `<asio/impl/src.hpp>`, and sets `ASIO_SEPARATE_COMPILATION` and `ASIO_NO_DEPRECATED` as project-wide compile definitions (every TU, not just the src TU, or the link breaks with ODR duplicates).
- CI (GitHub Actions), modeled on dice-template-library's setup and the shared `dice-group/cpp-conan-release-reusable-workflow` pieces (`add_conan_provider`, `configure_conan`, `setup_apt`, the publish-branch-package and publish-release chain, the public dice-group conan remote). **Free public runners only**: `ubuntu-24.04`, `ubuntu-24.04-arm`, `macos-15`; never self-hosted or paid runners (tentris-storage's `arc-runner-set` and WarpBuild labels are explicitly not copied).
- PR matrix, about 7 jobs: ubuntu-24.04 with gcc-15 and clang-21, both RelWithDebInfo plus ASan+UBSan; one clang-21 TSan leg (x86_64, short suites only, `continue-on-error` until stable, then blocking); ubuntu-24.04-arm with gcc-15 and clang-21 RelWithDebInfo (the platform probes run here); macos-15 with Homebrew gcc-15, RelWithDebInfo, no sanitizers, tight `timeout-minutes`. gcc-15 and clang-21 come from the toolchain apt repos via `setup_apt`; verified in the first PR, since both are newer than what the sibling repos pin.
- Speed rules: tests trigger on `pull_request` only, publish on `push` to `rewrite` (dice-template-library's double-fire on push plus PR is not copied); concurrency groups with `cancel-in-progress` and `fail-fast: false` everywhere; `timeout-minutes` on every job; conan package cache (`~/.conan2/p`) keyed by os, compiler, and a hash of the conanfile; **ccache** on top (neither sibling repo has it; sanitizer rebuilds of a compiled library are exactly where it pays), keyed by compiler and build type; build and ctest parallelism matched to the runner's core count; short-versus-long test label split so PRs run the short suites and the long crash and stress suites run in a separate non-blocking job (tentris-storage's pattern); portable `-march` baselines (`x86-64-v2`, `armv8-a`), never `-march=native` on heterogeneous public runners.
- Sanitizer jobs run with `ASAN_OPTIONS=allow_user_segv_handler=1`, and the barrier's futex path carries TSan annotations (the barrier generates SIGSEGV as normal operation; sanitizers own those signals by default).
- Strip MPI from the inherited GTest suite so it runs in CI; it guards the old engine until the old engine is deleted.
- Fix the C API header linkage bugs while touching the build (functions declared `static` inside `extern "C"`).

Acceptance: library and tests build and pass through conan on all three platforms.

## Step 1: platform probes and primitives (about 2 weeks)

Probes first, as small CI tests, because several design assumptions must be validated before the engine is built on them (architecture section 14):

- Linux: mprotect downgrade shootdown synchrony (two-thread probe validating the commit capture freeze); MADV_PAGEOUT shared-folio behavior (double-mapping probe, run per target kernel); mincore counting cache rather than RSS; aarch64 tagged-pointer faults (TBI); overlayfs directory-fsync durability (crash probe on an upperdir).
- Darwin: MAP_FIXED replacement atomicity under a reader-hammer stress; barrier fires as SIGBUS; directory durability under F_FULLFSYNC (documented manual power-cut procedure, best effort).
- Both: futex or nanosleep-backoff wait from inside the signal handler; signal chaining including SIG_DFL and SIG_IGN.

Primitives, each with unit tests:

- result and error types; the metall-logger-interface binding with Privateer's simple default implementation (architecture section 13).
- Durable file utilities: temp plus fdatasync plus rename or linkat publication, directory fsync, O_TMPFILE detection, F_FULLFSYNC policy.
- Region registry, handler installation and chaining (per-thread mlocked sigaltstack with SA_ONSTACK, mlocked handler-text range), mlocked state array, in-flight counters and the epoch-pair lookup gate, RLIMIT_MEMLOCK and RLIMIT_NOFILE raising with the refuse-to-open path.
- Fork-based crash harness: child process killed at injected points between syscalls; port of the dice-template-library sandbox (the same harness the metall fork's durability tests ported), used by every later step.

## Step 2: block store and recipe (2 to 3 weeks)

- Content-addressed store: 256 shard skeleton, publication, dedup byte-compare, durable-name set, refcount map, unlink candidates, open-time sweep.
- Recipe: binary format with header and checksums, atomic commit, open validation including block file size checks.
- Crash tests: kill at every phase boundary of a durable and a non-durable commit; property checked on reopen: the store equals the last durable recipe, the sweep removes exactly the garbage, never a referenced block.
- A/B 1 (hash algorithm) microbenchmark: blake3 vs sha256 vs xxh3-128 with byte-compare, block sizes 2, 8, 32 MiB, x86_64 and aarch64. A/B 2 (content vs generation addressing) gets its microbenchmark here; final call waits for step 5 data (dedup rate on real workloads).

## Step 3: the engine (4 to 6 weeks)

- Region lifecycle: create, open, open_read_only, close with the shutdown quiescence protocol (epoch-pair gate flip plus handler and free in-flight counter drains, per architecture section 12), the pthread_atfork child handler that poisons open regions (architecture section 7).
- Slot state machine and handler (architecture section 3, including the poisoned error path), extend, the commit pipeline with per-slot release (section 4), free_region (section 10), snapshot_to and copy (section 9).
- Executor, background write-back, and memory governor (sections 5 to 7): the two asio thread_pools first (work pool with post-plus-counter fan-out for the commit path, one-thread timer pool), then the cleaner with cold-first victim selection, capped re-dirty backoff, and the hard-mark backoff override, then the governor (dirty budget with the timed in-handler hard-watermark wait; resident budget Linux-only, soft watermark and low target, smaps_rollup accounting, MADV_PAGEOUT trimming). Each lands after the synchronous commit path is stress-clean, since all of them reuse the per-slot protocol. Governor tests: budgets hold under bulk load (dirty) and converge under scan-plus-write mixes (resident), the timed wait bounds writer stalls, no deadlock when the writer blocks at the hard mark while holding application locks, poisoned slots fail commits and close without hanging.
- Executor lifecycle tests: a region closed while tasks are still queued sees every queued no-op run and its outstanding-task counter drain (no hang, no use-after-free); a sweep timer firing concurrently with close completes with operation_aborted and touches no freed state; a throw injected into a task body lands in the catch-all and sets the error flag instead of terminating the process.
- Tests:
  - deterministic single-thread state machine tests (every transition, every wait path forced via test hooks),
  - multithread stress under TSan: many readers, one writer, a GC thread calling free_region, a committer, running snapshot cycles; assertions on lost writes, torn commits, fault-loop timeouts,
  - crash tests through all commit phases and snapshot staging,
  - VMA budget behavior, read-only semantics (stray write crashes, no file mutation), error-flag path ending in check_sanity false.
- Microbenchmarks: barrier fault latency, commit throughput and latency tail with a concurrent waiting writer, reader disturbance during commit capture (TLB shootdown effect), A/B 3 (block size) and A/B 4 (commit worker scaling).
- A/B 5: background write-back off vs on (and eager-durable variant), measured on bulk-load peak memory, checkpoint latency, and wasted block versions (write amplification) under the Tentris write pattern. A/B 6, Linux: batched fdatasync via io_uring vs a thread pool over plain fdatasync, on checkpoint latency. Decision-grade setup: a real block device with a volatile write cache (NVMe SSD, ext4, the acceptance-gate target; never tmpfs or a cache-ignoring virtual disk where fdatasync is a no-op); a sweep over dirty-block counts (1, 8, 64, 512, 4096) so the crossover is visible instead of one point; both arms use fdatasync plus the shard-directory fsyncs the barrier requires, with the directory fsyncs batched into the io_uring ring too (IORING_OP_FSYNC on the dir fd); the thread-pool arm's worker count matched to the io_uring queue depth, so the comparison is batching versus threads, not depth-1 versus depth-N. A/B 7: governor watermark placement and sweep interval, measured on bulk-load peak RSS, writer stall time at the hard mark, and query-latency impact of `MADV_PAGEOUT` trimming under concurrent readers.

## Step 4: metall adapter (about 2 weeks)

- New `include/metall/ext/privateer.hpp` in the metall fork, branch `feature/durable-file-handling`, per architecture section 15.
- Run metall's own kernel test suite and the new durability tests with the privateer backend as an additional CI leg in the metall fork (`METALL_USE_PRIVATEER`).
- Parity tests for snapshot, copy, consistent, remove, open_read_only concurrency, and the close-refuses-mark-on-failure path.

## Step 5: benchmark gate and A/B resolution (2 to 3 weeks)

Workloads, run against the default backend and the privateer backend:

- Synthetic, in this repo: bulk allocation load, random pointer-chase reads, mixed read-write with periodic checkpoints, matching the shape of hypertrie node graphs.
- Real: tentris-lib loaded via metall-ffi with the backend switched, on a standard RDF benchmark dataset; measure load time, query latency and throughput (readers concurrent with a writer), checkpoint duration, disk usage across a checkpoint retention series, checkpoint deletion time. This arm uses a local development wiring: metall-ffi built against the metall fork branch (`feature/durable-file-handling` plus the adapter) via conan editable or local recipes; the productionized packages are step 6 deliverables and are not a prerequisite here.

Acceptance gate (the user-facing requirement): read and write and read-write at least at parity with pure metall within measurement noise; checkpoints strictly better than copy-based snapshots on ext4. If the gate fails, the contingencies below apply before any format freeze.

Resolve A/B 1 to 7, freeze recipe format v1, delete the sigaction and uffd engines and the stash tier.

## Step 6: downstream rollout (2 to 3 weeks)

- Package `privateer/0.2.0` on the dice conan remote; package the metall fork (the adapter alone cannot be carried as a patch on upstream metall 0.32, because it targets the fork's durability-reworked kernel contract; a conan patch would have to carry the whole durability rework, so packaging the fork is the committed route) and bump metall-ffi with a backend option.
- tentris-lib develop: integration behind a build option; soak test of the MVCC pattern: live writer, GC thread, frequent frontend checkpoints plus WAL, checkpoint retention cycling, on x86_64 and aarch64.
- Performance sign-off repeats the step 5 gate inside tentris-lib CI.

## Step 7: documentation and upstream (about 1 week)

- README rewrite, deployment guide (vm.max_map_count, overcommit expectations, Darwin lldb caveat, F_FULLFSYNC policy).
- Offer the work upstream to LLNL.

Total: about 16 to 20 weeks of focused work. Steps 0 to 2 and the probes parallelize poorly with nothing; steps 4 and 5 can overlap once step 3 stabilizes.

## Contingencies

- Barrier fault cost visible in bulk loads: larger `block_size`, or an eager materialization hint on extend (map new slots read-write and dirty up front, skipping per-slot faults during loads).
- Darwin MAP_FIXED probe fails: pause reader faults during remap on Darwin only (commit-scoped reader gate), or restrict Darwin to development use with documented caveats.
- Hash cost too high on aarch64: generation addressing (A/B 2) removes hashing from the commit path entirely.
- Reader disturbance from capture shootdowns measurable: batch capture runs harder, raise block size, or split commits into smaller capture windows.
- Sub-chunk free divergence grows stores in long soaks: switch default `block_size` to 2 MiB (matches metall chunk frees) and re-run the VMA budget numbers.
- Metall contract drift on `feature/durable-file-handling`: the adapter lives in that branch and its CI leg catches drift immediately.
