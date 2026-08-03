// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_RECIPE_HPP
#define PRIVATEER_RECIPE_HPP

// The recipe maps every slot of a region to the content-named block that
// backs it, or to the empty sentinel. It lives in two parts, both little
// endian.
//
// The manifest is one file, <segment>/_recipe, replaced atomically by
// rename at each commit: a 72 byte header (magic, format version, hash
// algorithm id, entry width, log2 of the slots per segment, block_size,
// capacity, size, slot count, header checksum, segment record count,
// dictionary chunk count), one 24 byte record per segment (encoding,
// segment file length, segment file digest), and a checksum over the
// record table. block_size, the hash algorithm and the slots per segment
// are datastore constants adopted from the header.
//
// The entries themselves live in segment files, one per fixed slot range
// of the region. A segment file is a block-store file named by the hash of
// its whole content: a framed header (magic, encoding, base slot, entry
// count), one entry per slot holding the raw digest bytes, all zero for an
// empty slot, and a trailing checksum. A segment whose slots are all empty
// has no file at all, only an ALL_EMPTY record. So a commit writes the
// segments whose entries changed plus the manifest, an unchanged segment
// dedups against the file the last commit published, and the store's
// reference counting, durability barrier and sweep cover segment files the
// same way they cover data blocks.
//
// A torn or corrupt manifest or segment fails to load, and so does an
// intact one this build cannot serve: a newer format version, an unknown
// hash algorithm id, an unknown segment encoding, or a nonzero dictionary
// chunk count.
//
// The entry is exactly as wide as the algorithm's digest, 16 bytes under
// xxh3-128, and the manifest records that width. So the files cost nothing
// for digest room they do not use, and an algorithm with a wider digest is
// a new algorithm id rather than a new format version.
//
// Format version 1 is a single file: the same first 56 header bytes, one
// entry per slot, and a checksum over the entries. It is frozen and read
// only. load still reads it, so a datastore an older build wrote opens,
// and its first commit writes version 2.

#include <privateer/block_hash.hpp>
#include <privateer/block_store.hpp>
#include <privateer/error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace privateer {

	inline constexpr char recipe_file_name[] = "_recipe";
	inline constexpr uint32_t recipe_format_version = 2;

	// Slots one segment covers, as the log2 the manifest header records. A
	// changed entry rewrites its whole segment, so the size trades that
	// write amplification (256 KiB of entries under xxh3-128) against the
	// number of records in the manifest.
	inline constexpr uint8_t recipe_segment_log2 = 14;
	inline constexpr uint64_t recipe_segment_slots = uint64_t{1} << recipe_segment_log2;

	// how a segment holds the entries of its slot range
	enum struct segment_encoding : uint8_t {
		all_empty = 0,  // every slot is the empty sentinel, and no file exists
		raw = 1,        // one raw digest per slot, in the segment file
	};

	// One manifest record. The slot range follows from the record's
	// position: record i covers [i * S, min((i + 1) * S, slot count)) with
	// S the slots per segment of the manifest header.
	struct segment_record {
		segment_encoding encoding = segment_encoding::all_empty;
		uint32_t byte_length = 0;  // length of the segment file, 0 for all_empty
		block_digest digest{};     // name of the segment file, the sentinel for all_empty
	};

	// segments a slot count needs
	[[nodiscard]] constexpr uint64_t segment_count(uint64_t slot_count) noexcept {
		return slot_count / recipe_segment_slots + (slot_count % recipe_segment_slots != 0 ? 1 : 0);
	}

	struct recipe {
		uint64_t block_size = 0;
		uint64_t capacity = 0;  // maximum region size in bytes
		uint64_t size = 0;      // extended region size in bytes, a multiple of block_size
		hash_algorithm algorithm = hash_algorithm::xxh3_128;

		// one entry per slot; a digest of size 0 is the empty sentinel
		std::vector<block_digest> entries;

		// The manifest records of the last load or commit, in slot order.
		// They name the segment files the store holds for this recipe, so a
		// caller that keeps store bookkeeping references them like blocks.
		// Empty after a version 1 load, which has no segment files.
		std::vector<segment_record> segments;

		// The frozen version 1 encoding: the whole entry table in one
		// buffer. Fails with invalid_argument when the fields are
		// inconsistent: block_size zero, size not slot-aligned, size beyond
		// capacity, an entry count that does not match size, or an entry
		// whose width is neither the algorithm's digest size nor the
		// sentinel.
		[[nodiscard]] result<std::vector<std::byte>> serialize() const;

		// Reads a version 1 buffer. A version 2 manifest needs its segment
		// files, so it is unsupported here; load is what reads it.
		static result<recipe> deserialize(std::span<std::byte const> bytes);

		// Publishes the recipe: every segment file into the store, then the
		// manifest as <segment>/_recipe through a staged write and a rename,
		// which is the atomic commit point. Every published name is noted as
		// unsynced in the store, so a later barrier over the pending names
		// covers it. durable syncs the published segment files and their
		// directory entries, then the manifest content before the rename and
		// the segment directory after it; a non-durable commit syncs
		// nothing.
		//
		// Returns the manifest records it wrote, in slot order. The store
		// counts references but does not know which of its names a recipe
		// holds, so the caller installs the records as segments and
		// reconciles the references of the set it replaces.
		[[nodiscard]] result<std::vector<segment_record>> commit(std::filesystem::path const &segment_dir,
																 block_store &store, bool durable) const;

		// Reads <segment>/_recipe and every segment file it names. A version
		// 1 file carries the entries itself; the store stays unused then and
		// segments stays empty.
		static result<recipe> load(std::filesystem::path const &segment_dir, block_store const &store);
	};

	// Framed bytes of the segment at index, or nothing when every slot of its
	// range holds the empty sentinel, which is what an all_empty record
	// stands for. The bytes are content addressed: their hash names the
	// segment file.
	[[nodiscard]] result<std::optional<std::vector<std::byte>>> encode_segment(recipe const &rec, size_t index);

	// Checks one framed segment against its record (length, magic, encoding,
	// base slot, entry count, trailing checksum, and the content hash
	// against the record digest) and writes its entries into out. out is the
	// slot range of the segment, so its length is the entry count the frame
	// must carry and base_slot its first slot.
	[[nodiscard]] result<> decode_segment(std::span<std::byte const> bytes, segment_record const &record,
										  uint64_t base_slot, hash_algorithm algorithm,
										  std::span<block_digest> out);

	// open validation: every referenced block file exists and is exactly
	// block_size bytes
	result<> validate_blocks(recipe const &rec, block_store const &store);

	// deep open validation: every referenced block file re-hashes to its
	// recipe name; one full read pass over the unique referenced blocks
	result<> deep_verify_blocks(recipe const &rec, block_store const &store);

}  // namespace privateer

#endif  // PRIVATEER_RECIPE_HPP
