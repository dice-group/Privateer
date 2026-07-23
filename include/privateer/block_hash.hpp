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
#include <span>
#include <string>

namespace privateer {

	enum struct hash_algorithm : uint8_t {
		xxh3_128 = 1,
		blake3 = 2,
		sha256 = 3,
		rapidhash = 4,
	};

	inline constexpr size_t max_digest_size = 32;

	// digest byte count of the algorithm, 0 for an id this build does not know
	[[nodiscard]] constexpr size_t digest_size(hash_algorithm alg) noexcept {
		switch (alg) {
			case hash_algorithm::xxh3_128: return 16;
			case hash_algorithm::blake3: return 32;
			case hash_algorithm::sha256: return 32;
			case hash_algorithm::rapidhash: return 8;
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

	// hashes one block; alg must be a known algorithm
	[[nodiscard]] block_digest hash_block(hash_algorithm alg, std::span<std::byte const> data) noexcept;

	// lowercase hex of the digest: the block file name
	[[nodiscard]] std::string to_hex(block_digest const &digest);

	// 64 bit xxh3 checksum for on-disk integrity checks, not for block names
	[[nodiscard]] uint64_t checksum64(std::span<std::byte const> data) noexcept;

}  // namespace privateer

#endif  // PRIVATEER_BLOCK_HASH_HPP
