// Tests for the block hash layer. The digest constants pin the exact
// output bytes: sha256, blake3, and xxh3_128 against vectors from
// independent implementations, rapidhash against its reference
// implementation (the algorithm is defined by that implementation and has
// changed values across major versions, so the pin catches silent drift
// on a dependency bump).

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
		EXPECT_EQ(digest_size(hash_algorithm::blake3), 32u);
		EXPECT_EQ(digest_size(hash_algorithm::sha256), 32u);
		EXPECT_EQ(digest_size(hash_algorithm::rapidhash), 8u);
		EXPECT_EQ(digest_size(static_cast<hash_algorithm>(0)), 0u);
		EXPECT_EQ(digest_size(static_cast<hash_algorithm>(255)), 0u);
	}

	TEST(BlockHash, AlgorithmNames) {
		EXPECT_STREQ(to_string(hash_algorithm::xxh3_128), "xxh3_128");
		EXPECT_STREQ(to_string(hash_algorithm::blake3), "blake3");
		EXPECT_STREQ(to_string(hash_algorithm::sha256), "sha256");
		EXPECT_STREQ(to_string(hash_algorithm::rapidhash), "rapidhash");
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
				{hash_algorithm::sha256, "empty", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
				{hash_algorithm::sha256, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
				{hash_algorithm::sha256, "a1000", "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
				{hash_algorithm::blake3, "empty", "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
				{hash_algorithm::blake3, "abc", "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"},
				{hash_algorithm::blake3, "a1000", "9957a9014733dd6b6e2f6abcbe7b259a6da1aa0b0e184cd7bf1810e5c425f405"},
				{hash_algorithm::xxh3_128, "empty", "99aa06d3014798d86001c324468d497f"},
				{hash_algorithm::xxh3_128, "abc", "06b05ab6733a618578af5f94892f3950"},
				{hash_algorithm::xxh3_128, "a1000", "b01da365eddaa29cb3e7af627147db7c"},
				{hash_algorithm::rapidhash, "empty", "0338dc4be2cecdae"},
				{hash_algorithm::rapidhash, "abc", "cb475beafa9c0da2"},
				{hash_algorithm::rapidhash, "a1000", "2e40098e7e22bf67"},
		};
		for (auto const &c : cases) {
			std::string const input = input_by_label(c.input_label);
			auto const digest = hash_block(c.alg, as_bytes(input));
			EXPECT_EQ(digest.size, digest_size(c.alg)) << to_string(c.alg) << " " << c.input_label;
			EXPECT_EQ(to_hex(digest), c.hex) << to_string(c.alg) << " " << c.input_label;
		}
	}

	TEST(BlockHash, PaddingBeyondTheDigestIsZero) {
		hash_algorithm const algs[] = {hash_algorithm::xxh3_128, hash_algorithm::rapidhash};
		for (auto const alg : algs) {
			auto const digest = hash_block(alg, as_bytes("abc"));
			for (size_t i = digest.size; i < max_digest_size; ++i) {
				EXPECT_EQ(digest.bytes[i], std::byte{0}) << to_string(alg) << " byte " << i;
			}
		}
	}

	TEST(BlockHash, EqualDataHashesEqualDifferentDataDiffers) {
		hash_algorithm const algs[] = {hash_algorithm::xxh3_128, hash_algorithm::blake3,
									   hash_algorithm::sha256, hash_algorithm::rapidhash};
		std::string const a(4096, 'x');
		std::string b = a;
		b[2048] = 'y';
		for (auto const alg : algs) {
			EXPECT_EQ(hash_block(alg, as_bytes(a)), hash_block(alg, as_bytes(a))) << to_string(alg);
			EXPECT_NE(hash_block(alg, as_bytes(a)), hash_block(alg, as_bytes(b))) << to_string(alg);
		}
	}

	TEST(BlockHash, Checksum64KnownAnswers) {
		EXPECT_EQ(checksum64(as_bytes("")), 0x2d06800538d394c2ull);
		EXPECT_EQ(checksum64(as_bytes("abc")), 0x78af5f94892f3950ull);
		EXPECT_EQ(checksum64(as_bytes(std::string(1000, 'a'))), 0xb3e7af627147db7cull);
	}

}  // namespace
