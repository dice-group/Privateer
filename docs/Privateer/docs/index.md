# Privateer

Privateer is versioned segment storage for memory-mapped data. It is the storage engine behind a
memory-mapped datastore: the application sees one contiguous address range and writes into it with
ordinary stores, and the engine turns those writes into block files on disk that a later run maps
back.

The source is at [dice-group/Privateer](https://github.com/dice-group/Privateer). This documentation
describes version 0.2, which is a rewrite for use as a segment-storage backend of
[metall](https://github.com/LLNL/metall). Version 0.1 was developed at Lawrence Livermore National
Laboratory and is at [LLNL/Privateer](https://github.com/LLNL/Privateer); the two share no code and
have different APIs.

## Overview

A datastore is a directory that holds fixed-size block files and a recipe. The recipe says which
block belongs at which offset, and it is the only thing that has to be replaced atomically for the
datastore to move from one version to the next.

Three properties follow from that layout:

- A checkpoint writes the blocks that changed, not the whole segment.
- A block is named by a digest of its content, so equal content is one file, and content that did
  not change is not written again.
- A snapshot is another recipe over the same block files, so it costs link counts instead of a copy.

## The model

A region is one virtual-memory reservation whose base address does not move while the region is
open:

```
[ segment header | slot 0 | slot 1 | ... ]
```

The header is anonymous memory that is never persisted, so it is the right place for volatile state.
Each slot is `block_size` bytes and maps its recipe entry: a block file read-only, or anonymous
zeros for an empty slot. Slots past the extended size stay unmapped, which is what makes a large
capacity cheap.

### The write barrier

A read-write open registers the region with the process-wide fault handler. Slots are mapped
read-only, so the first write into a slot traps. The handler claims the slot, makes it writable,
counts it dirty, and the retried store lands; further writes into that slot are native speed. Making
a slot writable is a protection change and not a copy, because the kernel supplies the previous
content from the page cache of the block file as pages are written.

A read-only open registers nothing. A stray write into a read-only region is a real error and
crashes.

### Commit

A commit captures every dirty slot, freezes it, and hashes it. What happens next depends on the
hash:

- equal to the recipe entry: nothing is written, the slot is clean again,
- new, but a block file with that name exists: the content is compared against that file, and
  nothing is written,
- new: the block is written into the store under its content name.

Then the new recipe replaces the old one with an atomic rename. A durable commit adds a durability
barrier before the rename and reclaims retired block files after it, so it returns only when
everything the new recipe references is on stable storage.

One commit runs at a time. Readers stay live for the whole commit, and a writer that faults a
captured slot waits for that slot's own write-out and nothing else. A consistent cut still needs the
application to hold its writes, because a commit cannot know which of them belong together.

### Snapshots and reclaim

`snapshot_to` runs a durable commit and stages a self-contained copy of the result: every referenced
block file is hard-linked, with a per-file copy where the link is refused, and the recipe copy is
written and synced. Both steps hold the commit mutex, so no commit in between can reclaim a block
the staged copy references. Publishing the staged directory is one atomic rename, which the caller
does.

Block files are reference counted against the recipes that use them. A file that nothing references
any more is deleted by the durable commit that retired it, and an open-time sweep removes whatever a
crash left behind.

### Memory

Two mechanisms bound memory, and only the first one is for a write phase:

- **Background write-back with a dirty budget.** The cleaner writes dirty slots back ahead of
  commits, cold slots first. A dirty-byte budget sets the rate: above the soft watermark the cleaner
  runs until dirty bytes are back at the low watermark, and a write fault that would cross the hard
  watermark waits for the drain. The resident size of a bulk load settles near the soft watermark.
- **The resident sweep.** Linux only and advisory. It pushes resident pages of clean and empty slots
  out with `MADV_PAGEOUT`. It suits a process that writes without serving reads, because concurrent
  readers fault back what the sweep pushes out.

`region::statistics()` reports what write-out did (slots hashed, skipped, deduplicated, written)
plus write-back and stall counters, which is how write amplification and the thrash regime become
observable from outside the engine.
