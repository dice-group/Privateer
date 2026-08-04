// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_BLOCK_STORE_HPP
#define PRIVATEER_BLOCK_STORE_HPP

// Content-addressed store of immutable block files under
// <segment>/blocks/<shard>/<name>. The name is the lowercase hex of the
// content digest, the shard is the hex of its first byte. All 256 shard
// directories are created at store creation and made durable there, so
// publication never creates directories.
//
// publish is thread-safe: it touches no store state, and the atomic link
// step resolves races on one name (the losers byte-compare against the
// winner's file). Everything else (durable-name set, references, reclaim,
// sweep) is bookkeeping owned by one caller at a time; the engine
// serializes it under its commit mutex.
//
// The durable-name set holds every name whose file content was synced and
// whose directory entry was synced; both are required before a committed
// recipe may reference the name. Names leave the set when their file is
// unlinked. At open the caller seeds the set with the names the verified
// recipe references.
//
// The pending-name set holds the names published in this run that no
// successful durability barrier covers yet, so a barrier costs what was
// published since the last one instead of a pass over the whole recipe. It
// is a superset of the names the in-memory recipe references minus the
// durable names: every publish site notes its name, and the pruning after a
// barrier removes only names that became durable. A name retired before it
// was ever synced stays pending, because dedup can reference it again.
//
// A name whose durability barrier failed is distrusted. The failed sync
// dropped the file's dirty pages and reported the error only to the open
// file the sync ran on, so a barrier through a fresh descriptor over the
// same file reports success and proves nothing. A distrusted name is never a
// dedup target and never becomes durable; the way back is a rewrite through
// a fresh file, which publish performs and which the next barrier covers.

#include <privateer/block_hash.hpp>
#include <privateer/error.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#ifdef PRIVATEER_TEST_HOOKS
#include <atomic>
#endif

namespace privateer {

#ifdef PRIVATEER_TEST_HOOKS
	// Test-only hooks, compiled in when the build includes the tests.
	namespace detail_block_store {

		// When set, decides whether the durability barrier's file sync for
		// this name fails, the way fdatasync on a device error would. Tests
		// use it to exercise the barrier's failure path. Atomic because a
		// barrier wave reads it from the fan-out's threads.
		extern std::atomic<bool (*)(block_digest const &name)> barrier_sync_fails_fn;

	}  // namespace detail_block_store
#endif  // PRIVATEER_TEST_HOOKS

	struct block_store {
		// Creates <segment_dir>/blocks with all shard directories. durable
		// syncs the skeleton up front, so commits never create directories;
		// snapshot staging skips the syncs because metall fsyncs the whole
		// staged tree before publishing it.
		static result<block_store> create(std::filesystem::path const &segment_dir, bool durable = true);

		// opens an existing store; every shard directory must be present
		static result<block_store> open(std::filesystem::path const &segment_dir);

		block_store(block_store &&) = default;
		block_store &operator=(block_store &&) = default;
		block_store(block_store const &) = delete;
		block_store &operator=(block_store const &) = delete;

		// Publishes data under its digest, atomically. Returns true when a
		// new file was created, false when an identical file already carried
		// the name (dedup). A name that already has a file is answered by
		// comparing that file, so a duplicate writes nothing. A different
		// file under the name is a fatal hash_collision error. A distrusted
		// name skips the compare and replaces its file. Thread-safe.
		result<bool> publish(block_digest const &name, std::span<std::byte const> data) const;

		// path of the block file for a name, existing or not
		[[nodiscard]] std::filesystem::path block_path(block_digest const &name) const;

		// How the store spreads independent sync calls. body(index) is called
		// once for every index below count, in any order and possibly on other
		// threads, and the call returns only after all of them finished; body
		// never throws. The store's own bookkeeping stays outside the fan-out,
		// so it keeps a single owner. Passing none runs every index on the
		// calling thread, which is also what a small batch does: syncs of a
		// device are what parallelizes, and a batch of a few costs more to
		// spread than to run.
		using sync_fan_out = std::function<void(size_t count, std::function<void(size_t)> const &body)>;

		// The durability barrier for a set of published names: syncs the file
		// content and the shard directory entry of every name not yet in the
		// durable-name set, then records the names. Nothing is recorded when
		// any sync fails; the names the failed wave covered are distrusted
		// instead, and the call fails outright when one of the names it is
		// asked to cover is distrusted already. The file syncs and the
		// directory syncs of one call are independent and may run in one
		// wave: a crash between them leaves a block file that no committed
		// recipe names, which the open-time sweep removes.
		result<> make_durable(std::span<block_digest const> names, sync_fan_out const &fan_out = {});

		// Records a published name as one that owes a sync. A name already in
		// the durable-name set is not recorded. Bookkeeping owned by one
		// caller at a time, the same rule as add_reference and
		// drop_reference; the engine serializes it under its commit mutex.
		void note_unsynced(block_digest const &name);

		// The durability barrier over the pending names: syncs every pending
		// name that a recipe entry references and that is not durable yet.
		// A pending name nothing references is skipped, because its file may
		// already be unlinked and syncing it would fail for nothing; it stays
		// pending, so a later reference through dedup is covered by the next
		// barrier. On success the pending set keeps exactly the names that
		// are still not durable. A failure leaves the pending set untouched
		// and reports the error.
		result<> make_pending_durable(sync_fan_out const &fan_out = {});

		[[nodiscard]] bool is_durable(block_digest const &name) const;

		// Whether the name's file must be written again before a recipe may
		// reference it durably: a barrier failed on it and no rewrite
		// followed. publish rewrites such a name instead of deduping against
		// its file, and a caller that would skip the publish because the
		// content already carries the name asks this first. Reading it beside
		// running publishes is safe: only the barrier and the unlink paths
		// change it, and the engine serializes those under its commit mutex.
		[[nodiscard]] bool needs_rewrite(block_digest const &name) const noexcept;

		// Ends the distrust of a name whose file a publish just rewrote.
		// Bookkeeping owned by one caller at a time, the same rule as
		// note_unsynced.
		void note_rewritten(block_digest const &name);

		// records a name as durable without syncing: open-time seeding, the
		// verified consistency mark certifies the referenced blocks
		void seed_durable(block_digest const &name);

		// Reference bookkeeping: a name's count is the number of slots whose
		// recipe entry carries it. The count reaching zero makes the name an
		// unlink candidate; re-referencing it withdraws the candidate.
		void add_reference(block_digest const &name);
		void drop_reference(block_digest const &name);

		[[nodiscard]] bool referenced(block_digest const &name) const;

		// Unlinks the candidate files and drops them from the durable-name
		// set. Errors are logged and never fatal: a failed name stays a
		// candidate for the next pass, the commit is already durable and
		// only garbage remains. The fan-out spreads the pass by shard, since
		// each shard owns its unlinks and its one directory sync.
		void reclaim(sync_fan_out const &fan_out = {});

		// Unlinks an unreferenced name's file right away instead of leaving
		// it to reclaim. For unwind paths that must not leave the file
		// behind as a dedup target: after a failed durability barrier a
		// re-synced file cannot be trusted, so a retry must rewrite through
		// a fresh file. The caller guarantees the on-disk recipe does not
		// reference the name (it names a file created after the last recipe
		// commit). A name that still has references is left alone.
		void discard_unreferenced(block_digest const &name);

		// Open-time sweep: unlinks every file in the store whose name is not
		// in referenced, including temp leftovers. Returns the number of
		// files removed. Read-write opens only; the caller has verified the
		// consistency mark, so the recipe on disk is the last durable one and
		// nothing the sweep removes can be needed again.
		result<size_t> sweep(std::span<block_digest const> referenced) const;

	private:
		explicit block_store(std::filesystem::path blocks_dir) : blocks_dir_{std::move(blocks_dir)} {}

		[[nodiscard]] std::filesystem::path shard_path(block_digest const &name) const;

		// records the names of a failed barrier wave as distrusted
		void distrust(std::span<block_digest const> names) noexcept;

		std::filesystem::path blocks_dir_;
		boost::unordered_flat_set<block_digest, block_digest_hash> durable_;
		boost::unordered_flat_set<block_digest, block_digest_hash> pending_;
		boost::unordered_flat_map<block_digest, uint32_t, block_digest_hash> refcounts_;
		boost::unordered_flat_set<block_digest, block_digest_hash> candidates_;
		boost::unordered_flat_set<block_digest, block_digest_hash> distrusted_;
		// Set when a failed barrier could not record its names. From then on
		// every barrier fails: the store can no longer tell which of its
		// files a barrier already failed on.
		bool distrust_all_ = false;
	};

}  // namespace privateer

#endif  // PRIVATEER_BLOCK_STORE_HPP
