# API

Everything lives in namespace `privateer`. The header a user needs is `privateer/region.hpp`; the
other headers are the pieces a region is built from and are useful on their own for tools.

## Errors

```cpp
#include <privateer/error.hpp>

template<typename T = void>
using result = std::expected<T, error>;

struct error {
    errc code;
    int sys_errno = 0;      // errno of the failed syscall, 0 if none
    char const *context;    // static string naming the operation
};

std::string to_string(error const &err);   // "<code>: <context> (errno <n>: <text>)"
char const *name(errc code) noexcept;
```

Nothing throws and nothing calls `exit`. `errc` names the failure kinds a caller can act on:
`invalid_argument`, `io_error`, `datastore_missing`, `datastore_exists`, `datastore_inconsistent`,
`recipe_corrupt`, `recipe_unsupported`, `option_mismatch`, `block_file_invalid`,
`memlock_limit_too_low`, `vma_budget_exceeded`, `hash_collision`, `shutting_down`,
`capacity_exceeded`.

## Region

```cpp
#include <privateer/region.hpp>

struct region {
    // Creates the datastore and opens it. capacity is rounded up to whole blocks and
    // fixed for the lifetime of the datastore. The extended size starts at zero.
    static result<region> create(std::filesystem::path const &segment_dir, uint64_t capacity,
                                 region_options const &options = {});

    // Opens an existing datastore: validates the recipe and every referenced block file,
    // maps the slots, and sweeps files nothing references.
    static result<region> open(std::filesystem::path const &segment_dir,
                               region_options const &options = {});

    // Like open, but never mutates the datastore and refuses extend. A stray write crashes.
    static result<region> open_read_only(std::filesystem::path const &segment_dir,
                                         region_options const &options = {});

    void *segment() const noexcept;          // slot 0, the base of the persistent range
    void *segment_header() const noexcept;   // header_size bytes of volatile memory
    uint64_t size() const noexcept;          // extended size in bytes
    uint64_t capacity() const noexcept;
    uint64_t block_size() const noexcept;
    hash_algorithm algorithm() const noexcept;
    bool read_only() const noexcept;

    // false once a failure was recorded: commits and close then fail, and the
    // datastore is left without its consistency mark
    bool check_sanity() const noexcept;

    region_statistics statistics() const noexcept;

    // Makes at least target_size bytes usable, rounded up to whole blocks. Fails beyond
    // capacity, beyond the VMA budget, or when RLIMIT_MEMLOCK cannot hold the grown state
    // array. The new size is persisted by the next commit.
    result<> extend(uint64_t target_size);

    // Discards the whole blocks fully covered by the range: they read as zeros again and
    // their block files become reclaimable. Partly covered blocks stay untouched.
    result<> free_region(uint64_t offset, uint64_t nbytes);

    // Writes back what changed and replaces the recipe atomically. durable adds the
    // durability barrier before the rename and reclaims retired blocks after it.
    result<> commit(bool durable);

    // Durable commit, then a self-contained copy of the result in staging_segment_dir,
    // with the block files hard-linked. The caller publishes it with a rename.
    result<> snapshot_to(std::filesystem::path const &staging_segment_dir);

    // Same staging for a datastore that no writer holds open. The source is not modified.
    static result<> copy(std::filesystem::path const &src_segment_dir,
                         std::filesystem::path const &dst_segment_dir);
};
```

A region is movable and not copyable. Closing it (the destructor) requires quiesced writers: a
writer that is parked at the dirty hard watermark while the region closes has its fault forwarded as
a crash.

Addresses inside the segment are only valid while the region is open, and a later run maps the same
content at a different base. Data structures that persist must therefore use offsets.

## Options

```cpp
struct region_options {
    std::optional<uint64_t> block_size;      // create default 2 MiB; must match the recipe on open
    std::optional<hash_algorithm> algorithm; // create default xxh3_128
    size_t header_size = 0;                  // volatile bytes mapped before slot 0
    size_t vma_headroom = 4096;              // map-count entries left for the rest of the process
    bool lock_state_array = true;            // mlock the slot state pages
    bool deep_verify = false;                // re-hash every referenced block at open
    std::chrono::nanoseconds poison_timeout = std::chrono::seconds{10};
    size_t commit_workers = 0;               // 0: hardware concurrency capped at 16
    cleaner_options cleaner;                 // background write-back
    governor_options governor;               // memory budgets
};
```

`block_size` and `algorithm` are datastore constants. They are written into the recipe header at
create and adopted from it at open; setting them to something else on open fails with
`option_mismatch`.

`deep_verify` reads every unique referenced block once and checks it against its name. Size
validation always runs and catches truncation; deep verify also catches content corruption.

### Background write-back

```cpp
enum struct cleaner_mode : uint8_t { off, non_durable, eager_durable };

struct cleaner_options {
    cleaner_mode mode = cleaner_mode::off;
    std::chrono::nanoseconds interval = std::chrono::seconds{1};
    size_t batch_slots = 8;
    std::chrono::nanoseconds backoff_base = std::chrono::milliseconds{500};
    std::chrono::nanoseconds backoff_cap = std::chrono::seconds{30};
    size_t failure_limit = 8;
};
```

`non_durable` is the mode to use: it writes and remaps blocks, and their durability stays with the
next durable commit. `eager_durable` syncs every batch, which costs more and delivers less, because
a barrier gets cheap by spreading many syncs together and a commit already does that.

The cleaner is an optimization, so its failures are not fatal. A failed batch unwinds its slots back
to dirty and backs off; after `failure_limit` consecutive failures the cleaner disables itself for
the region's lifetime and write-back happens at commit time again.

### Memory budgets

```cpp
struct governor_options {
    uint64_t dirty_soft = 0;      // 0 disables the dirty budget
    uint64_t dirty_low = 0;
    uint64_t dirty_hard = 0;      // 0: writers never wait
    std::chrono::nanoseconds hard_timeout = std::chrono::milliseconds{100};
    uint64_t hard_floor_blocks = 8;

    uint64_t resident_soft = 0;   // 0 disables the resident sweep; Linux only
    uint64_t resident_low = 0;
    std::chrono::nanoseconds sweep_interval = std::chrono::seconds{1};
};
```

The dirty budget needs the cleaner, because the cleaner is what drains dirty bytes between commits.
Above `dirty_soft` the cleaner writes back cold blocks first until dirty bytes are at `dirty_low`. A
write fault that would take dirty bytes above `dirty_hard` waits until the drain brings it below,
bounded by `hard_timeout`; on timeout it proceeds and overshoots by one block.

Sizing rule: put `dirty_soft` at the dirty memory the deployment can spare, `dirty_low` at three
quarters of it, `dirty_hard` at one and a half times it. The resident set of a write phase settles
near `dirty_soft`. The write volume does not grow from this, because the cleaner writes a block early
instead of a commit writing it later. What a tight budget costs is writer stalls, and below about one
eighth of the bytes the phase dirties it starts to lose write throughput to them.

The resident budget is advisory, Linux only, and one process-level sweep serves every region that
sets one. It only pages out clean and empty blocks, so it cannot bound a write phase, and against
concurrent readers it thrashes. It suits a process that writes without serving reads.

### Statistics

```cpp
struct region_statistics {
    uint64_t slots_cleaned;     // blocks the cleaner wrote back
    uint64_t slots_redirtied;   // cleaned blocks that turned dirty again inside their backoff
    uint64_t writer_stalls;     // write faults that waited at the hard watermark

    uint64_t slots_hashed;      // blocks frozen and hashed for write-out
    uint64_t slots_skipped;     // hash matched the recipe entry, nothing written
    uint64_t slots_deduped;     // name already existed, compare replaced the write
    uint64_t slots_written;     // new block file written

    double redirty_ratio() const noexcept;
};
```

Counters are monotonic while the region is open and all zero on a read-only region. The bytes a
checkpoint wrote are `slots_written * block_size`, which is the write amplification against the bytes
the application dirtied. A redirty ratio near one under a steady hard watermark means the dirty
budget cannot hold the write working set.

## Logging

```cpp
#include <privateer/logger.hpp>

void set_default_log_min_level(log_level lvl) noexcept;
```

Messages go through metall's C logger hook, `metall_log`. An application that defines that symbol
receives the engine's messages in its own sink, and the built-in weak default prints to stderr with
`set_default_log_min_level` as its threshold. The fault handler never logs; it only sets the error
flag.

## Lower layers

These are used by a region and are public because tools need them:

* `privateer/block_hash.hpp`: `hash_algorithm`, `block_digest`, and hashing of a block. The algorithm
  id is part of the on-disk format, and a weak digest cannot corrupt data because publication
  byte-compares whenever a name already exists.
* `privateer/block_store.hpp`: the content-addressed store. Publication stages a file and links it
  under the content name, so it is thread-safe with no store state and no lock; a duplicate is
  answered by comparing the existing file and writes nothing.
* `privateer/recipe.hpp`: the on-disk recipe. The entries live in content-named segment files in the
  block store, one per fixed slot range, and the little-endian manifest `_recipe` names them; the
  manifest is replaced by rename at every commit, so a commit writes the segments whose entries
  changed plus the manifest. A version 1 recipe, one file with every entry in it, still loads.
* `privateer/vm.hpp`: the reservation and the fixed-address mapping calls, plus `page_size()`.
* `privateer/fault_handler.hpp`: installing the process-wide handler, and arming a thread with an
  mlocked alternate signal stack.
* `privateer/region_registry.hpp`: the lookup the handler uses to find the region a fault belongs to.
* `privateer/version.hpp`: `version()`, the library version as a string.
