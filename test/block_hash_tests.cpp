// Tests for the block hash layer. The digest constants pin the exact
// output bytes of xxh3_128 against vectors from an independent
// implementation, so the pin catches silent drift on a dependency bump.
// Ids 2 to 4 are retired candidates: this build computes none of them, and
// digest_size reporting 0 is what makes a recipe naming one of them fail
// to load instead of being misread.

#include <gtest/gtest.h>

#include <privateer/block_hash.hpp>

#include <cstring>
#include <string>

using namespace privateer;

namespace {

	std::span<std::byte const> as_bytes(std::string const &data) {
		return {reinterpret_cast<std::byte const *>(data.data()), data.size()};
	}

	TEST(BlockHash, DigestSizes) {
		EXPECT_EQ(digest_size(hash_algorithm::xxh3_128), 16u);
		EXPECT_EQ(digest_size(static_cast<hash_algorithm>(0)), 0u);
		EXPECT_EQ(digest_size(static_cast<hash_algorithm>(255)), 0u);
	}

	// The retired ids stay spent, so a store that names one is refused
	// rather than read under a different algorithm.
	TEST(BlockHash, RetiredAlgorithmIdsAreUnknownToThisBuild) {
		for (uint8_t const id : {2, 3, 4}) {
			EXPECT_EQ(digest_size(static_cast<hash_algorithm>(id)), 0u) << "id " << unsigned{id};
			EXPECT_STREQ(to_string(static_cast<hash_algorithm>(id)), "unknown") << "id " << unsigned{id};
		}
	}

	TEST(BlockHash, AlgorithmNames) {
		EXPECT_STREQ(to_string(hash_algorithm::xxh3_128), "xxh3_128");
		EXPECT_STREQ(to_string(static_cast<hash_algorithm>(0)), "unknown");
	}

	struct known_answer {
		hash_algorithm alg;
		char const *input_label;
		char const *hex;
	};

	std::string input_by_label(std::string const &label) {
		if (label == "empty") {
			return {};
		}
		if (label == "abc") {
			return "abc";
		}
		return std::string(1000, 'a');
	}

	TEST(BlockHash, KnownAnswers) {
		known_answer const cases[] = {
				{hash_algorithm::xxh3_128, "empty", "99aa06d3014798d86001c324468d497f"},
				{hash_algorithm::xxh3_128, "abc", "06b05ab6733a618578af5f94892f3950"},
				{hash_algorithm::xxh3_128, "a1000", "b01da365eddaa29cb3e7af627147db7c"},
		};
		for (auto const &c : cases) {
			std::string const input = input_by_label(c.input_label);
			auto const digest = hash_block(c.alg, as_bytes(input));
			EXPECT_EQ(digest.size, digest_size(c.alg)) << to_string(c.alg) << " " << c.input_label;
			EXPECT_EQ(to_hex(digest), c.hex) << to_string(c.alg) << " " << c.input_label;
		}
	}

	// The digest is 16 bytes and the recipe entry is 32, so the upper half
	// must be zero: entries are compared whole.
	TEST(BlockHash, PaddingBeyondTheDigestIsZero) {
		auto const digest = hash_block(hash_algorithm::xxh3_128, as_bytes("abc"));
		ASSERT_LT(digest.size, max_digest_size);
		for (size_t i = digest.size; i < max_digest_size; ++i) {
			EXPECT_EQ(digest.bytes[i], std::byte{0}) << "byte " << i;
		}
	}

	TEST(BlockHash, EqualDataHashesEqualDifferentDataDiffers) {
		std::string const a(4096, 'x');
		std::string b = a;
		b[2048] = 'y';
		EXPECT_EQ(hash_block(hash_algorithm::xxh3_128, as_bytes(a)),
				  hash_block(hash_algorithm::xxh3_128, as_bytes(a)));
		EXPECT_NE(hash_block(hash_algorithm::xxh3_128, as_bytes(a)),
				  hash_block(hash_algorithm::xxh3_128, as_bytes(b)));
	}

	TEST(BlockHash, Checksum64KnownAnswers) {
		EXPECT_EQ(checksum64(as_bytes("")), 0x2d06800538d394c2ull);
		EXPECT_EQ(checksum64(as_bytes("abc")), 0x78af5f94892f3950ull);
		EXPECT_EQ(checksum64(as_bytes(std::string(1000, 'a'))), 0xb3e7af627147db7cull);
	}

}  // namespace
