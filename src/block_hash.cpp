// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/block_hash.hpp>

#include <cstdlib>
#include <cstring>

#include <xxhash.h>

namespace privateer {

	char const *to_string(hash_algorithm alg) noexcept {
		switch (alg) {
			case hash_algorithm::xxh3_128: return "xxh3_128";
		}
		return "unknown";
	}

	block_digest hash_block(hash_algorithm alg, std::span<std::byte const> data) noexcept {
		block_digest digest;
		digest.size = static_cast<uint8_t>(digest_size(alg));
		switch (alg) {
			case hash_algorithm::xxh3_128: {
				XXH128_hash_t const hash = XXH3_128bits(data.data(), data.size());
				XXH128_canonical_t canonical;
				XXH128_canonicalFromHash(&canonical, hash);
				std::memcpy(digest.bytes.data(), canonical.digest, sizeof(canonical.digest));
				return digest;
			}
		}
		std::abort();  // callers validate the algorithm id before hashing
	}

	std::string to_hex(block_digest const &digest) {
		static constexpr char alphabet[] = "0123456789abcdef";
		std::string hex(size_t{2} * digest.size, '\0');
		for (size_t i = 0; i < digest.size; ++i) {
			auto const byte = static_cast<unsigned>(digest.bytes[i]);
			hex[2 * i] = alphabet[byte >> 4];
			hex[2 * i + 1] = alphabet[byte & 0xF];
		}
		return hex;
	}

	uint64_t checksum64(std::span<std::byte const> data) noexcept {
		return XXH3_64bits(data.data(), data.size());
	}

}  // namespace privateer
