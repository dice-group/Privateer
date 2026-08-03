# Privateer

Privateer is versioned segment storage for memory-mapped data. A datastore is a directory of
fixed-size block files plus a recipe that says which block belongs at which offset. The recipe is a
small manifest plus segment files that hold its entries, one file per fixed range of offsets. The
engine maps those blocks into one contiguous address range. Blocks are mapped private and read-only,
so the first write into a block traps: a fault handler makes that block writable and the retried
store lands. A commit hashes every changed block and every changed recipe segment, publishes each
under a name derived from its content, and replaces the manifest with one atomic rename. Two blocks
with the same content are one file, and a snapshot is a second recipe over the same files.

This is version 0.2, a rewrite for use as a segment-storage backend of
[metall](https://github.com/LLNL/metall). It shares no code with version 0.1 and its API is
different. The origin of the project is [LLNL/Privateer](https://github.com/LLNL/Privateer); see
[Upstream](#upstream) below.

## What it gives an application

- **Block-granular persistence.** A checkpoint writes the blocks that changed, not the whole
  segment. The block size is the write-back unit and defaults to 2 MiB, which is metall's chunk
  size.
- **Deduplication.** A block whose content already exists under its content name is not written
  again. A block whose content did not change since the last commit is not even published.
- **Snapshots that share storage.** A snapshot hard-links the block files and the recipe segment
  files it references, so it costs a small manifest plus link counts, not a copy.
- **Checkpoints under a live writer.** The write barrier is per block, and a commit freezes only
  the blocks it captured. Readers stay live throughout, and no mapping is protected globally during
  a sync.
- **A memory budget for write phases.** Background write-back plus a dirty-byte budget caps the
  resident size of a bulk load, at the cost of writer stalls when the budget is too small for the
  working set.
- **Crash consistency.** A durable commit puts every block and every recipe segment the new manifest
  references on stable storage before the manifest is renamed into place. An interrupted commit
  leaves the previous recipe, and the previous state, intact.

## Requirements

- Linux or macOS, x86_64 or aarch64.
- A C++23 compiler. CI builds gcc 15 and clang 21 on Linux and gcc 15 on macOS.
- CMake 3.24 or newer.
- [Conan 2](https://conan.io) for the dependencies: standalone asio, Boost headers and xxHash.
  GoogleTest, Google Benchmark and, on Linux, liburing are test and benchmark dependencies only.

## Building

```bash
conan install . --build=missing -s build_type=Release
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake
cmake --build build -j"$(nproc)"
ctest --test-dir build -j"$(nproc)" -LE "long_running|probes"
```

CI configures the same tree through [cmake-conan](https://github.com/conan-io/cmake-conan) instead,
with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=conan_provider.cmake`, which resolves the dependencies
during the configure step and needs no separate `conan install`.

The two CMake options are `PRIVATEER_BUILD_TESTING` and `PRIVATEER_BUILD_BENCHMARKS`. Both default
to on for a top-level build and off when the project is added as a subproject.
`PRIVATEER_BUILD_TESTING` also compiles the test-only hooks into the library, which is why a
packaged build turns both off.

## Using it

As a Conan requirement:

```python
def requirements(self):
    self.requires("privateer/0.2.0")
```

In CMake:

```cmake
find_package(privateer REQUIRED)
target_link_libraries(app PRIVATE privateer::privateer)
```

The library needs C++23 and carries asio's single implementation translation unit, so consumers
inherit `ASIO_STANDALONE`, `ASIO_SEPARATE_COMPILATION` and `ASIO_NO_DEPRECATED`.

Create a datastore, write into it, and commit:

```cpp
#include <privateer/region.hpp>

auto region = privateer::region::create("datastore", 8ull << 30);  // capacity in bytes
if (!region) {
    std::fprintf(stderr, "create failed: %s\n", to_string(region.error()).c_str());
    return 1;
}
if (auto extended = region->extend(1ull << 20); !extended) { /* ... */ }

std::memset(region->segment(), 'p', 1ull << 20);   // the first store per block faults once

if (auto committed = region->commit(true); !committed) { /* ... */ }
```

Reopen it, and take a snapshot:

```cpp
auto region = privateer::region::open("datastore");
auto reader = privateer::region::open_read_only("datastore");   // a stray write crashes honestly
auto staged = region->snapshot_to("datastore.snapshot.staging");
```

Nothing throws. Every call that can fail returns `privateer::result<T>`, which is
`std::expected<T, privateer::error>`, and an error is a code from `privateer::errc`, the errno of
the failed syscall, and a static string naming the operation. `region::check_sanity()` reports
whether the region recorded a failure; once it is false, commits and close fail and the datastore is
left without its consistency mark.

Log messages go through metall's logger hook (`metall_log`), so an application that already provides
that sink receives the engine's messages through it. Without one, a weak default prints to stderr
and `privateer::set_default_log_min_level` sets its threshold.

## How it works

A region is one virtual-memory reservation whose base address does not move while the region is
open:

```
[ segment header | slot 0 | slot 1 | ... ]
```

The header is volatile anonymous memory and is never persisted. Every slot is `block_size` bytes and
maps its recipe entry, either a block file read-only or anonymous zeros for an empty slot. Slots
beyond the extended size stay unmapped.

**The write barrier.** A read-write open registers the region with the process-wide fault handler.
The first write into a slot traps, the handler claims the slot, makes it writable and counts it
dirty; the retried store lands and later writes to that slot are native. Making a slot writable is a
protection change, not a copy: the kernel supplies the old bytes from the page cache of the block
file as pages are written. A read-only open registers nothing.

**A commit** captures every dirty slot, hashes it, and writes back only what changed. A hash equal
to the recipe entry writes nothing at all. A new hash whose block file already exists is answered by
a compare against that file, so a duplicate writes nothing either. The remaining blocks are written
into the store. The recipe segments whose entries changed are then published under their content
names, and the new manifest replaces the old one with an atomic rename. A durable commit adds a
durability barrier before the rename and reclaims retired files after it, so it returns only when
everything the new recipe references is on stable storage. One commit runs at a time. A writer
that faults a captured slot waits for that slot's write-out and nothing else, so a commit does not
stop the process, but a consistent cut still requires that the application is not writing during it.

**Snapshots** run a durable commit and then stage a self-contained copy: every referenced block file
and recipe segment file is hard-linked, with a per-file copy where linking is refused, and the
manifest is written and synced. Both steps hold the commit mutex, so no commit in between can
reclaim a file the staged recipe references. Publishing the staged directory is one atomic rename,
which the caller does.

**Reclaim** is reference counted. An unreferenced file, block or recipe segment, is deleted after
the durable commit that retired it, and an open-time sweep removes anything left behind by a crash.

**Growth and holes.** `extend` publishes a larger size and maps the new range as anonymous zeros;
capacity is fixed when the datastore is created. `free_region` remaps the whole slots inside a range
to fresh zeros and lets their old block files be reclaimed. Slots only partly covered stay
untouched.

**Options** are in `privateer::region_options`. The ones a deployment sets are `block_size` (fixed
at create and recorded in the recipe), `cleaner` for background write-back, and `governor` for the
dirty-byte budget that caps the resident size of a write phase. `deep_verify` re-hashes every
referenced block at open, which turns the open-time check from structurally plausible into
content-verified. `region::statistics()` reports what write-out did: slots hashed, skipped,
deduplicated and written, plus write-back and stall counters, which is what makes write amplification
measurable from outside.

## Tests

`ctest` runs the suites. Labels split them:

```bash
ctest --test-dir <build> -j"$(nproc)" -LE "long_running|probes"   # the everyday run, seconds
ctest --test-dir <build> -L probes                                # platform assumption probes
ctest --test-dir <build> -L long_running                          # the soak, minutes
```

The probes are not library tests. They check the platform properties the engine is built on, with
plain platform calls, so a platform that does not hold them fails loudly instead of subtly. The soak
stresses the fault path, the cleaner and commits together, and `PRIVATEER_SOAK_SECONDS` stretches
it. CI runs the everyday suites and the probes on every leg, under AddressSanitizer with
UndefinedBehaviorSanitizer and under ThreadSanitizer as well.

## Benchmarks

`PRIVATEER_BUILD_BENCHMARKS=ON` builds five Google Benchmark binaries under `bench/`:
`region_bench` (fault, commit and snapshot paths of a whole region), `barrier_bench` (durability
barrier shapes), `hash_bench` (block hashing), `addressing_bench` (the per-block commit mechanics
of content addressing) and `recipe_bench` (recipe persistence: the segments and manifest a commit
writes for its changeset, against the whole-file write of format version 1). They write into
`TMPDIR`, so put it on the device under test, and not on a network or virtual file system when the
numbers are meant to be compared.

## License

Two licenses, both MIT, because this repository holds a rewrite next to inherited files:

- `LICENSE-UPB`: files written for this engine. They carry a line naming Data Science Group (DICE),
  Paderborn University.
- `LICENSE`: files inherited from the upstream project, together with `NOTICE`.

SPDX-License-Identifier: MIT

## Upstream

Version 0.1 of Privateer was developed at Lawrence Livermore National Laboratory and is available at
[LLNL/Privateer](https://github.com/LLNL/Privateer). Release LLNL-CODE-827155. The design it started
from is described in:

```text
K. Youssef, A. A. Raqibul Islam, K. Iwabuchi, W. -c. Feng and R. Pearce,
"Optimizing Performance and Storage of Memory-Mapped Persistent Data Structures,"
2022 IEEE High Performance Extreme Computing Conference (HPEC), Waltham, MA, USA, 2022,
pp. 1-7, doi: 10.1109/HPEC55821.2022.9926392.
```

Upstream contacts: Karim Youssef (karimy at vt dot edu), Keita Iwabuchi (kiwabuchi at llnl dot gov),
Roger A Pearce (rpearce at llnl dot gov).
