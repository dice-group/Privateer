// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// The registry's memory discipline: remove() takes no heap memory, and add()
// reports an allocation failure instead of throwing it. This binary replaces
// the global allocation functions to count them and to make them fail, which
// is why it holds no other tests.

#include <gtest/gtest.h>

#include <privateer/error.hpp>
#include <privateer/region_registry.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

// sanitizer detection: gcc defines __SANITIZE_*, clang answers __has_feature
#ifdef __SANITIZE_THREAD__
#define PRIVATEER_TEST_TSAN 1
#endif
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define PRIVATEER_TEST_TSAN 1
#endif
#endif

using namespace privateer;
using in_flight = region_registry::in_flight_kind;

#ifdef PRIVATEER_TEST_TSAN

namespace {

	// TSan's runtime defines the global allocation functions itself, and its
	// definitions are strong: a second one does not link. The tests need the
	// hooks, so this build has none of them.
	TEST(RegionRegistryAllocation, TheHooksAreNotAvailableInAThreadSanitizerBuild) {
		GTEST_SKIP() << "TSan defines the global allocation functions itself";
	}

}  // namespace

#else

namespace {

	// Armed per thread, so the hooks stay inert for gtest's own allocations
	// and for every thread but the one under test. The counters live in the
	// binary's static thread-local block, so reading them allocates nothing.
	thread_local size_t g_allocations = 0;
	thread_local bool g_counting = false;
	thread_local bool g_failing = false;

	// counts every heap allocation of the calling thread while it lives
	struct allocation_watch {
		allocation_watch() noexcept {
			g_allocations = 0;
			g_counting = true;
		}
		allocation_watch(allocation_watch const &) = delete;
		allocation_watch &operator=(allocation_watch const &) = delete;
		~allocation_watch() { g_counting = false; }

		[[nodiscard]] static size_t count() noexcept { return g_allocations; }
	};

	// fails every heap allocation of the calling thread while it lives
	struct allocation_failure {
		allocation_failure() noexcept { g_failing = true; }
		allocation_failure(allocation_failure const &) = delete;
		allocation_failure &operator=(allocation_failure const &) = delete;
		~allocation_failure() { g_failing = false; }
	};

	void *allocate_or_null(std::size_t size, std::size_t alignment) noexcept {
		if (g_failing) {
			return nullptr;
		}
		if (g_counting) {
			++g_allocations;
		}
		void *addr = nullptr;
		// posix_memalign wants at least pointer alignment
		std::size_t const wanted = alignment > sizeof(void *) ? alignment : sizeof(void *);
		if (::posix_memalign(&addr, wanted, size != 0 ? size : 1) != 0) {
			return nullptr;
		}
		return addr;
	}

	void *allocate(std::size_t size, std::size_t alignment) {
		void *const addr = allocate_or_null(size, alignment);
		if (addr == nullptr) {
			throw std::bad_alloc{};
		}
		return addr;
	}

}  // namespace

// Every form allocates through posix_memalign and releases through free. All
// of them are replaced, including the nothrow ones: a form left to the
// runtime would pair its allocation with a free here, which a sanitizer
// reports as a mismatch.
void *operator new(std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void *operator new[](std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void *operator new(std::size_t size, std::align_val_t align) {
	return allocate(size, static_cast<std::size_t>(align));
}
void *operator new[](std::size_t size, std::align_val_t align) {
	return allocate(size, static_cast<std::size_t>(align));
}
void *operator new(std::size_t size, std::nothrow_t const &) noexcept {
	return allocate_or_null(size, alignof(std::max_align_t));
}
void *operator new[](std::size_t size, std::nothrow_t const &) noexcept {
	return allocate_or_null(size, alignof(std::max_align_t));
}
void *operator new(std::size_t size, std::align_val_t align, std::nothrow_t const &) noexcept {
	return allocate_or_null(size, static_cast<std::size_t>(align));
}
void *operator new[](std::size_t size, std::align_val_t align, std::nothrow_t const &) noexcept {
	return allocate_or_null(size, static_cast<std::size_t>(align));
}
void operator delete(void *addr) noexcept { std::free(addr); }
void operator delete[](void *addr) noexcept { std::free(addr); }
void operator delete(void *addr, std::size_t) noexcept { std::free(addr); }
void operator delete[](void *addr, std::size_t) noexcept { std::free(addr); }
void operator delete(void *addr, std::align_val_t) noexcept { std::free(addr); }
void operator delete[](void *addr, std::align_val_t) noexcept { std::free(addr); }
void operator delete(void *addr, std::size_t, std::align_val_t) noexcept { std::free(addr); }
void operator delete[](void *addr, std::size_t, std::align_val_t) noexcept { std::free(addr); }
void operator delete(void *addr, std::nothrow_t const &) noexcept { std::free(addr); }
void operator delete[](void *addr, std::nothrow_t const &) noexcept { std::free(addr); }
void operator delete(void *addr, std::align_val_t, std::nothrow_t const &) noexcept { std::free(addr); }
void operator delete[](void *addr, std::align_val_t, std::nothrow_t const &) noexcept {
	std::free(addr);
}

namespace {

	// The hooks are called through the allocation functions themselves, not
	// through a new expression: a new expression whose lifetime ends in the
	// same scope may be elided, and an elided allocation would make every
	// expectation below hold for the wrong reason.
	TEST(RegionRegistryAllocation, TheHooksSeeTheAllocationsOfTheirThread) {
		size_t counted = 0;
		{
			allocation_watch watch;
			void *const probe = ::operator new(64);
			counted = allocation_watch::count();
			::operator delete(probe);
		}
		EXPECT_EQ(counted, 1u);

		bool threw = false;
		{
			allocation_failure fail_all;
			try {
				::operator delete(::operator new(64));
			} catch (std::bad_alloc const &) {
				threw = true;
			}
		}
		EXPECT_TRUE(threw);

		// and nothing is counted or failed outside the guards
		std::vector<int> plain;
		plain.push_back(3);
		EXPECT_EQ(plain.front(), 3);
	}

	// remove() runs on the close path of a process that may be out of
	// memory, and it is noexcept: an allocation there would end the process
	// instead of closing the datastore. It must not need one.
	TEST(RegionRegistryAllocation, RemoveTakesNoHeapMemory) {
		region_registry reg;
		constexpr size_t count = 8;
		std::vector<region_record> records(count);
		for (size_t i = 0; i < count; ++i) {
			uintptr_t const start = 0x100000 + i * 0x10000;
			ASSERT_TRUE(reg.add(records[i], start, start + 0x8000));
		}

		size_t allocations = 0;
		for (size_t i = 0; i < count; ++i) {
			allocation_watch watch;
			reg.remove(records[i]);
			allocations += allocation_watch::count();
		}
		EXPECT_EQ(allocations, 0u);
		EXPECT_EQ(reg.acquire(0x100000, in_flight::handler), nullptr);
	}

	// The same promise once more, from the outside: with every allocation of
	// this thread failing, remove still returns and the registry is intact.
	TEST(RegionRegistryAllocation, RemoveCompletesWhileEveryAllocationFails) {
		region_registry reg;
		region_record a;
		region_record b;
		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));
		ASSERT_TRUE(reg.add(b, 0x30000, 0x40000));

		{
			allocation_failure fail_all;
			reg.remove(a);
		}

		EXPECT_EQ(reg.acquire(0x18000, in_flight::handler), nullptr);
		auto *hit = reg.acquire(0x38000, in_flight::handler);
		ASSERT_EQ(hit, &b);
		region_registry::release(*hit, in_flight::handler);
	}

	// add() is reached from region::open, which reports errors and throws
	// nothing. An allocation failure has to come back as an error.
	TEST(RegionRegistryAllocation, AddReportsAnAllocationFailure) {
		region_registry reg;
		region_record a;
		region_record b;
		ASSERT_TRUE(reg.add(a, 0x10000, 0x20000));

		result<> added;
		{
			allocation_failure fail_all;
			added = reg.add(b, 0x30000, 0x40000);
		}
		ASSERT_FALSE(added.has_value());
		EXPECT_EQ(added.error().code, errc::io_error);
		EXPECT_EQ(added.error().sys_errno, ENOMEM);

		// the failed add changed nothing and the registry still works
		EXPECT_EQ(reg.acquire(0x38000, in_flight::handler), nullptr);
		auto *hit = reg.acquire(0x18000, in_flight::handler);
		ASSERT_EQ(hit, &a);
		region_registry::release(*hit, in_flight::handler);
		ASSERT_TRUE(reg.add(b, 0x30000, 0x40000));
		hit = reg.acquire(0x38000, in_flight::handler);
		ASSERT_EQ(hit, &b);
		region_registry::release(*hit, in_flight::handler);
	}

}  // namespace

#endif  // PRIVATEER_TEST_TSAN
