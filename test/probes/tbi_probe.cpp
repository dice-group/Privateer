// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Probe P7, Linux aarch64 only: faults through TBI-tagged pointers.
//
// Top-byte-ignore is on by default for user pointers on aarch64 Linux, so an
// application may legally fault through a tagged pointer. Without
// SA_EXPOSE_TAGBITS the kernel delivers si_addr untagged; the handler masks the
// top byte anyway (belt and suspenders) before the region lookup, so a tagged
// fault address can never miss the lookup and turn into a forwarded crash.

#include <gtest/gtest.h>

#include "probe_support.hpp"

#include <atomic>
#include <cstdint>

// memory-instrumenting sanitizers compute shadow addresses from the raw
// pointer, so a tagged dereference wild-faults inside the sanitizer before the
// barrier fires; the probe targets kernel behavior, not sanitizer behavior
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PRIVATEER_PROBE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PRIVATEER_PROBE_SANITIZED 1
#endif
#endif

using namespace privateer::probes;

namespace {

	constexpr uintptr_t tag_mask = 0x00FF'FFFF'FFFF'FFFF;  // clears bits 63 to 56

	void *g_slot = nullptr;
	size_t g_slot_len = 0;
	std::atomic<int> g_fault_count{0};
	std::atomic<uintptr_t> g_raw_si_addr{0};
	std::atomic<bool> g_matched_after_mask{false};

	extern "C" void masking_handler(int sig, siginfo_t *si, void *) {
		auto const raw = reinterpret_cast<uintptr_t>(si->si_addr);
		auto const addr = raw & tag_mask;
		auto const slot = reinterpret_cast<uintptr_t>(g_slot);
		g_fault_count.fetch_add(1, std::memory_order_relaxed);
		g_raw_si_addr.store(raw, std::memory_order_relaxed);
		if (addr >= slot && addr < slot + g_slot_len) {
			g_matched_after_mask.store(true, std::memory_order_relaxed);
			::mprotect(g_slot, g_slot_len, PROT_READ | PROT_WRITE);
			return;
		}
		::signal(sig, SIG_DFL);
	}

	TEST(TbiProbe, TaggedPointerFaultClassifies) {
#ifdef PRIVATEER_PROBE_SANITIZED
		GTEST_SKIP() << "tagged dereferences break sanitizer shadow addressing";
#else
		size_t const len = page_size();
		temp_file file{len, 'A'};
		mapping m = mapping::map_file(file.fd, len, PROT_READ);
		g_slot = m.addr;
		g_slot_len = len;
		g_fault_count.store(0);
		g_matched_after_mask.store(false);

		scoped_sigaction const segv{SIGSEGV, masking_handler};

		auto const untagged = reinterpret_cast<uintptr_t>(m.addr);
		auto *tagged = reinterpret_cast<unsigned char volatile *>(untagged | (0xabULL << 56));

		*tagged = 'B';  // TBI: the store ignores the top byte, the barrier must too

		EXPECT_EQ(g_fault_count.load(), 1);
		EXPECT_TRUE(g_matched_after_mask.load());
		EXPECT_EQ(m.bytes()[0], 'B');
		// without SA_EXPOSE_TAGBITS the kernel already strips the tag
		EXPECT_EQ(g_raw_si_addr.load(), untagged);

		g_slot = nullptr;
#endif
	}

}  // namespace
