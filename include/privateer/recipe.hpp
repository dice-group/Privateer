#ifndef PRIVATEER_RECIPE_HPP
#define PRIVATEER_RECIPE_HPP

// The recipe maps every slot of a region to the content-named block that
// backs it, or to the empty sentinel. On disk it is one little-endian
// binary file, <segment>/_recipe, replaced atomically by rename at each
// commit: a header (magic, format version, hash algorithm id, block_size,
// capacity, size, slot count, header checksum), one 32 byte entry per slot
// (raw digest bytes zero padded, all zero for an empty slot), and a
// trailing checksum over the entries. A torn or corrupt file fails to
// load, and so does an intact one whose header this build cannot serve: a
// newer format version, or an unknown hash algorithm id. block_size and
// the hash algorithm are datastore constants adopted from the header.

#include <privateer/block_hash.hpp>
#include <privateer/block_store.hpp>
#include <privateer/error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace privateer {

	inline constexpr char recipe_file_name[] = "_recipe";
	inline constexpr uint32_t recipe_format_version = 1;

	struct recipe {
		uint64_t block_size = 0;
		uint64_t capacity = 0;  // maximum region size in bytes
		uint64_t size = 0;      // extended region size in bytes, a multiple of block_size
		hash_algorithm algorithm = hash_algorithm::xxh3_128;

		// one entry per slot; a digest of size 0 is the empty sentinel
		std::vector<block_digest> entries;

		// fails with invalid_argument when the fields are inconsistent:
		// block_size zero, size not slot-aligned, size beyond capacity, an
		// entry count that does not match size, or an entry whose width is
		// neither the algorithm's digest size nor the sentinel
		[[nodiscard]] result<std::vector<std::byte>> serialize() const;

		static result<recipe> deserialize(std::span<std::byte const> bytes);

		// Atomic publication as <segment>/_recipe: staged write, rename.
		// durable syncs the file content before the rename and the segment
		// directory after it; a non-durable commit syncs nothing.
		result<> commit(std::filesystem::path const &segment_dir, bool durable) const;

		static result<recipe> load(std::filesystem::path const &segment_dir);
	};

	// open validation: every referenced block file exists and is exactly
	// block_size bytes
	result<> validate_blocks(recipe const &rec, block_store const &store);

	// deep open validation: every referenced block file re-hashes to its
	// recipe name; one full read pass over the unique referenced blocks
	result<> deep_verify_blocks(recipe const &rec, block_store const &store);

}  // namespace privateer

#endif  // PRIVATEER_RECIPE_HPP
