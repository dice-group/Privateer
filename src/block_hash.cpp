#include <privateer/block_hash.hpp>

#include <cstdlib>
#include <cstring>

#include <blake3.h>
#include <openssl/evp.h>
#include <rapidhash.h>
#include <xxhash.h>

namespace privateer {

	namespace {

		// big-endian serialization, the canonical digest byte order
		void store_be64(std::byte *out, uint64_t value) noexcept {
			for (int i = 0; i < 8; ++i) {
				out[i] = static_cast<std::byte>(value >> (56 - 8 * i));
			}
		}

	}  // namespace

	char const *to_string(hash_algorithm alg) noexcept {
		switch (alg) {
			case hash_algorithm::xxh3_128: return "xxh3_128";
			case hash_algorithm::blake3: return "blake3";
			case hash_algorithm::sha256: return "sha256";
			case hash_algorithm::rapidhash: return "rapidhash";
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
			case hash_algorithm::blake3: {
				blake3_hasher hasher;
				blake3_hasher_init(&hasher);
				blake3_hasher_update(&hasher, data.data(), data.size());
				blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t *>(digest.bytes.data()), BLAKE3_OUT_LEN);
				return digest;
			}
			case hash_algorithm::sha256: {
				unsigned int len = 0;
				if (EVP_Digest(data.empty() ? "" : reinterpret_cast<char const *>(data.data()), data.size(),
							   reinterpret_cast<unsigned char *>(digest.bytes.data()), &len, EVP_sha256(), nullptr) != 1 ||
					len != digest.size) {
					std::abort();  // EVP_Digest fails only on a broken crypto library
				}
				return digest;
			}
			case hash_algorithm::rapidhash: {
				store_be64(digest.bytes.data(), ::rapidhash(data.data(), data.size()));
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
