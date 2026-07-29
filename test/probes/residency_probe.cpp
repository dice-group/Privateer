// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Probes P5 and P6, Linux only: the syscalls behind the resident-budget trim.
//
// P5: MADV_PAGEOUT on a clean single-mapped file range evicts the pages from
//     the page cache (the load-bearing property for the resident trim). The
//     doubly-mapped case is kernel-dependent and only recorded.
// P6: mincore reports page-cache presence for file pages, not this process's
//     resident set: MADV_DONTNEED drops the PTEs but mincore still reports 1.

#include <gtest/gtest.h>

#include "probe_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <sys/vfs.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

using namespace privateer::probes;

namespace {

	constexpr size_t probe_pages = 32;

	// The probes must run on a disk-backed filesystem: memory-backed pages
	// (tmpfs, e.g. /tmp on Ubuntu 25.04+) cannot be paged out without swap, so
	// MADV_PAGEOUT is a no-op there and the resident trim does not apply. The
	// working directory is the build tree, which is disk-backed on the CI
	// runners and in the dev container.
	std::filesystem::path probe_dir() {
		return std::filesystem::current_path();
	}

	bool memory_backed(std::filesystem::path const &dir) {
		struct statfs sb {};
		if (::statfs(dir.c_str(), &sb) != 0) {
			return false;
		}
		return sb.f_type == TMPFS_MAGIC || sb.f_type == RAMFS_MAGIC;
	}

	// forces every page in, with reads only, so file pages stay clean shared folios
	void touch_all(mapping const &m) {
		for (size_t off = 0; off < m.len; off += page_size()) {
			(void) m.bytes()[off];
		}
	}

	size_t resident_pages(mapping const &m) {
#ifdef __APPLE__
		using mincore_vec_t = char;
#else
		using mincore_vec_t = unsigned char;
#endif
		std::vector<mincore_vec_t> vec(m.len / page_size());
		if (::mincore(m.addr, m.len, vec.data()) != 0) {
			ADD_FAILURE() << "mincore failed: " << errno;
			return SIZE_MAX;
		}
		size_t count = 0;
		for (auto const b : vec) {
			count += (b & 1);
		}
		return count;
	}

	TEST(ResidencyProbe, PageoutEvictsSingleMappedCleanPages) {
		if (memory_backed(probe_dir())) {
			GTEST_SKIP() << "working directory is memory-backed; MADV_PAGEOUT needs a disk-backed filesystem";
		}
		size_t const len = probe_pages * page_size();
		temp_file file{len, 'X', probe_dir()};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);

		touch_all(m);
		ASSERT_EQ(resident_pages(m), probe_pages);

		// PAGEOUT is advisory: the kernel may refuse a pass over recently
		// touched pages, and a reclaim-congested host can decline whole
		// passes. Eviction is therefore asserted as progress over a patient
		// window of passes with growing backoff, like the governor's
		// repeated trim sweeps. The trim needs progress, not convergence in
		// one window; a kernel where PAGEOUT never evicts a clean
		// single-mapped page leaves all pages resident through every pass
		// and fails here deterministically.
		size_t after = probe_pages;
		int64_t backoff_ns = 100'000'000;
		for (int attempt = 0; attempt < 12 && after == probe_pages; ++attempt) {
			if (::madvise(m.addr, len, MADV_PAGEOUT) != 0) {
				if (errno == EINVAL) {
					GTEST_SKIP() << "kernel without MADV_PAGEOUT";
				}
				FAIL() << "madvise(MADV_PAGEOUT) failed: " << errno;
			}
			after = resident_pages(m);
			if (after == probe_pages) {
				// the cap keeps tv_nsec below one second, which nanosleep requires
				timespec const backoff{0, static_cast<long>(backoff_ns)};
				::nanosleep(&backoff, nullptr);
				backoff_ns = std::min<int64_t>(backoff_ns * 2, 800'000'000);
			}
		}
		RecordProperty("resident_after_pageout", static_cast<int>(after));
		EXPECT_LT(after, probe_pages) << "MADV_PAGEOUT evicted none of " << probe_pages
									  << " clean single-mapped pages across 12 passes; "
										 "the resident trim relies on eviction here";
	}

	TEST(ResidencyProbe, PageoutOnDoublyMappedFileRecorded) {
		if (memory_backed(probe_dir())) {
			GTEST_SKIP() << "working directory is memory-backed; MADV_PAGEOUT needs a disk-backed filesystem";
		}
		size_t const len = probe_pages * page_size();
		temp_file file{len, 'X', probe_dir()};
		mapping a = mapping::map_file(file.fd, len, PROT_READ);
		mapping b = mapping::map_file(file.fd, len, PROT_READ);

		touch_all(a);
		touch_all(b);
		ASSERT_EQ(resident_pages(a), probe_pages);
		ASSERT_EQ(resident_pages(b), probe_pages);

		if (::madvise(a.addr, len, MADV_PAGEOUT) != 0) {
			GTEST_SKIP() << "madvise(MADV_PAGEOUT) failed: " << errno;
		}

		// kernel-dependent: recent kernels skip folios mapped more than once.
		// Recorded because snapshots hard-link block files that other regions
		// or processes may map too. On the 6.x CI kernels both stay resident.
		size_t const after_a = resident_pages(a);
		size_t const after_b = resident_pages(b);
		RecordProperty("resident_a_after_pageout", static_cast<int>(after_a));
		RecordProperty("resident_b_after_pageout", static_cast<int>(after_b));
		std::printf("[ P5 ] PAGEOUT on a doubly-mapped file: %zu / %zu of %zu pages still resident\n",
					after_a, after_b, probe_pages);
		// the two mappings share folios, so their residency must agree
		EXPECT_EQ(after_a, after_b);
	}

	TEST(ResidencyProbe, MincoreCountsPageCacheNotResidentSet) {
		size_t const len = probe_pages * page_size();
		temp_file file{len, 'X', probe_dir()};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);

		touch_all(m);
		ASSERT_EQ(resident_pages(m), probe_pages);

		// drops this mapping's PTEs; the folios stay in the page cache
		ASSERT_EQ(::madvise(m.addr, len, MADV_DONTNEED), 0);
		EXPECT_EQ(resident_pages(m), probe_pages)
				<< "mincore reports the resident set, not the page cache; victim "
				   "selection for the resident trim assumes page-cache semantics";
	}

	TEST(ResidencyProbe, MincoreDropsForAnonymousDontneed) {
		// contrast case: for anonymous memory DONTNEED really empties the range
		size_t const len = probe_pages * page_size();
		void *addr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		ASSERT_NE(addr, MAP_FAILED);
		for (size_t off = 0; off < len; off += page_size()) {
			static_cast<unsigned char volatile *>(addr)[off] = 1;
		}

		std::vector<unsigned char> vec(probe_pages);
		ASSERT_EQ(::mincore(addr, len, vec.data()), 0);
		size_t resident = 0;
		for (auto const b : vec) {
			resident += (b & 1);
		}
		ASSERT_EQ(resident, probe_pages);

		ASSERT_EQ(::madvise(addr, len, MADV_DONTNEED), 0);
		ASSERT_EQ(::mincore(addr, len, vec.data()), 0);
		resident = 0;
		for (auto const b : vec) {
			resident += (b & 1);
		}
		EXPECT_EQ(resident, 0u);

		::munmap(addr, len);
	}

}  // namespace
