#include <privateer/block_store.hpp>

#include <privateer/file_util.hpp>
#include <privateer/logger.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace privateer {

	namespace fs = std::filesystem;

	namespace {

		std::string shard_name(uint8_t byte) {
			static constexpr char alphabet[] = "0123456789abcdef";
			return {alphabet[byte >> 4], alphabet[byte & 0xF]};
		}

		uint8_t shard_byte(block_digest const &name) {
			return static_cast<uint8_t>(name.bytes[0]);
		}

		// byte-compares an existing block file against data
		result<bool> file_equals(fs::path const &path, std::span<std::byte const> data) {
			int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				return fail_errno(errc::io_error, "open block for the dedup compare");
			}
			struct stat st {};
			if (::fstat(fd, &st) != 0) {
				auto const err = fail_errno(errc::io_error, "stat block for the dedup compare");
				::close(fd);
				return err;
			}
			if (std::cmp_not_equal(st.st_size, data.size())) {
				::close(fd);
				return false;
			}
			std::vector<std::byte> buffer(std::min<size_t>(data.size(), size_t{1} << 20));
			size_t offset = 0;
			while (offset < data.size()) {
				ssize_t const got = ::pread(fd, buffer.data(), std::min(buffer.size(), data.size() - offset),
											static_cast<off_t>(offset));
				if (got < 0) {
					if (errno == EINTR) {
						continue;
					}
					auto const err = fail_errno(errc::io_error, "read block for the dedup compare");
					::close(fd);
					return err;
				}
				if (got == 0 || std::memcmp(buffer.data(), data.data() + offset, static_cast<size_t>(got)) != 0) {
					::close(fd);
					return false;
				}
				offset += static_cast<size_t>(got);
			}
			::close(fd);
			return true;
		}

	}  // namespace

	result<block_store> block_store::create(fs::path const &segment_dir, bool durable) {
		fs::path blocks_dir = segment_dir / "blocks";
		if (::mkdir(blocks_dir.c_str(), 0755) != 0) {
			if (errno == EEXIST) {
				return fail(errc::datastore_exists, "block store already present");
			}
			return fail_errno(errc::io_error, "mkdir blocks");
		}
		for (unsigned byte = 0; byte < 256; ++byte) {
			fs::path const shard = blocks_dir / shard_name(static_cast<uint8_t>(byte));
			if (::mkdir(shard.c_str(), 0755) != 0) {
				return fail_errno(errc::io_error, "mkdir shard");
			}
			if (durable) {
				if (auto synced = sync_directory(shard); !synced) {
					return std::unexpected{synced.error()};
				}
			}
		}
		if (durable) {
			if (auto synced = sync_directory(blocks_dir); !synced) {
				return std::unexpected{synced.error()};
			}
			if (auto synced = sync_directory(segment_dir); !synced) {
				return std::unexpected{synced.error()};
			}
		}
		return block_store{std::move(blocks_dir)};
	}

	result<block_store> block_store::open(fs::path const &segment_dir) {
		fs::path blocks_dir = segment_dir / "blocks";
		struct stat st {};
		if (::stat(blocks_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
			return fail(errc::datastore_missing, "blocks directory absent");
		}
		for (unsigned byte = 0; byte < 256; ++byte) {
			fs::path const shard = blocks_dir / shard_name(static_cast<uint8_t>(byte));
			if (::stat(shard.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
				return fail(errc::datastore_inconsistent, "shard directory absent");
			}
		}
		return block_store{std::move(blocks_dir)};
	}

	fs::path block_store::shard_path(block_digest const &name) const {
		return blocks_dir_ / shard_name(shard_byte(name));
	}

	fs::path block_store::block_path(block_digest const &name) const {
		return shard_path(name) / to_hex(name);
	}

	result<bool> block_store::publish(block_digest const &name, std::span<std::byte const> data) const {
		if (name.size == 0 || data.empty()) {
			return fail(errc::invalid_argument, "publish needs a name and content");
		}
		auto staged = staged_file::create_in(shard_path(name));
		if (!staged) {
			return std::unexpected{staged.error()};
		}
		if (auto written = staged->write(data); !written) {
			return std::unexpected{written.error()};
		}
		auto published = staged->publish(to_hex(name), publish_mode::fail_if_exists);
		if (!published) {
			return std::unexpected{published.error()};
		}
		if (*published) {
			return true;
		}
		auto equal = file_equals(block_path(name), data);
		if (!equal) {
			return std::unexpected{equal.error()};
		}
		if (!*equal) {
			return fail(errc::hash_collision, "existing block differs under the same name");
		}
		return false;
	}

	result<> block_store::make_durable(std::span<block_digest const> names) {
		boost::unordered_flat_set<uint8_t> shards;
		for (auto const &name : names) {
			if (durable_.contains(name)) {
				continue;
			}
			int const fd = ::open(block_path(name).c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				return fail_errno(errc::io_error, "open block for the durability barrier");
			}
			auto synced = sync_file(fd);
			::close(fd);
			if (!synced) {
				return synced;
			}
			shards.insert(shard_byte(name));
		}
		for (auto const byte : shards) {
			if (auto synced = sync_directory(blocks_dir_ / shard_name(byte)); !synced) {
				return synced;
			}
		}
		for (auto const &name : names) {
			durable_.insert(name);
		}
		return {};
	}

	bool block_store::is_durable(block_digest const &name) const {
		return durable_.contains(name);
	}

	void block_store::seed_durable(block_digest const &name) {
		durable_.insert(name);
	}

	void block_store::add_reference(block_digest const &name) {
		++refcounts_[name];
		candidates_.erase(name);
	}

	void block_store::drop_reference(block_digest const &name) {
		auto const it = refcounts_.find(name);
		if (it == refcounts_.end()) {
			PRIVATEER_LOG(log_level::warning, "unbalanced drop_reference on block {}", to_hex(name));
			return;
		}
		if (--it->second == 0) {
			refcounts_.erase(it);
			candidates_.insert(name);
		}
	}

	bool block_store::referenced(block_digest const &name) const {
		return refcounts_.contains(name);
	}

	void block_store::reclaim() {
		// names unlinked in this pass, grouped by shard: a name leaves the
		// candidate set only once its shard directory is synced, so a failed
		// sync is retried by the next pass
		boost::unordered_flat_map<uint8_t, std::vector<block_digest>> unlinked;
		for (auto const &name : candidates_) {
			if (::unlink(block_path(name).c_str()) != 0 && errno != ENOENT) {
				PRIVATEER_LOG(log_level::warning, "cannot unlink block {} (errno {})", to_hex(name), errno);
				continue;
			}
			durable_.erase(name);
			unlinked[shard_byte(name)].push_back(name);
		}
		for (auto const &[byte, names] : unlinked) {
			if (auto synced = sync_directory(blocks_dir_ / shard_name(byte)); !synced) {
				PRIVATEER_LOG(log_level::warning, "cannot sync shard {} after reclaim: {}",
							  shard_name(byte), to_string(synced.error()));
				continue;
			}
			for (auto const &name : names) {
				candidates_.erase(name);
			}
		}
	}

	result<size_t> block_store::sweep(std::span<block_digest const> referenced) const {
		boost::unordered_flat_set<std::string> keep;
		for (auto const &name : referenced) {
			keep.insert(to_hex(name));
		}
		size_t removed = 0;
		for (unsigned byte = 0; byte < 256; ++byte) {
			fs::path const shard = blocks_dir_ / shard_name(static_cast<uint8_t>(byte));
			std::error_code ec;
			for (auto it = fs::directory_iterator{shard, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
				if (keep.contains(it->path().filename().string())) {
					continue;
				}
				if (::unlink(it->path().c_str()) != 0) {
					PRIVATEER_LOG(log_level::warning, "sweep cannot unlink {} (errno {})",
								  it->path().string(), errno);
					continue;
				}
				++removed;
			}
			if (ec) {
				return std::unexpected{error{errc::io_error, ec.value(), "list shard for the sweep"}};
			}
		}
		return removed;
	}

}  // namespace privateer
