# Test Privateer

The tests use GoogleTest and run through `ctest`. Build them with `PRIVATEER_BUILD_TESTING=ON`, see
[Build Executables](./cmake.md).

## Running

```bash
ctest --test-dir build -j"$(nproc)" -LE "long_running|probes"   # the everyday run, seconds
ctest --test-dir build -L probes                                # platform assumption probes
ctest --test-dir build -L long_running                          # the soak, minutes
```

Two labels split the suites off from the everyday run:

* `probes` are not library tests. They check properties of the platform that the engine is built on,
  with plain platform calls and no library code:
    * the signal mechanics of the write barrier: a write to a read-only private file mapping faults
      with the expected signal and code, the handler can park on a timed wait and be woken, and a
      timeout lets it proceed,
    * that `mprotect` returning means no other core can still write through a stale writable TLB
      entry, which is what makes the commit capture sound,
    * `MAP_FIXED` replacement atomicity under concurrent readers, which the commit remap needs,
    * the durability plumbing of block publication: `O_TMPFILE` plus `linkat`, `fdatasync`, directory
      fsync, and `EEXIST` on a racing publication,
    * on Linux, that `MADV_PAGEOUT` evicts clean file pages and what `mincore` actually reports,
    * on aarch64 Linux, what top-byte-ignore does to a fault address.

  A platform that does not hold these fails here rather than subtly at runtime. Some of them ask the
  kernel for advisory behaviour that a shared machine may decline, so CI records their result instead
  of failing the job on it.
* `long_running` is the soak. It drives the fault path, the cleaner, commits and snapshots against
  each other for a fixed time and checks the invariants throughout. `PRIVATEER_SOAK_SECONDS`
  stretches it.

Crash tests are part of the everyday run. They fork a child, kill it inside a chosen commit phase,
and check that reopening the datastore lands on the previous version with nothing lost and nothing
half-written.

## Under sanitizers

The suites are meant to be run under sanitizers, and CI does it on every leg: AddressSanitizer with
UndefinedBehaviorSanitizer, and ThreadSanitizer separately, on x86_64 and aarch64. Locally:

```bash
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan -j"$(nproc)" -LE "long_running|probes"
```

## Where datastores are written

Every test makes its own `mkdtemp` directory under the system temporary directory, so `TMPDIR`
selects the device, and removes it again when it ends. Point `TMPDIR` at a real disk when timing
anything; a virtual file system changes write costs enough to matter.
