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

#include <privateer/block_hash.hpp>
#include <privateer/error.hpp>

#include <cstddef>
#include <filesystem>
#include <span>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace privateer {

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
		// the name (dedup). A different file under the name is a fatal
		// hash_collision error. Thread-safe.
		result<bool> publish(block_digest const &name, std::span<std::byte const> data) const;

		// path of the block file for a name, existing or not
		[[nodiscard]] std::filesystem::path block_path(block_digest const &name) const;

		// The durability barrier for a set of published names: syncs the file
		// content and the shard directory entry of every name not yet in the
		// durable-name set, then records the names. Nothing is recorded when
		// any sync fails.
		result<> make_durable(std::span<block_digest const> names);

		[[nodiscard]] bool is_durable(block_digest const &name) const;

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
		// only garbage remains.
		void reclaim();

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

		std::filesystem::path blocks_dir_;
		boost::unordered_flat_set<block_digest, block_digest_hash> durable_;
		boost::unordered_flat_map<block_digest, uint32_t, block_digest_hash> refcounts_;
		boost::unordered_flat_set<block_digest, block_digest_hash> candidates_;
	};

}  // namespace privateer

#endif  // PRIVATEER_BLOCK_STORE_HPP
