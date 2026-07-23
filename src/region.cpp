#include <privateer/region.hpp>

#include <privateer/block_store.hpp>
#include <privateer/logger.hpp>
#include <privateer/recipe.hpp>
#include <privateer/rlimits.hpp>
#include <privateer/slot_table.hpp>
#include <privateer/vm.hpp>

#include <cstdio>
#include <mutex>
#include <utility>

namespace privateer {

	namespace fs = std::filesystem;

	namespace {

		constexpr uint64_t round_up(uint64_t value, uint64_t multiple) noexcept {
			return (value + multiple - 1) / multiple * multiple;
		}

		// The VMA budget: vm.max_map_count minus the configured headroom for
		// the rest of the process. Darwin has no map-count limit.
		size_t vma_budget(size_t headroom) noexcept {
#ifdef __linux__
			size_t limit = 65530;  // the kernel default, used when the sysctl is unreadable
			if (std::FILE *file = std::fopen("/proc/sys/vm/max_map_count", "r"); file != nullptr) {
				unsigned long long value = 0;
				if (std::fscanf(file, "%llu", &value) == 1) {
					limit = static_cast<size_t>(value);
				}
				std::fclose(file);
			}
			return limit > headroom ? limit - headroom : 0;
#else
			(void) headroom;
			return SIZE_MAX;
#endif
		}

	}  // namespace

	struct region::state {
		fs::path segment_dir;
		std::optional<block_store> store;
		recipe rec;  // the in-memory recipe table; entries change only under the commit mutex
		slot_table table;
		vm_reservation reservation;
		size_t header_bytes = 0;
		size_t vma_headroom = 0;
		bool read_only = false;
		std::mutex region_mutex;  // serializes extend and close bookkeeping
	};

	region::region() : state_{std::make_unique<state>()} {}
	region::region(region &&) noexcept = default;
	region &region::operator=(region &&) noexcept = default;
	region::~region() = default;

	result<region> region::create(fs::path const &segment_dir, uint64_t capacity,
								  region_options const &options) {
		uint64_t const block_size = options.block_size.value_or(default_block_size);
		if (block_size == 0 || block_size % page_size() != 0) {
			return fail(errc::invalid_argument, "block_size must be a positive page multiple");
		}
		if (capacity == 0) {
			return fail(errc::invalid_argument, "capacity is zero");
		}
		std::error_code ec;
		fs::create_directories(segment_dir, ec);
		if (ec) {
			return std::unexpected{error{errc::io_error, ec.value(), "create the segment directory"}};
		}
		auto store = block_store::create(segment_dir);
		if (!store) {
			return std::unexpected{store.error()};
		}
		recipe rec;
		rec.block_size = block_size;
		rec.capacity = round_up(capacity, block_size);
		rec.size = 0;
		rec.algorithm = options.algorithm.value_or(hash_algorithm::xxh3_128);
		if (auto committed = rec.commit(segment_dir, true); !committed) {
			return std::unexpected{committed.error()};
		}
		return open_impl(segment_dir, options, false);
	}

	result<region> region::open(fs::path const &segment_dir, region_options const &options) {
		return open_impl(segment_dir, options, false);
	}

	result<region> region::open_read_only(fs::path const &segment_dir, region_options const &options) {
		return open_impl(segment_dir, options, true);
	}

	result<region> region::open_impl(fs::path const &segment_dir, region_options const &options,
									 bool read_only) {
		auto rec = recipe::load(segment_dir);
		if (!rec) {
			return std::unexpected{rec.error()};
		}
		auto store = block_store::open(segment_dir);
		if (!store) {
			return std::unexpected{store.error()};
		}

		if (options.block_size && *options.block_size != rec->block_size) {
			return fail(errc::option_mismatch, "requested block_size differs from the recipe header");
		}
		if (options.algorithm && *options.algorithm != rec->algorithm) {
			return fail(errc::option_mismatch, "requested hash algorithm differs from the recipe header");
		}
		if (rec->block_size % page_size() != 0) {
			return fail(errc::option_mismatch, "block_size is not a multiple of this host's page size");
		}
		uint64_t const slot_count = rec->capacity / rec->block_size;
		if (slot_count == 0) {
			return fail(errc::datastore_inconsistent, "capacity is smaller than one slot");
		}

		if (auto validated = validate_blocks(*rec, *store); !validated) {
			return std::unexpected{validated.error()};
		}

		uint64_t const size_slots = rec->size / rec->block_size;
		if (size_slots > vma_budget(options.vma_headroom)) {
			return fail(errc::vma_budget_exceeded, "mapping the extended size would cross the VMA budget");
		}

		bool const lock = !read_only && options.lock_state_array;
		if (lock) {
			if (auto raised = ensure_memlock_limit(slot_count * sizeof(uint32_t) + 64); !raised) {
				return std::unexpected{raised.error()};
			}
		}
		auto table = slot_table::create(slot_count, lock);
		if (!table) {
			return std::unexpected{table.error()};
		}

		size_t const header_bytes = round_up(options.header_size, page_size());
		auto reservation = vm_reservation::reserve(header_bytes + round_up(rec->capacity, page_size()));
		if (!reservation) {
			return std::unexpected{reservation.error()};
		}
		auto *const base = static_cast<std::byte *>(reservation->addr());
		if (header_bytes > 0) {
			if (auto mapped = map_anonymous(base, header_bytes, page_access::read_write, false); !mapped) {
				return std::unexpected{mapped.error()};
			}
		}

		std::byte *const segment = base + header_bytes;
		for (uint64_t i = 0; i < size_slots; ++i) {
			auto const &entry = rec->entries[i];
			void *const slot_addr = segment + i * rec->block_size;
			if (entry.size == 0) {
				if (auto mapped = map_anonymous(slot_addr, rec->block_size, page_access::read); !mapped) {
					return std::unexpected{mapped.error()};
				}
				table->publish(i, slot_state::empty);
			} else {
				if (auto mapped = map_block_file(slot_addr, rec->block_size, store->block_path(entry));
					!mapped) {
					return std::unexpected{mapped.error()};
				}
				table->publish(i, slot_state::clean);
			}
		}
		table->set_extended_size(rec->size);

		if (!read_only) {
			for (auto const &entry : rec->entries) {
				if (entry.size != 0) {
					store->seed_durable(entry);
					store->add_reference(entry);
				}
			}
			auto swept = store->sweep(rec->entries);
			if (!swept) {
				return std::unexpected{swept.error()};
			}
			if (*swept > 0) {
				PRIVATEER_LOG(log_level::info, "open-time sweep removed {} unreferenced files", *swept);
			}
		}

		region reg;
		reg.state_->segment_dir = segment_dir;
		reg.state_->store = std::move(*store);
		reg.state_->rec = std::move(*rec);
		reg.state_->table = std::move(*table);
		reg.state_->reservation = std::move(*reservation);
		reg.state_->header_bytes = header_bytes;
		reg.state_->vma_headroom = options.vma_headroom;
		reg.state_->read_only = read_only;
		return reg;
	}

	void *region::segment() const noexcept {
		return static_cast<std::byte *>(state_->reservation.addr()) + state_->header_bytes;
	}

	void *region::segment_header() const noexcept {
		return state_->reservation.addr();
	}

	uint64_t region::size() const noexcept {
		return state_->table.extended_size();
	}

	uint64_t region::capacity() const noexcept {
		return state_->rec.capacity;
	}

	uint64_t region::block_size() const noexcept {
		return state_->rec.block_size;
	}

	hash_algorithm region::algorithm() const noexcept {
		return state_->rec.algorithm;
	}

	bool region::read_only() const noexcept {
		return state_->read_only;
	}

	result<> region::extend(uint64_t target_size) {
		auto &st = *state_;
		if (st.read_only) {
			return fail(errc::invalid_argument, "extend on a read-only region");
		}
		std::lock_guard lock{st.region_mutex};
		uint64_t const current = st.table.extended_size();
		if (target_size <= current) {
			return {};
		}
		uint64_t const target = round_up(target_size, st.rec.block_size);
		if (target > st.rec.capacity) {
			return fail(errc::capacity_exceeded, "extend beyond the region capacity");
		}
		if (target / st.rec.block_size > vma_budget(st.vma_headroom)) {
			return fail(errc::vma_budget_exceeded, "extend would cross the VMA budget");
		}
		auto *const grown = static_cast<std::byte *>(segment()) + current;
		if (auto mapped = map_anonymous(grown, target - current, page_access::read); !mapped) {
			return std::unexpected{mapped.error()};
		}
		for (uint64_t i = current / st.rec.block_size; i < target / st.rec.block_size; ++i) {
			st.table.publish(i, slot_state::empty);
		}
		st.table.set_extended_size(target);
		return {};
	}

}  // namespace privateer
