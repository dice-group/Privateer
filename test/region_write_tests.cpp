#include <gtest/gtest.h>

#include <privateer/block_store.hpp>
#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <sys/mman.h>

using namespace privateer;
using privateer::testing::is_fault_signal;
using privateer::testing::subprocess_result;
namespace fs = std::filesystem;

namespace {

	struct RegionWriteTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		[[nodiscard]] region_options options() const {
			region_options opts;
			opts.block_size = bs;
			return opts;
		}

		// a fresh read-write region with the given number of extended slots
		region make_region(uint64_t extended_slots) {
			auto reg = region::create(dir.path, 8 * bs, options());
			EXPECT_TRUE(reg.has_value()) << to_string(reg.error());
			EXPECT_TRUE(reg->extend(extended_slots * bs));
			return std::move(*reg);
		}

		static unsigned char volatile *bytes(region &reg) {
			return static_cast<unsigned char volatile *>(reg.segment());
		}
	};

	TEST_F(RegionWriteTest, TheFirstWriteMaterializesTheSlot) {
		auto reg = make_region(2);
		bytes(reg)[0] = 'x';
		EXPECT_EQ(bytes(reg)[0], 'x');
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::dirty);
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		EXPECT_TRUE(reg.check_sanity());
	}

	TEST_F(RegionWriteTest, RepeatWritesToASlotCountOnce) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'a';
		bytes(reg)[8] = 'b';
		bytes(reg)[bs - 1] = 'c';
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		EXPECT_EQ(bytes(reg)[0], 'a');
		EXPECT_EQ(bytes(reg)[8], 'b');
		EXPECT_EQ(bytes(reg)[bs - 1], 'c');
	}

	TEST_F(RegionWriteTest, EachTouchedSlotCountsOnce) {
		auto reg = make_region(3);
		bytes(reg)[0] = 'a';
		bytes(reg)[2 * bs] = 'c';
		auto &table = detail_region::table_of(reg);
		EXPECT_EQ(table.dirty_slots(), 2u);
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.load(1), slot_state::empty);
		EXPECT_EQ(table.load(2), slot_state::dirty);
	}

	TEST_F(RegionWriteTest, OverwritingACleanSlotStaysPrivate) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
		ASSERT_EQ(bytes(*reg)[0], 'a');

		bytes(*reg)[0] = 'z';
		EXPECT_EQ(bytes(*reg)[0], 'z');
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::dirty);

		// the block file never changes under the private mapping
		std::vector<std::byte> const data(bs, std::byte{'a'});
		auto store = block_store::open(dir.path);
		ASSERT_TRUE(store.has_value());
		std::ifstream block{store->block_path(hash_block(hash_algorithm::xxh3_128, data)), std::ios::binary};
		char first = 0;
		block.read(&first, 1);
		EXPECT_EQ(first, 'a');
	}

	TEST_F(RegionWriteTest, WritesBeyondTheExtendedSizeDie) {
		auto reg = make_region(1);
		auto const res = PRIVATEER_SANDBOX {
			bytes(reg)[bs] = 1;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST_F(RegionWriteTest, ErrnoSurvivesABarrierFault) {
		auto reg = make_region(1);
		errno = 42;
		bytes(reg)[0] = 'e';
		EXPECT_EQ(errno, 42);
	}

	TEST_F(RegionWriteTest, AWriterParkedOnATransientResumesAfterPublish) {
		auto reg = make_region(1);
		auto &table = detail_region::table_of(reg);
		ASSERT_TRUE(table.try_claim(0, slot_state::empty, slot_state::syncing));

		std::atomic<bool> done{false};
		std::thread writer{[&] {
			(void) arm_thread_fault_stack();
			bytes(reg)[0] = 'w';
			done.store(true, std::memory_order_release);
		}};
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		EXPECT_FALSE(done.load(std::memory_order_acquire));  // parked on the transient

		table.publish(0, slot_state::empty);
		writer.join();
		EXPECT_TRUE(done.load());
		EXPECT_EQ(bytes(reg)[0], 'w');
		EXPECT_EQ(table.load(0), slot_state::dirty);
	}

	TEST_F(RegionWriteTest, AWriteToADirtyEmptySlotMaterializes) {
		auto reg = make_region(1);
		auto &table = detail_region::table_of(reg);
		ASSERT_TRUE(table.try_claim(0, slot_state::empty, slot_state::freeing));
		table.publish(0, slot_state::dirty_empty);

		bytes(reg)[0] = 'f';
		EXPECT_EQ(bytes(reg)[0], 'f');
		EXPECT_EQ(table.load(0), slot_state::dirty);
		EXPECT_EQ(table.dirty_slots(), 1u);
	}

	TEST_F(RegionWriteTest, AFailedProtectionChangePoisonsTheSlot) {
		auto reg = make_region(1);
		detail_region::mprotect_fn = [](void *, size_t, int) {
			errno = ENOMEM;
			return -1;
		};
		bool const handled =
				detail_region::deliver_fault(reg, reinterpret_cast<uintptr_t>(reg.segment()), SIGSEGV);
		detail_region::mprotect_fn = ::mprotect;

		EXPECT_FALSE(handled);
		auto &table = detail_region::table_of(reg);
		EXPECT_EQ(table.load(0), slot_state::poisoned);
		EXPECT_EQ(table.dirty_slots(), 0u);  // the failed claim is balanced
		EXPECT_FALSE(reg.check_sanity());

		// a later fault on the poisoned slot forwards without waiting
		EXPECT_FALSE(detail_region::deliver_fault(reg, reinterpret_cast<uintptr_t>(reg.segment()), SIGSEGV));
		EXPECT_EQ(table.load(0), slot_state::poisoned);
	}

	TEST_F(RegionWriteTest, ConcurrentFirstTouchesOfOneSlotAllLand) {
		auto reg = make_region(1);
		constexpr int threads = 8;
		std::atomic<bool> go{false};
		std::vector<std::thread> writers;
		for (int i = 0; i < threads; ++i) {
			writers.emplace_back([&, i] {
				(void) arm_thread_fault_stack();
				while (!go.load(std::memory_order_acquire)) {
				}
				bytes(reg)[i] = static_cast<unsigned char>('a' + i);
			});
		}
		go.store(true, std::memory_order_release);
		for (auto &writer : writers) {
			writer.join();
		}
		for (int i = 0; i < threads; ++i) {
			EXPECT_EQ(bytes(reg)[i], static_cast<unsigned char>('a' + i));
		}
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		EXPECT_EQ(detail_region::table_of(reg).load(0), slot_state::dirty);
	}

	TEST_F(RegionWriteTest, ConcurrentWritersAcrossSlotsCountPerSlot) {
		auto reg = make_region(4);
		std::vector<std::thread> writers;
		for (int i = 0; i < 4; ++i) {
			writers.emplace_back([&, i] {
				(void) arm_thread_fault_stack();
				bytes(reg)[static_cast<uint64_t>(i) * bs] = static_cast<unsigned char>('0' + i);
			});
		}
		for (auto &writer : writers) {
			writer.join();
		}
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 4u);
		for (int i = 0; i < 4; ++i) {
			EXPECT_EQ(bytes(reg)[static_cast<uint64_t>(i) * bs], static_cast<unsigned char>('0' + i));
		}
	}

	TEST_F(RegionWriteTest, AForkChildSeesThePoisonedRegion) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'x';
		auto const res = PRIVATEER_SANDBOX {
			return reg.check_sanity() ? 7 : 0;  // the atfork handler poisons the child's copy
		};
		EXPECT_EQ(res, subprocess_result::exit_success);
		EXPECT_TRUE(reg.check_sanity());  // the parent is untouched
	}

	TEST_F(RegionWriteTest, WritesAfterCloseDie) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		// Close frees the reservation, so another thread's mapping (an
		// executor pool, the sanitizer runtime) can legitimately reuse the
		// address before the probe write lands. The child claims the page
		// with PROT_NONE first: a successful claim proves close released it
		// and pins where the write goes; a failed claim marks the attempt
		// occupied, and the parent retries. A close that stopped releasing
		// the reservation fails every attempt.
		fs::path const occupied = dir.path / "occupied";
		for (int attempt = 0; attempt < 5; ++attempt) {
			fs::remove(occupied);
			auto const res = PRIVATEER_SANDBOX {
				// fresh dispositions: the sandbox cleared the inherited handler
				privateer::detail_fault_handler::uninstall_for_tests();
				auto reg = region::open(dir.path);
				if (!reg) {
					return 10;
				}
				auto *const raw = static_cast<unsigned char volatile *>(reg->segment());
				raw[0] = 'x';
				if (raw[0] != 'x') {
					return 11;
				}
				{
					region closed = std::move(*reg);
				}
				void *const base = const_cast<unsigned char *>(raw);
				int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
				flags |= MAP_FIXED_NOREPLACE;
#endif
				void *const claimed = ::mmap(base, page_size(), PROT_NONE, flags, -1, 0);
				if (claimed == MAP_FAILED || claimed != base) {
					if (claimed != MAP_FAILED) {
						::munmap(claimed, page_size());
					}
					std::ofstream{dir.path / "occupied"};
					return 13;
				}
				raw[0] = 'y';  // faults on the PROT_NONE claim; this must die
				return 12;
			};
			if (fs::exists(occupied)) {
				continue;  // a bystander mapping held the address; try again
			}
			EXPECT_TRUE(is_fault_signal(res));
			return;
		}
		FAIL() << "the closed region's address range was still occupied after five attempts";
	}

}  // namespace
