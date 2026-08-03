// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/recipe.hpp>

#include <privateer/file_util.hpp>

#include <algorithm>
#include <cstring>
#include <new>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace privateer {

	namespace fs = std::filesystem;

	namespace {

		constexpr char magic[8] = {'P', 'V', 'R', 'E', 'C', 'I', 'P', 'E'};
		constexpr uint32_t legacy_format_version = 1;  // the single-file recipe
		constexpr size_t header_size = 56;             // includes the header checksum
		constexpr size_t header_checksum_offset = 48;  // checksum covers the bytes before it
		constexpr size_t entry_width_offset = 13;      // one byte, next to the algorithm id
		constexpr size_t min_file_size = header_size + sizeof(uint64_t);

		// The manifest: the version 1 header, then the segment record table.
		constexpr size_t segment_log2_offset = 14;
		constexpr size_t record_count_offset = 56;      // the table checksum covers from here
		constexpr size_t dictionary_count_offset = 64;  // reserved, always zero
		constexpr size_t manifest_header_size = 72;
		constexpr size_t manifest_min_size = manifest_header_size + sizeof(uint64_t);
		constexpr size_t record_size = 24;
		constexpr size_t record_flags_offset = 1;
		constexpr size_t record_length_offset = 4;
		constexpr size_t record_digest_offset = 8;
		constexpr size_t record_digest_room = 16;  // digest bytes one record has room for
		// The widest slots-per-segment a manifest may name. The entries of one
		// segment are read into memory as one span, so the log2 bounds that
		// buffer instead of leaving the file to name any size it likes.
		constexpr unsigned segment_log2_max = 24;

		// The segment file: the frame, then the entries, then the checksum.
		constexpr char segment_magic[4] = {'P', 'V', 'S', 'G'};
		constexpr size_t segment_encoding_offset = 4;
		constexpr size_t segment_base_offset = 8;
		constexpr size_t segment_count_offset = 16;
		constexpr size_t segment_header_size = 24;

		void store_le32(std::byte *out, uint32_t value) noexcept {
			for (int i = 0; i < 4; ++i) {
				out[i] = static_cast<std::byte>(value >> (8 * i));
			}
		}

		void store_le64(std::byte *out, uint64_t value) noexcept {
			for (int i = 0; i < 8; ++i) {
				out[i] = static_cast<std::byte>(value >> (8 * i));
			}
		}

		uint32_t load_le32(std::byte const *in) noexcept {
			uint32_t value = 0;
			for (int i = 0; i < 4; ++i) {
				value |= static_cast<uint32_t>(in[i]) << (8 * i);
			}
			return value;
		}

		uint64_t load_le64(std::byte const *in) noexcept {
			uint64_t value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(in[i]) << (8 * i);
			}
			return value;
		}

		bool all_zero(std::byte const *bytes, size_t len) noexcept {
			return std::all_of(bytes, bytes + len, [](std::byte b) { return b == std::byte{0}; });
		}

		// The field invariants every encoding of a recipe needs, and the entry
		// width they imply. One place, so the version 1 buffer and the
		// segments reject the same recipe with the same message.
		result<size_t> checked_fields(recipe const &rec) {
			if (rec.block_size == 0) {
				return fail(errc::invalid_argument, "recipe block_size is zero");
			}
			if (rec.size % rec.block_size != 0) {
				return fail(errc::invalid_argument, "recipe size is not slot-aligned");
			}
			if (rec.size > rec.capacity) {
				return fail(errc::invalid_argument, "recipe size exceeds capacity");
			}
			if (rec.entries.size() != rec.size / rec.block_size) {
				return fail(errc::invalid_argument, "recipe entry count does not match size");
			}
			size_t const expected_digest = digest_size(rec.algorithm);
			if (expected_digest == 0) {
				// symmetric with the read path: a recipe never names an
				// algorithm this build cannot compute, so no commit can reach
				// hash_block with an id it has no implementation for
				return fail(errc::recipe_unsupported, "recipe hash algorithm unknown to the build");
			}
			// The entry is exactly as wide as the digest, and the header
			// carries that width, so an algorithm with a wider digest needs a
			// new id and not a new format version.
			return expected_digest;
		}

		// The entries of one range against that width. Every writer checks the
		// range it encodes, so no writer walks the whole recipe for a slice of
		// it.
		result<> checked_entries(std::span<block_digest const> entries, size_t width) {
			for (auto const &entry : entries) {
				if (entry.size != 0 && entry.size != width) {
					return fail(errc::invalid_argument, "recipe entry width does not match the algorithm");
				}
			}
			return {};
		}

		// Whole content of one file. A file that shrinks under the read yields
		// the bytes that were there, and the caller's length checks report the
		// truncation. An absent file gets missing_code, which says what the
		// gap means to the caller.
		result<std::vector<std::byte>> read_whole_file(fs::path const &path, errc missing_code,
													   char const *missing_context, char const *io_context) {
			int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				if (errno == ENOENT) {
					return fail(missing_code, missing_context);
				}
				return fail_errno(errc::io_error, io_context);
			}
			struct stat st {};
			if (::fstat(fd, &st) != 0) {
				auto const err = fail_errno(errc::io_error, io_context);
				::close(fd);
				return err;
			}
			std::vector<std::byte> bytes(static_cast<size_t>(st.st_size));
			size_t offset = 0;
			while (offset < bytes.size()) {
				ssize_t const got = ::pread(fd, bytes.data() + offset, bytes.size() - offset,
											static_cast<off_t>(offset));
				if (got < 0) {
					if (errno == EINTR) {
						continue;
					}
					auto const err = fail_errno(errc::io_error, io_context);
					::close(fd);
					return err;
				}
				if (got == 0) {
					break;  // shrunk under us; the length checks report it
				}
				offset += static_cast<size_t>(got);
			}
			::close(fd);
			bytes.resize(offset);
			return bytes;
		}

		// Atomic publication of the manifest as <segment>/_recipe: staged
		// write, rename. durable syncs the content before the rename and the
		// segment directory after it.
		result<> commit_manifest(std::span<std::byte const> bytes, fs::path const &segment_dir, bool durable) {
			auto staged = staged_file::create_in(segment_dir);
			if (!staged) {
				return std::unexpected{staged.error()};
			}
			if (auto written = staged->write(bytes); !written) {
				return std::unexpected{written.error()};
			}
			if (durable) {
				if (auto synced = staged->sync(); !synced) {
					return std::unexpected{synced.error()};
				}
			}
			auto published = staged->publish(recipe_file_name, publish_mode::replace);
			if (!published) {
				return std::unexpected{published.error()};
			}
			if (durable) {
				return sync_directory(segment_dir);
			}
			return {};
		}

		// The manifest bytes over a record table the caller already built.
		result<std::vector<std::byte>> encode_manifest(recipe const &rec, size_t entry_width,
													   std::span<segment_record const> records) {
			std::vector<std::byte> bytes(manifest_header_size + records.size() * record_size + sizeof(uint64_t));
			std::memcpy(bytes.data(), magic, sizeof(magic));
			store_le32(bytes.data() + 8, recipe_format_version);
			bytes[12] = static_cast<std::byte>(rec.algorithm);
			bytes[entry_width_offset] = static_cast<std::byte>(entry_width);
			bytes[segment_log2_offset] = static_cast<std::byte>(recipe_segment_log2);
			store_le64(bytes.data() + 16, rec.block_size);
			store_le64(bytes.data() + 24, rec.capacity);
			store_le64(bytes.data() + 32, rec.size);
			store_le64(bytes.data() + 40, rec.entries.size());
			store_le64(bytes.data() + header_checksum_offset,
					   checksum64({bytes.data(), header_checksum_offset}));
			store_le64(bytes.data() + record_count_offset, records.size());
			// the dictionary chunk count stays zero: this build encodes none

			std::byte *out = bytes.data() + manifest_header_size;
			for (auto const &record : records) {
				out[0] = static_cast<std::byte>(record.encoding);
				store_le32(out + record_length_offset, record.byte_length);
				// the sentinel digest of an all_empty record is all zero, and
				// so is its bytes array
				std::memcpy(out + record_digest_offset, record.digest.bytes.data(), entry_width);
				out += record_size;
			}
			store_le64(out, checksum64({bytes.data() + record_count_offset,
										bytes.size() - record_count_offset - sizeof(uint64_t)}));
			return bytes;
		}

	}  // namespace

	result<std::vector<std::byte>> recipe::serialize() const {
		auto const width = checked_fields(*this);
		if (!width) {
			return std::unexpected{width.error()};
		}
		if (auto checked = checked_entries(entries, *width); !checked) {
			return std::unexpected{checked.error()};
		}
		size_t const entry_size = *width;

		std::vector<std::byte> bytes(header_size + entries.size() * entry_size + sizeof(uint64_t));
		std::memcpy(bytes.data(), magic, sizeof(magic));
		store_le32(bytes.data() + 8, legacy_format_version);
		bytes[12] = static_cast<std::byte>(algorithm);
		bytes[entry_width_offset] = static_cast<std::byte>(entry_size);
		store_le64(bytes.data() + 16, block_size);
		store_le64(bytes.data() + 24, capacity);
		store_le64(bytes.data() + 32, size);
		store_le64(bytes.data() + 40, entries.size());
		store_le64(bytes.data() + header_checksum_offset,
				   checksum64({bytes.data(), header_checksum_offset}));

		std::byte *out = bytes.data() + header_size;
		for (auto const &entry : entries) {
			// a sentinel entry is all zero, and its bytes array is too
			std::memcpy(out, entry.bytes.data(), entry_size);
			out += entry_size;
		}
		store_le64(out, checksum64({bytes.data() + header_size, entries.size() * entry_size}));
		return bytes;
	}

	result<recipe> recipe::deserialize(std::span<std::byte const> bytes) {
		if (bytes.size() < min_file_size) {
			return fail(errc::recipe_corrupt, "recipe truncated");
		}
		if (std::memcmp(bytes.data(), magic, sizeof(magic)) != 0) {
			return fail(errc::recipe_corrupt, "recipe magic mismatch");
		}
		if (load_le64(bytes.data() + header_checksum_offset) !=
			checksum64({bytes.data(), header_checksum_offset})) {
			return fail(errc::recipe_corrupt, "recipe header checksum mismatch");
		}
		uint32_t const version = load_le32(bytes.data() + 8);
		if (version > recipe_format_version) {
			return fail(errc::recipe_unsupported, "recipe format version newer than the build");
		}
		if (version == 0) {
			return fail(errc::recipe_corrupt, "recipe format version invalid");
		}
		if (version != legacy_format_version) {
			// the manifest names segment files, so it needs the store
			return fail(errc::recipe_unsupported, "a version 2 recipe loads through the block store");
		}
		auto const algorithm = static_cast<hash_algorithm>(bytes[12]);
		size_t const expected_digest = digest_size(algorithm);
		if (expected_digest == 0) {
			return fail(errc::recipe_unsupported, "recipe hash algorithm unknown to the build");
		}
		// A recipe written before the width entered the header carries a zero
		// here, which this rejects rather than reading at the wrong stride.
		size_t const entry_size = static_cast<size_t>(bytes[entry_width_offset]);
		if (entry_size == 0 || entry_size > max_digest_size) {
			return fail(errc::recipe_corrupt, "recipe entry width invalid");
		}
		if (entry_size != expected_digest) {
			return fail(errc::recipe_corrupt, "recipe entry width does not match the algorithm");
		}

		recipe rec;
		rec.algorithm = algorithm;
		rec.block_size = load_le64(bytes.data() + 16);
		rec.capacity = load_le64(bytes.data() + 24);
		rec.size = load_le64(bytes.data() + 32);
		uint64_t const slot_count = load_le64(bytes.data() + 40);
		if (rec.block_size == 0 || rec.size % rec.block_size != 0 || rec.size > rec.capacity ||
			slot_count != rec.size / rec.block_size) {
			return fail(errc::recipe_corrupt, "recipe header fields inconsistent");
		}
		if (bytes.size() != header_size + slot_count * entry_size + sizeof(uint64_t)) {
			return fail(errc::recipe_corrupt, "recipe length does not match the slot count");
		}
		std::byte const *in = bytes.data() + header_size;
		if (load_le64(in + slot_count * entry_size) != checksum64({in, slot_count * entry_size})) {
			return fail(errc::recipe_corrupt, "recipe entries checksum mismatch");
		}

		rec.entries.resize(slot_count);
		for (auto &entry : rec.entries) {
			// An entry is exactly the digest, so there is no padding to check;
			// all zero is the empty sentinel. The rest of bytes stays zero
			// from the default construction, which is what makes two digests
			// of one algorithm comparable over the whole array.
			if (!all_zero(in, entry_size)) {
				std::memcpy(entry.bytes.data(), in, entry_size);
				entry.size = static_cast<uint8_t>(entry_size);
			}
			in += entry_size;
		}
		return rec;
	}

	result<std::optional<std::vector<std::byte>>> encode_segment(recipe const &rec, size_t index) {
		auto const width = checked_fields(rec);
		if (!width) {
			return std::unexpected{width.error()};
		}
		uint64_t const slot_count = rec.entries.size();
		uint64_t const base = static_cast<uint64_t>(index) * recipe_segment_slots;
		if (base >= slot_count) {
			return fail(errc::invalid_argument, "recipe segment index is beyond the slot count");
		}
		size_t const count = static_cast<size_t>(std::min<uint64_t>(recipe_segment_slots, slot_count - base));
		std::span<block_digest const> const slots{rec.entries.data() + base, count};
		if (auto checked = checked_entries(slots, *width); !checked) {
			return std::unexpected{checked.error()};
		}
		if (std::ranges::all_of(slots, [](block_digest const &entry) { return entry.size == 0; })) {
			return std::nullopt;  // an all_empty record, and no file
		}

		std::vector<std::byte> bytes(segment_header_size + count * *width + sizeof(uint64_t));
		std::memcpy(bytes.data(), segment_magic, sizeof(segment_magic));
		bytes[segment_encoding_offset] = static_cast<std::byte>(segment_encoding::raw);
		store_le64(bytes.data() + segment_base_offset, base);
		store_le64(bytes.data() + segment_count_offset, count);
		std::byte *out = bytes.data() + segment_header_size;
		for (auto const &entry : slots) {
			// a sentinel entry is all zero, and its bytes array is too
			std::memcpy(out, entry.bytes.data(), *width);
			out += *width;
		}
		size_t const payload_end = segment_header_size + count * *width;
		store_le64(out, checksum64({bytes.data(), payload_end}));
		return std::optional{std::move(bytes)};
	}

	result<> decode_segment(std::span<std::byte const> bytes, segment_record const &record, uint64_t base_slot,
							hash_algorithm algorithm, std::span<block_digest> out) {
		size_t const width = digest_size(algorithm);
		if (width == 0) {
			return fail(errc::recipe_unsupported, "recipe hash algorithm unknown to the build");
		}
		if (bytes.size() != record.byte_length) {
			return fail(errc::recipe_corrupt, "recipe segment length does not match its record");
		}
		if (bytes.size() != segment_header_size + out.size() * width + sizeof(uint64_t)) {
			return fail(errc::recipe_corrupt, "recipe segment length does not match its slot range");
		}
		if (std::memcmp(bytes.data(), segment_magic, sizeof(segment_magic)) != 0) {
			return fail(errc::recipe_corrupt, "recipe segment magic mismatch");
		}
		if (static_cast<segment_encoding>(bytes[segment_encoding_offset]) != record.encoding) {
			return fail(errc::recipe_corrupt, "recipe segment encoding disagrees with its record");
		}
		if (load_le64(bytes.data() + segment_base_offset) != base_slot) {
			return fail(errc::recipe_corrupt, "recipe segment base slot disagrees with its position");
		}
		if (load_le64(bytes.data() + segment_count_offset) != out.size()) {
			return fail(errc::recipe_corrupt, "recipe segment entry count disagrees with its slot range");
		}
		size_t const payload_end = segment_header_size + out.size() * width;
		if (load_le64(bytes.data() + payload_end) != checksum64({bytes.data(), payload_end})) {
			return fail(errc::recipe_corrupt, "recipe segment checksum mismatch");
		}
		// The name of the file is the hash of its content, so this is what
		// makes a segment read back the entries the commit wrote, whatever
		// happened to the file in between.
		if (hash_block(algorithm, bytes) != record.digest) {
			return fail(errc::recipe_corrupt, "recipe segment does not hash to its record digest");
		}

		std::byte const *in = bytes.data() + segment_header_size;
		for (auto &entry : out) {
			// An entry is exactly the digest, so there is no padding to check;
			// all zero is the empty sentinel. The rest of bytes stays zero
			// from the default construction, which is what makes two digests
			// of one algorithm comparable over the whole array.
			entry = block_digest{};
			if (!all_zero(in, width)) {
				std::memcpy(entry.bytes.data(), in, width);
				entry.size = static_cast<uint8_t>(width);
			}
			in += width;
		}
		return {};
	}

	result<std::vector<segment_record>> recipe::commit(fs::path const &segment_dir, block_store &store,
													   bool durable) const {
		auto const width = checked_fields(*this);
		if (!width) {
			return std::unexpected{width.error()};
		}
		if (*width > record_digest_room) {
			return fail(errc::recipe_unsupported, "recipe digest wider than a manifest record");
		}

		size_t const count = static_cast<size_t>(segment_count(entries.size()));
		std::vector<segment_record> records(count);
		std::vector<block_digest> published;
		published.reserve(count);
		for (size_t index = 0; index < count; ++index) {
			auto encoded = encode_segment(*this, index);
			if (!encoded) {
				return std::unexpected{encoded.error()};
			}
			if (!*encoded) {
				continue;  // the default record is all_empty
			}
			auto const name = hash_block(algorithm, **encoded);
			if (auto ok = store.publish(name, **encoded); !ok) {
				return std::unexpected{ok.error()};
			}
			// The name owes a sync: a file this commit created, or a dedup hit
			// on a file no barrier covers yet.
			store.note_unsynced(name);
			records[index] = segment_record{segment_encoding::raw,
											static_cast<uint32_t>((*encoded)->size()), name};
			published.push_back(name);
		}
		if (durable) {
			// The barrier over exactly the segment files of this recipe. The
			// pending names of the store are somebody else's phase.
			if (auto synced = store.make_durable(published); !synced) {
				return std::unexpected{synced.error()};
			}
		}
		auto manifest = encode_manifest(*this, *width, records);
		if (!manifest) {
			return std::unexpected{manifest.error()};
		}
		if (auto written = commit_manifest(*manifest, segment_dir, durable); !written) {
			return std::unexpected{written.error()};
		}
		return records;
	}

	namespace {

		// The version 2 read path: validate the manifest, then read and decode
		// every segment file it names.
		result<recipe> load_manifest(std::span<std::byte const> bytes, block_store const &store) {
			if (bytes.size() < manifest_min_size) {
				return fail(errc::recipe_corrupt, "recipe manifest truncated");
			}
			if (std::memcmp(bytes.data(), magic, sizeof(magic)) != 0) {
				return fail(errc::recipe_corrupt, "recipe magic mismatch");
			}
			if (load_le64(bytes.data() + header_checksum_offset) !=
				checksum64({bytes.data(), header_checksum_offset})) {
				return fail(errc::recipe_corrupt, "recipe header checksum mismatch");
			}
			if (load_le32(bytes.data() + 8) != recipe_format_version) {
				return fail(errc::recipe_corrupt, "recipe format version invalid");
			}
			auto const algorithm = static_cast<hash_algorithm>(bytes[12]);
			size_t const expected_digest = digest_size(algorithm);
			if (expected_digest == 0) {
				return fail(errc::recipe_unsupported, "recipe hash algorithm unknown to the build");
			}
			if (expected_digest > record_digest_room) {
				return fail(errc::recipe_unsupported, "recipe digest wider than a manifest record");
			}
			size_t const entry_width = static_cast<size_t>(bytes[entry_width_offset]);
			if (entry_width == 0 || entry_width > max_digest_size) {
				return fail(errc::recipe_corrupt, "recipe entry width invalid");
			}
			if (entry_width != expected_digest) {
				return fail(errc::recipe_corrupt, "recipe entry width does not match the algorithm");
			}
			unsigned const log2_slots = static_cast<unsigned>(bytes[segment_log2_offset]);
			if (log2_slots == 0 || log2_slots > segment_log2_max) {
				return fail(errc::recipe_corrupt, "recipe segment size invalid");
			}
			// Adopted from the header, like block_size: a datastore keeps the
			// segment size it was written with.
			uint64_t const slots_per_segment = uint64_t{1} << log2_slots;

			recipe rec;
			rec.algorithm = algorithm;
			rec.block_size = load_le64(bytes.data() + 16);
			rec.capacity = load_le64(bytes.data() + 24);
			rec.size = load_le64(bytes.data() + 32);
			uint64_t const slot_count = load_le64(bytes.data() + 40);
			if (rec.block_size == 0 || rec.size % rec.block_size != 0 || rec.size > rec.capacity ||
				slot_count != rec.size / rec.block_size) {
				return fail(errc::recipe_corrupt, "recipe header fields inconsistent");
			}
			uint64_t const count = load_le64(bytes.data() + record_count_offset);
			// The ceil without the sum, which any slot count the header may
			// carry would overflow.
			if (count != slot_count / slots_per_segment + (slot_count % slots_per_segment != 0 ? 1 : 0)) {
				return fail(errc::recipe_corrupt, "recipe segment count does not match the slot count");
			}
			if (load_le64(bytes.data() + dictionary_count_offset) != 0) {
				return fail(errc::recipe_unsupported, "recipe dictionary chunks unknown to the build");
			}
			// Length against the record count, without multiplying a count the
			// file could have made large enough to overflow.
			size_t const table_room = bytes.size() - manifest_min_size;
			if (table_room % record_size != 0 || count != table_room / record_size) {
				return fail(errc::recipe_corrupt, "recipe manifest length does not match the segment count");
			}
			if (load_le64(bytes.data() + bytes.size() - sizeof(uint64_t)) !=
				checksum64({bytes.data() + record_count_offset,
							bytes.size() - record_count_offset - sizeof(uint64_t)})) {
				return fail(errc::recipe_corrupt, "recipe segment table checksum mismatch");
			}

			// A slot count this host cannot hold describes no region this build
			// can open, so it is a bad manifest and not an allocation failure
			// of the caller.
			try {
				rec.entries.assign(static_cast<size_t>(slot_count), block_digest{});
				rec.segments.resize(static_cast<size_t>(count));
			} catch (std::bad_alloc const &) {
				return fail(errc::recipe_corrupt, "recipe slot count does not fit in memory");
			} catch (std::length_error const &) {
				return fail(errc::recipe_corrupt, "recipe slot count does not fit in memory");
			}
			for (size_t index = 0; index < rec.segments.size(); ++index) {
				std::byte const *const in = bytes.data() + manifest_header_size + index * record_size;
				if (in[record_flags_offset] != std::byte{0}) {
					return fail(errc::recipe_corrupt, "recipe segment flags are not zero");
				}
				auto const encoding = static_cast<segment_encoding>(in[0]);
				if (encoding != segment_encoding::all_empty && encoding != segment_encoding::raw) {
					return fail(errc::recipe_unsupported, "recipe segment encoding unknown to the build");
				}
				if (!all_zero(in + record_digest_offset + entry_width, record_digest_room - entry_width)) {
					return fail(errc::recipe_corrupt, "recipe segment digest padding is not zero");
				}
				segment_record &record = rec.segments[index];
				record.encoding = encoding;
				record.byte_length = load_le32(in + record_length_offset);
				if (!all_zero(in + record_digest_offset, entry_width)) {
					std::memcpy(record.digest.bytes.data(), in + record_digest_offset, entry_width);
					record.digest.size = static_cast<uint8_t>(entry_width);
				}

				uint64_t const base = static_cast<uint64_t>(index) * slots_per_segment;
				size_t const slots = static_cast<size_t>(std::min<uint64_t>(slots_per_segment,
																			slot_count - base));
				if (encoding == segment_encoding::all_empty) {
					if (record.byte_length != 0 || record.digest.size != 0) {
						return fail(errc::recipe_corrupt, "an empty recipe segment names a file");
					}
					continue;  // its entries are sentinels already
				}
				if (record.digest.size == 0) {
					return fail(errc::recipe_corrupt, "a raw recipe segment names no file");
				}
				auto const content = read_whole_file(store.block_path(record.digest),
													 errc::datastore_inconsistent,
													 "referenced recipe segment absent",
													 "read a recipe segment");
				if (!content) {
					return std::unexpected{content.error()};
				}
				std::span<block_digest> const out{rec.entries.data() + base, slots};
				if (auto decoded = decode_segment(*content, record, base, algorithm, out); !decoded) {
					return std::unexpected{decoded.error()};
				}
			}
			return rec;
		}

	}  // namespace

	result<recipe> recipe::load(fs::path const &segment_dir, block_store const &store) {
		auto const bytes = read_whole_file(segment_dir / recipe_file_name, errc::datastore_missing,
										  "recipe file absent", "read the recipe file");
		if (!bytes) {
			return std::unexpected{bytes.error()};
		}
		// The version picks the reader: version 2 is a manifest of segment
		// files, version 1 is the whole recipe. Every other buffer goes to the
		// version 1 reader, which reports the damage or the unsupported
		// version.
		if (bytes->size() >= 12 && std::memcmp(bytes->data(), magic, sizeof(magic)) == 0 &&
			load_le32(bytes->data() + 8) == recipe_format_version) {
			return load_manifest(*bytes, store);
		}
		return deserialize(*bytes);
	}

	result<> validate_blocks(recipe const &rec, block_store const &store) {
		boost::unordered_flat_set<block_digest, block_digest_hash> checked;
		for (auto const &entry : rec.entries) {
			if (entry.size == 0 || !checked.insert(entry).second) {
				continue;
			}
			struct stat st {};
			if (::stat(store.block_path(entry).c_str(), &st) != 0) {
				return fail_errno(errc::datastore_inconsistent, "referenced block absent");
			}
			if (!S_ISREG(st.st_mode) || std::cmp_not_equal(st.st_size, rec.block_size)) {
				return fail(errc::block_file_invalid, "referenced block is not exactly block_size");
			}
		}
		return {};
	}

	result<> deep_verify_blocks(recipe const &rec, block_store const &store) {
		boost::unordered_flat_set<block_digest, block_digest_hash> checked;
		std::vector<std::byte> content(rec.block_size);
		for (auto const &entry : rec.entries) {
			if (entry.size == 0 || !checked.insert(entry).second) {
				continue;
			}
			int const fd = ::open(store.block_path(entry).c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				return fail_errno(errc::datastore_inconsistent, "open referenced block");
			}
			size_t offset = 0;
			while (offset < content.size()) {
				ssize_t const got = ::pread(fd, content.data() + offset, content.size() - offset,
											static_cast<off_t>(offset));
				if (got < 0) {
					if (errno == EINTR) {
						continue;
					}
					auto const err = fail_errno(errc::io_error, "read referenced block");
					::close(fd);
					return err;
				}
				if (got == 0) {
					break;
				}
				offset += static_cast<size_t>(got);
			}
			::close(fd);
			if (offset != content.size()) {
				return fail(errc::block_file_invalid, "referenced block is not exactly block_size");
			}
			if (hash_block(rec.algorithm, content) != entry) {
				return fail(errc::block_file_invalid, "referenced block does not hash to its name");
			}
		}
		return {};
	}

}  // namespace privateer
