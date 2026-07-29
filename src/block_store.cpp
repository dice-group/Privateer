// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/block_store.hpp>

#include <privateer/file_util.hpp>
#include <privateer/logger.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
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

		// Batches below this stay on the calling thread. One sync of a device
		// costs hundreds of microseconds and a spread costs a few to join, so
		// the crossing is low; measured, a batch of eight is already twice as
		// fast spread out.
		constexpr size_t spread_floor = 4;

		// runs body over every index below count, spread when there is enough
		// of it and a fan-out to spread with
		void spread_syncs(block_store::sync_fan_out const &fan_out, size_t count,
						  std::function<void(size_t)> const &body) {
			if (fan_out && count >= spread_floor) {
				fan_out(count, body);
				return;
			}
			for (size_t index = 0; index < count; ++index) {
				body(index);
			}
		}

		// what a block name already holds against the content being published
		enum struct existing_block : int {
			absent,   // the name has no file
			equal,    // the file holds this content, so it is already published
			differs,  // the file holds other content under the same name
		};

		// Byte-compares the file under path against data. A name with no file
		// is absent rather than an error, because writing one is what a
		// publisher does about it.
		result<existing_block> compare_block(fs::path const &path, std::span<std::byte const> data) {
			int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				if (errno == ENOENT) {
					return existing_block::absent;
				}
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
				return existing_block::differs;
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
					return existing_block::differs;
				}
				offset += static_cast<size_t>(got);
			}
			::close(fd);
			return existing_block::equal;
		}

		// How many times publish resolves the name before it gives up. Each
		// extra round needs another publisher to take the name in the window
		// between this one's compare and its link, and something to unlink
		// that file again before the next compare. A directory entry that
		// cannot be opened at all, a dangling symlink in a tampered store,
		// exhausts the rounds and reports io_error.
		constexpr unsigned publish_attempts = 4;

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
		fs::path const path = block_path(name);
		for (unsigned attempt = 0; attempt < publish_attempts; ++attempt) {
			// A file already under the name carries the content the name
			// stands for, so the compare is the whole publication and the
			// block is not written at all. Content that repeats is the
			// common case on a workload that copies a structure and changes
			// parts of it.
			auto const existing = compare_block(path, data);
			if (!existing) {
				return std::unexpected{existing.error()};
			}
			switch (*existing) {
				case existing_block::equal:
					return false;
				case existing_block::differs:
					return fail(errc::hash_collision, "existing block differs under the same name");
				case existing_block::absent:
					break;  // the write below is what resolves it
			}
			auto staged = staged_file::create_in(shard_path(name));
			if (!staged) {
				return std::unexpected{staged.error()};
			}
			if (auto written = staged->write(data); !written) {
				return std::unexpected{written.error()};
			}
			auto const published = staged->publish(to_hex(name), publish_mode::fail_if_exists);
			if (!published) {
				return std::unexpected{published.error()};
			}
			if (*published) {
				return true;
			}
			// The atomic link is what resolves a race on one name, and this
			// caller lost it. The next round compares against the winner's
			// file, which is the dedup answer; only an unlink of that file
			// inside the same window sends the round back to the write.
		}
		return fail(errc::io_error, "the block name neither opens nor accepts a link");
	}

	result<> block_store::make_durable(std::span<block_digest const> names, sync_fan_out const &fan_out) {
		// the names this call owes a sync, each one once: a dedup hit puts the
		// same name in several recipe entries
		std::vector<block_digest> pending;
		boost::unordered_flat_set<block_digest, block_digest_hash> seen;
		boost::unordered_flat_set<uint8_t> shard_set;
		for (auto const &name : names) {
			if (durable_.contains(name) || !seen.insert(name).second) {
				continue;
			}
			pending.push_back(name);
			shard_set.insert(shard_byte(name));
		}
		if (pending.empty()) {
			for (auto const &name : names) {
				durable_.insert(name);
			}
			return {};
		}
		std::vector<uint8_t> const shards{shard_set.begin(), shard_set.end()};

		// One wave over the block files and the shard directories. Every sync
		// keeps its own failure, so a failure anywhere leaves the durable-name
		// set untouched.
		std::vector<std::optional<error>> failures(pending.size() + shards.size());
		// The path each sync builds is the only allocation left in here, and it
		// may run on a pool thread, where an escaping exception would take the
		// process down. It becomes this index's failure instead, so the barrier
		// reports it and records nothing.
		auto const sync_one = [&](size_t index) {
			try {
				if (index < pending.size()) {
					int const fd = ::open(block_path(pending[index]).c_str(), O_RDONLY | O_CLOEXEC);
					if (fd < 0) {
						failures[index] =
								fail_errno(errc::io_error, "open block for the durability barrier").error();
						return;
					}
					auto synced = sync_file(fd);
					::close(fd);
					if (!synced) {
						failures[index] = synced.error();
					}
					return;
				}
				if (auto synced = sync_directory(blocks_dir_ / shard_name(shards[index - pending.size()]));
					!synced) {
					failures[index] = synced.error();
				}
			} catch (...) {
				failures[index] = error{errc::io_error, ENOMEM, "durability barrier"};
			}
		};
		spread_syncs(fan_out, failures.size(), sync_one);

		for (auto const &failure : failures) {
			if (failure) {
				return std::unexpected{*failure};
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

	void block_store::discard_unreferenced(block_digest const &name) {
		if (refcounts_.contains(name)) {
			return;
		}
		if (::unlink(block_path(name).c_str()) != 0 && errno != ENOENT) {
			// stays a candidate; reclaim and the open-time sweep are the backstop
			PRIVATEER_LOG(log_level::warning, "cannot unlink block {} (errno {})", to_hex(name), errno);
			return;
		}
		durable_.erase(name);
		candidates_.erase(name);
	}

	void block_store::reclaim(sync_fan_out const &fan_out) {
		// The candidates grouped by shard, so one task owns a shard's unlinks
		// and its one directory sync. A name leaves the candidate set only
		// once that sync succeeded, so a failed sync is retried by the next
		// pass; the sets themselves are touched after the fan-out joined.
		struct shard_batch {
			uint8_t byte = 0;
			std::vector<block_digest> names;
			std::vector<block_digest> unlinked;
			bool synced = false;
		};
		std::vector<shard_batch> batches;
		boost::unordered_flat_map<uint8_t, size_t> batch_of;
		for (auto const &name : candidates_) {
			uint8_t const byte = shard_byte(name);
			auto const [it, fresh] = batch_of.try_emplace(byte, batches.size());
			if (fresh) {
				batches.push_back({byte, {}, {}, false});
			}
			batches[it->second].names.push_back(name);
		}

		// Reserved here, so the pass itself only allocates the paths it builds;
		// an exception from one of those may run on a pool thread, and it ends
		// the shard's task with its names still candidates, which is what any
		// other failure of the pass does.
		for (auto &batch : batches) {
			batch.unlinked.reserve(batch.names.size());
		}
		auto const reclaim_shard = [&](size_t index) {
			auto &batch = batches[index];
			try {
				for (auto const &name : batch.names) {
					if (::unlink(block_path(name).c_str()) != 0 && errno != ENOENT) {
						PRIVATEER_LOG(log_level::warning, "cannot unlink block {} (errno {})", to_hex(name), errno);
						continue;
					}
					batch.unlinked.push_back(name);
				}
				if (batch.unlinked.empty()) {
					return;
				}
				if (auto synced = sync_directory(blocks_dir_ / shard_name(batch.byte)); !synced) {
					PRIVATEER_LOG(log_level::warning, "cannot sync shard {} after reclaim: {}",
								  shard_name(batch.byte), to_string(synced.error()));
					return;
				}
				batch.synced = true;
			} catch (...) {
				// the shard keeps its candidates for the next pass
			}
		};
		spread_syncs(fan_out, batches.size(), reclaim_shard);

		for (auto const &batch : batches) {
			for (auto const &name : batch.unlinked) {
				durable_.erase(name);
				if (batch.synced) {
					candidates_.erase(name);
				}
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
