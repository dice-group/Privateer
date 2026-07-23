#include <privateer/recipe.hpp>

#include <privateer/file_util.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace privateer {

	namespace fs = std::filesystem;

	namespace {

		constexpr char magic[8] = {'P', 'V', 'R', 'E', 'C', 'I', 'P', 'E'};
		constexpr size_t header_size = 56;              // includes the header checksum
		constexpr size_t header_checksum_offset = 48;   // checksum covers the bytes before it
		constexpr size_t entry_size = max_digest_size;  // 32, zero padded
		constexpr size_t min_file_size = header_size + sizeof(uint64_t);

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

	}  // namespace

	result<std::vector<std::byte>> recipe::serialize() const {
		if (block_size == 0) {
			return fail(errc::invalid_argument, "recipe block_size is zero");
		}
		if (size % block_size != 0) {
			return fail(errc::invalid_argument, "recipe size is not slot-aligned");
		}
		if (size > capacity) {
			return fail(errc::invalid_argument, "recipe size exceeds capacity");
		}
		if (entries.size() != size / block_size) {
			return fail(errc::invalid_argument, "recipe entry count does not match size");
		}
		size_t const expected_digest = digest_size(algorithm);
		for (auto const &entry : entries) {
			if (entry.size != 0 && entry.size != expected_digest) {
				return fail(errc::invalid_argument, "recipe entry width does not match the algorithm");
			}
		}

		std::vector<std::byte> bytes(header_size + entries.size() * entry_size + sizeof(uint64_t));
		std::memcpy(bytes.data(), magic, sizeof(magic));
		store_le32(bytes.data() + 8, recipe_format_version);
		bytes[12] = static_cast<std::byte>(algorithm);
		store_le64(bytes.data() + 16, block_size);
		store_le64(bytes.data() + 24, capacity);
		store_le64(bytes.data() + 32, size);
		store_le64(bytes.data() + 40, entries.size());
		store_le64(bytes.data() + header_checksum_offset,
				   checksum64({bytes.data(), header_checksum_offset}));

		std::byte *out = bytes.data() + header_size;
		for (auto const &entry : entries) {
			std::memcpy(out, entry.bytes.data(), entry_size);  // padding is zero by construction
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
		if (version != recipe_format_version) {
			if (version > recipe_format_version) {
				return fail(errc::recipe_unsupported, "recipe format version newer than the build");
			}
			return fail(errc::recipe_corrupt, "recipe format version invalid");
		}
		auto const algorithm = static_cast<hash_algorithm>(bytes[12]);
		size_t const expected_digest = digest_size(algorithm);
		if (expected_digest == 0) {
			return fail(errc::recipe_unsupported, "recipe hash algorithm unknown to the build");
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
			if (!all_zero(in, entry_size)) {
				if (!all_zero(in + expected_digest, entry_size - expected_digest)) {
					return fail(errc::recipe_corrupt, "recipe entry padding not zero");
				}
				std::memcpy(entry.bytes.data(), in, entry_size);
				entry.size = static_cast<uint8_t>(expected_digest);
			}
			in += entry_size;
		}
		return rec;
	}

	result<> recipe::commit(fs::path const &segment_dir, bool durable) const {
		auto bytes = serialize();
		if (!bytes) {
			return std::unexpected{bytes.error()};
		}
		auto staged = staged_file::create_in(segment_dir);
		if (!staged) {
			return std::unexpected{staged.error()};
		}
		if (auto written = staged->write(*bytes); !written) {
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

	result<recipe> recipe::load(fs::path const &segment_dir) {
		int const fd = ::open((segment_dir / recipe_file_name).c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			if (errno == ENOENT) {
				return fail(errc::datastore_missing, "recipe file absent");
			}
			return fail_errno(errc::io_error, "open recipe");
		}
		struct stat st {};
		if (::fstat(fd, &st) != 0) {
			auto const err = fail_errno(errc::io_error, "stat recipe");
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
				auto const err = fail_errno(errc::io_error, "read recipe");
				::close(fd);
				return err;
			}
			if (got == 0) {
				break;  // shrunk under us; deserialize reports the truncation
			}
			offset += static_cast<size_t>(got);
		}
		::close(fd);
		return deserialize({bytes.data(), offset});
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

}  // namespace privateer
