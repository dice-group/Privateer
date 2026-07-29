// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Probe P8: MAP_FIXED replacement atomicity under concurrent readers.
//
// The commit path remaps a slot to its new block file with one
// mmap(MAP_FIXED) call while readers keep reading. That requires the
// replacement to be atomic: a reader must always see the old file or the new
// file, never an unmapped window (fault) and never anything else. Guaranteed
// on Linux; the probe is the required evidence on Darwin. The stress runs in a
// sandboxed child so a non-atomic kernel fails the test instead of killing the
// test binary.

#include <gtest/gtest.h>

#include <support/sandbox.hpp>

#include "probe_support.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace privateer::probes;
using privateer::testing::subprocess_result;

namespace {

	constexpr uint64_t pattern_x = 0x1111'1111'1111'1111;
	constexpr uint64_t pattern_y = 0x2222'2222'2222'2222;
	constexpr int remap_rounds = 2000;
	constexpr int reader_count = 4;

	TEST(RemapProbe, MapFixedReplacementIsAtomicUnderReaders) {
		// TSan's mmap interceptor models the remap as a plain write to the whole
		// range, so it reports the concurrent reader as a race by definition.
		// The probe validates kernel atomicity, which TSan cannot model.
#ifdef PRIVATEER_PROBE_TSAN
		GTEST_SKIP() << "TSan models mmap as a plain write; concurrent-read remap is the point here";
#else
		auto const res = PRIVATEER_SANDBOX {
			size_t const len = page_size();
			temp_file x{len, 0x11};
			temp_file y{len, 0x22};

			void *addr = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, x.fd, 0);
			if (addr == MAP_FAILED) {
				return 2;
			}

			std::atomic<bool> stop{false};
			std::atomic<int> bad_values{0};
			std::vector<std::thread> readers;
			readers.reserve(reader_count);
			for (int i = 0; i < reader_count; ++i) {
				readers.emplace_back([&] {
					auto word = std::atomic_ref<uint64_t>{*static_cast<uint64_t *>(addr)};
					while (!stop.load(std::memory_order_relaxed)) {
						uint64_t const v = word.load(std::memory_order_relaxed);
						if (v != pattern_x && v != pattern_y) {
							bad_values.fetch_add(1, std::memory_order_relaxed);
							stop.store(true, std::memory_order_relaxed);
						}
					}
				});
			}

			int failed_remaps = 0;
			for (int i = 0; i < remap_rounds && !stop.load(std::memory_order_relaxed); ++i) {
				int const fd = (i % 2 == 0) ? y.fd : x.fd;
				void *replaced = ::mmap(addr, len, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
				if (replaced != addr) {
					++failed_remaps;
					break;
				}
			}

			stop.store(true, std::memory_order_relaxed);
			for (auto &t : readers) {
				t.join();
			}

			// a fault in a reader kills the child with the fault signal instead
			if (bad_values.load() != 0) {
				return 3;  // torn or stale value, neither file's content
			}
			if (failed_remaps != 0) {
				return 4;
			}
			return 0;
		};
		EXPECT_EQ(res, subprocess_result::exit_success)
				<< "MAP_FIXED replacement is not atomic under readers on this platform";
#endif
	}

}  // namespace
