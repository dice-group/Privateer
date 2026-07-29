// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <privateer/vm.hpp>

#include "support/sandbox.hpp"
#include "support/temp_dir.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace privateer;
using privateer::testing::is_fault_signal;
namespace fs = std::filesystem;

namespace {

	fs::path write_file(fs::path const &dir, std::string const &name, size_t len, char fill) {
		fs::path const path = dir / name;
		std::ofstream out{path, std::ios::binary};
		std::vector<char> const content(len, fill);
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		out.close();
		return path;
	}

	struct VmTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		size_t const slot = page_size();

		unsigned char volatile *bytes(vm_reservation const &reservation, size_t offset = 0) {
			return static_cast<unsigned char volatile *>(reservation.addr()) + offset;
		}
	};

	TEST_F(VmTest, PageSizeIsAPowerOfTwo) {
		ASSERT_GT(page_size(), 0u);
		EXPECT_EQ(page_size() & (page_size() - 1), 0u);
	}

	TEST_F(VmTest, ReserveRejectsUnalignedLengths) {
		EXPECT_FALSE(vm_reservation::reserve(0).has_value());
		EXPECT_FALSE(vm_reservation::reserve(slot + 1).has_value());
	}

	TEST_F(VmTest, ReservationIsPageAlignedAndMovable) {
		auto reservation = vm_reservation::reserve(4 * slot);
		ASSERT_TRUE(reservation.has_value()) << to_string(reservation.error());
		EXPECT_NE(reservation->addr(), nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(reservation->addr()) % page_size(), 0u);
		vm_reservation moved = std::move(*reservation);
		EXPECT_EQ(reservation->addr(), nullptr);
		EXPECT_EQ(moved.size(), 4 * slot);
	}

	TEST_F(VmTest, AnonymousMappingReadsZeros) {
		auto reservation = vm_reservation::reserve(4 * slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read));
		for (size_t i = 0; i < slot; i += 512) {
			EXPECT_EQ(bytes(*reservation)[i], 0);
		}
	}

	TEST_F(VmTest, ReadWriteMappingAcceptsWrites) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read_write, false));
		bytes(*reservation)[0] = 42;
		bytes(*reservation)[slot - 1] = 43;
		EXPECT_EQ(bytes(*reservation)[0], 42);
		EXPECT_EQ(bytes(*reservation)[slot - 1], 43);
	}

	TEST_F(VmTest, ProtectUpgradeMakesTheRangeWritable) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read));
		ASSERT_TRUE(protect(reservation->addr(), slot, page_access::read_write));
		bytes(*reservation)[7] = 7;
		EXPECT_EQ(bytes(*reservation)[7], 7);
	}

	TEST_F(VmTest, BlockFileMappingShowsTheFileContent) {
		auto const path = write_file(dir.path, "block", slot, 'x');
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_block_file(reservation->addr(), slot, path));
		EXPECT_EQ(bytes(*reservation)[0], 'x');
		EXPECT_EQ(bytes(*reservation)[slot - 1], 'x');
	}

	TEST_F(VmTest, WritesToAFileMappingStayPrivate) {
		auto const path = write_file(dir.path, "block", slot, 'x');
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_block_file(reservation->addr(), slot, path));
		ASSERT_TRUE(protect(reservation->addr(), slot, page_access::read_write));
		bytes(*reservation)[0] = 'y';
		EXPECT_EQ(bytes(*reservation)[0], 'y');

		std::ifstream in{path, std::ios::binary};
		char first = 0;
		in.read(&first, 1);
		EXPECT_EQ(first, 'x');  // the file never changes under a private mapping
	}

	TEST_F(VmTest, WrongSizeBlockFilesAreRejected) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());

		auto const short_file = write_file(dir.path, "short", slot - 1, 'x');
		auto short_mapped = map_block_file(reservation->addr(), slot, short_file);
		ASSERT_FALSE(short_mapped.has_value());
		EXPECT_EQ(short_mapped.error().code, errc::block_file_invalid);

		auto const long_file = write_file(dir.path, "long", slot + 1, 'x');
		auto long_mapped = map_block_file(reservation->addr(), slot, long_file);
		ASSERT_FALSE(long_mapped.has_value());
		EXPECT_EQ(long_mapped.error().code, errc::block_file_invalid);
	}

	TEST_F(VmTest, MissingBlockFileFails) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		auto mapped = map_block_file(reservation->addr(), slot, dir.path / "absent");
		ASSERT_FALSE(mapped.has_value());
		EXPECT_EQ(mapped.error().code, errc::io_error);
		EXPECT_EQ(mapped.error().sys_errno, ENOENT);
	}

	TEST_F(VmTest, RemapReplacesAFileMappingWithZeros) {
		auto const path = write_file(dir.path, "block", slot, 'x');
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_block_file(reservation->addr(), slot, path));
		ASSERT_EQ(bytes(*reservation)[0], 'x');
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read));
		EXPECT_EQ(bytes(*reservation)[0], 0);
	}

	TEST_F(VmTest, RemapReplacesAnAnonymousMappingWithFileContent) {
		auto const path = write_file(dir.path, "block", slot, 'z');
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read));
		ASSERT_EQ(bytes(*reservation)[0], 0);
		ASSERT_TRUE(map_block_file(reservation->addr(), slot, path));
		EXPECT_EQ(bytes(*reservation)[0], 'z');
	}

	TEST_F(VmTest, WriteToAReadOnlyMappingDies) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		ASSERT_TRUE(map_anonymous(reservation->addr(), slot, page_access::read));
		auto const res = PRIVATEER_SANDBOX {
			bytes(*reservation)[0] = 1;
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

	TEST_F(VmTest, ReadOfTheBareReservationDies) {
		auto reservation = vm_reservation::reserve(slot);
		ASSERT_TRUE(reservation.has_value());
		auto const res = PRIVATEER_SANDBOX {
			return static_cast<int>(bytes(*reservation)[0]);
		};
		EXPECT_TRUE(is_fault_signal(res));
	}

}  // namespace
