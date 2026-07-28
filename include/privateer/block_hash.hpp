#ifndef PRIVATEER_BLOCK_HASH_HPP
#define PRIVATEER_BLOCK_HASH_HPP

// Content hashing for the block store. A block file is named by the
// lowercase hex of its content digest; the recipe stores the raw digest
// bytes in fixed width entries, zero padded to max_digest_size. The
// algorithm is a datastore constant recorded in the recipe header, so the
// numeric ids are part of the on-disk format and must never change.
// A weak hash cannot corrupt data: publication byte-compares whenever a
// block name already exists, so a collision costs a compare and an error.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace privateer {

	// Ids 2 (blake3), 3 (sha256) and 4 (rapidhash) were measured as candidates
	// and are retired. They stay spent: a new algorithm takes id 5 or higher,
	// so a store written by a build that had them keeps naming its own
	// algorithm and this build refuses it instead of misreading it.
	enum struct hash_algorithm : uint8_t {
		xxh3_128 = 1,
	};

	// In-memory capacity of a digest, and the widest a recipe entry may be.
	// A digest narrower than this is zero padded in memory so two digests of
	// one algorithm compare over the whole array; on disk the recipe entry is
	// exactly the digest width, which the recipe header records.
	inline constexpr size_t max_digest_size = 32;

	// digest byte count of the algorithm, 0 for an id this build does not know
	[[nodiscard]] constexpr size_t digest_size(hash_algorithm alg) noexcept {
		switch (alg) {
			case hash_algorithm::xxh3_128: return 16;
		}
		return 0;
	}

	[[nodiscard]] char const *to_string(hash_algorithm alg) noexcept;

	// Raw digest bytes in canonical (big-endian) order, zero padded beyond
	// size. Digests of different algorithms never compare equal through the
	// padding alone because callers never mix algorithms within a datastore.
	struct block_digest {
		std::array<std::byte, max_digest_size> bytes{};
		uint8_t size = 0;

		friend bool operator==(block_digest const &, block_digest const &) = default;
	};

	// Hash functor for containers keyed by block_digest. The digest bytes
	// are uniformly distributed already, so the first eight serve directly;
	// every supported digest is at least eight bytes wide.
	struct block_digest_hash {
		size_t operator()(block_digest const &digest) const noexcept {
			uint64_t word;
			std::memcpy(&word, digest.bytes.data(), sizeof(word));
			return static_cast<size_t>(word);
		}
	};

	// hashes one block; alg must be a known algorithm
	[[nodiscard]] block_digest hash_block(hash_algorithm alg, std::span<std::byte const> data) noexcept;

	// lowercase hex of the digest: the block file name
	[[nodiscard]] std::string to_hex(block_digest const &digest);

	// 64 bit xxh3 checksum for on-disk integrity checks, not for block names
	[[nodiscard]] uint64_t checksum64(std::span<std::byte const> data) noexcept;

}  // namespace privateer

#endif  // PRIVATEER_BLOCK_HASH_HPP
