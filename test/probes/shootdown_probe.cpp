// Probe P4: mprotect downgrade shootdown synchrony.
//
// The commit capture phase downgrades a dirty slot to PROT_READ and then treats
// its content as frozen. That is only sound if mprotect returning means no other
// core can still write through a stale writable TLB entry. This probe hammers a
// page from a writer thread while the main thread downgrades it, and asserts the
// page value never changes after mprotect has returned.

#include <gtest/gtest.h>

#include "probe_support.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace privateer::probes;

namespace {

	void *g_page = nullptr;
	size_t g_page_len = 0;
	std::atomic<uint32_t> g_resume_word{0};
	std::atomic<int> g_writer_faults{0};
	std::atomic<bool> g_wait_stuck{false};

	// runs on the writer thread when its store hits the downgraded page:
	// park until the main thread re-enables writes, then retry the store
	extern "C" void frozen_page_handler(int sig, siginfo_t *si, void *) {
		auto const addr = reinterpret_cast<uintptr_t>(si->si_addr);
		auto const page = reinterpret_cast<uintptr_t>(g_page);
		if (addr >= page && addr < page + g_page_len) {
			g_writer_faults.fetch_add(1, std::memory_order_relaxed);
			if (!wait_for_word(g_resume_word, 10'000'000'000)) {
				// bail out instead of hanging the test; the assertion below catches it
				g_wait_stuck.store(true, std::memory_order_relaxed);
				::mprotect(g_page, g_page_len, PROT_READ | PROT_WRITE);
			}
			return;  // main restored PROT_READ | PROT_WRITE before setting the word (law L1)
		}
		::signal(sig, SIG_DFL);
	}

	TEST(ShootdownProbe, DowngradeIsSynchronousAcrossThreads) {
		size_t const len = page_size();
		temp_file file{len, 0};
		mapping m = mapping::map_file(file.fd, len, PROT_READ | PROT_WRITE);
		g_page = m.addr;
		g_page_len = len;
		g_resume_word.store(1);  // writes enabled
		g_writer_faults.store(0);
		g_wait_stuck.store(false);

		scoped_sigaction const segv{SIGSEGV, frozen_page_handler};
		scoped_sigaction const bus{SIGBUS, frozen_page_handler};

		auto word = [&] {
			return std::atomic_ref<uint64_t>{*reinterpret_cast<uint64_t *>(m.addr)};
		};

		std::atomic<bool> stop{false};
		std::thread writer{[&] {
			uint64_t seq = 0;
			while (!stop.load(std::memory_order_relaxed)) {
				word().store(++seq, std::memory_order_relaxed);
			}
		}};

		int frozen_value_changed = 0;
		constexpr int rounds = 50;
		for (int r = 0; r < rounds && frozen_value_changed == 0; ++r) {
			g_resume_word.store(0, std::memory_order_relaxed);
			ASSERT_EQ(::mprotect(m.addr, len, PROT_READ), 0);

			// freeze point: from here until re-enable, the page must not change
			uint64_t const frozen = word().load(std::memory_order_relaxed);
			for (int i = 0; i < 50'000; ++i) {
				if (word().load(std::memory_order_relaxed) != frozen) {
					++frozen_value_changed;
					break;
				}
			}

			ASSERT_EQ(::mprotect(m.addr, len, PROT_READ | PROT_WRITE), 0);
			g_resume_word.store(1, std::memory_order_release);  // publish after protect, law L1
			wake_all(g_resume_word);

			// wait until the writer visibly makes progress again before the next round
			uint64_t const before = word().load(std::memory_order_relaxed);
			int64_t const deadline = monotonic_ns() + 5'000'000'000;
			while (word().load(std::memory_order_relaxed) == before && monotonic_ns() < deadline) {
			}
		}

		stop.store(true, std::memory_order_relaxed);
		writer.join();

		EXPECT_EQ(frozen_value_changed, 0) << "a write landed after mprotect(PROT_READ) returned";
		EXPECT_FALSE(g_wait_stuck.load());
		EXPECT_GE(g_writer_faults.load(), 1);

		g_page = nullptr;
	}

}  // namespace
