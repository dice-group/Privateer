#include <gtest/gtest.h>

#include <privateer/fault_handler.hpp>
#include <privateer/region.hpp>
#include <privateer/vm.hpp>

#include "support/store_builder.hpp"
#include "support/temp_dir.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

#include <sys/mman.h>

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
using namespace std::chrono_literals;
using privateer::testing::count_block_files;
namespace fs = std::filesystem;

namespace {

	bool eventually(std::function<bool()> const &condition, std::chrono::seconds timeout = 30s) {
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (condition()) {
				return true;
			}
			std::this_thread::sleep_for(1ms);
		}
		return condition();
	}

	// the protection-change seam: fails the next g_protect_fails write
	// upgrades, everything else passes through
	std::atomic<int> g_protect_fails{0};

	int failing_protect(void *addr, size_t len, int prot) {
		if ((prot & PROT_WRITE) != 0) {
			for (int budget = g_protect_fails.load(std::memory_order_acquire); budget != 0;) {
				if (budget < 0 ||
					g_protect_fails.compare_exchange_weak(budget, budget - 1,
														  std::memory_order_acq_rel)) {
					errno = ENOMEM;
					return -1;
				}
			}
		}
		return ::mprotect(addr, len, prot);
	}

	struct RegionCommitTest : ::testing::Test {
		privateer::testing::temp_dir dir;
		uint64_t const bs = page_size();

		void TearDown() override {
			detail_region::mprotect_fn = ::mprotect;
			g_protect_fails.store(0);
		}

		[[nodiscard]] region_options options() const {
			region_options opts;
			opts.block_size = bs;
			return opts;
		}

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

	TEST_F(RegionCommitTest, ACommitPersistsDirtySlots) {
		{
			auto reg = make_region(2);
			bytes(reg)[0] = 'x';
			bytes(reg)[bs] = 'y';
			ASSERT_TRUE(reg.commit(true));
			auto &table = detail_region::table_of(reg);
			EXPECT_EQ(table.load(0), slot_state::clean);
			EXPECT_EQ(table.load(1), slot_state::clean);
			EXPECT_EQ(table.dirty_slots(), 0u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 2 * bs);
		EXPECT_EQ(bytes(*reopened)[0], 'x');
		EXPECT_EQ(bytes(*reopened)[bs], 'y');
	}

	TEST_F(RegionCommitTest, WritesStayReadableThroughTheCommit) {
		auto reg = make_region(1);
		bytes(reg)[7] = 'q';
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(bytes(reg)[7], 'q');  // the remap swaps identical bytes
		EXPECT_EQ(bytes(reg)[8], 0);
	}

	TEST_F(RegionCommitTest, ACommitOnAReadOnlyRegionIsANoOpSuccess) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open_read_only(dir.path);
		ASSERT_TRUE(reg.has_value());
		EXPECT_TRUE(reg->commit(true));
		EXPECT_TRUE(reg->commit(false));
		EXPECT_EQ(bytes(*reg)[0], 'a');
	}

	TEST_F(RegionCommitTest, AnEmptyDirtySetStillPersistsTheSize) {
		{
			auto reg = make_region(3);
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(reopened->size(), 3 * bs);
	}

	TEST_F(RegionCommitTest, ValueIdenticalWritesProduceNoNewBlock) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'a';  // dirtied, but the content is unchanged
		ASSERT_EQ(detail_region::table_of(*reg).dirty_slots(), 1u);
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(count_block_files(dir.path), 1u);
		EXPECT_EQ(detail_region::table_of(*reg).load(0), slot_state::clean);
		EXPECT_EQ(detail_region::table_of(*reg).dirty_slots(), 0u);
	}

	TEST_F(RegionCommitTest, IdenticalSlotsDeduplicateIntoOneBlock) {
		{
			auto reg = make_region(2);
			bytes(reg)[0] = 'd';
			bytes(reg)[bs] = 'd';
			ASSERT_TRUE(reg.commit(true));
			EXPECT_EQ(count_block_files(dir.path), 1u);
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 'd');
		EXPECT_EQ(bytes(*reopened)[bs], 'd');
	}

	TEST_F(RegionCommitTest, ADurableCommitReclaimsTheRetiredBlock) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'b';
		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(count_block_files(dir.path), 1u);  // the block holding 'a' is unlinked
		EXPECT_EQ(bytes(*reg)[0], 'b');
	}

	TEST_F(RegionCommitTest, ANonDurableCommitNeverUnlinks) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value());
			bytes(*reg)[0] = 'b';
			ASSERT_TRUE(reg->commit(false));
			// the retired block must survive: a lost rename has to be able to
			// resurface the old recipe with all its blocks intact
			EXPECT_EQ(count_block_files(dir.path), 2u);
		}
		auto reopened = region::open(dir.path);  // the sweep removes the retired block
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], 'b');
		EXPECT_EQ(count_block_files(dir.path), 1u);
	}

	TEST_F(RegionCommitTest, ALaterDurableCommitReclaimsNonDurableRetirees) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		bytes(*reg)[0] = 'b';
		ASSERT_TRUE(reg->commit(false));
		ASSERT_EQ(count_block_files(dir.path), 2u);
		bytes(*reg)[0] = 'c';
		ASSERT_TRUE(reg->commit(true));
		// both retired names ('a' from the non-durable commit, 'b' from this
		// one) are unlinked once the rename is durable
		EXPECT_EQ(count_block_files(dir.path), 1u);
		EXPECT_EQ(bytes(*reg)[0], 'c');
	}

	TEST_F(RegionCommitTest, GrowthAcrossCommitsAccumulates) {
		{
			auto reg = make_region(1);
			bytes(reg)[0] = '1';
			ASSERT_TRUE(reg.commit(true));
			ASSERT_TRUE(reg.extend(3 * bs));
			bytes(reg)[2 * bs] = '3';
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(reopened->size(), 3 * bs);
		EXPECT_EQ(bytes(*reopened)[0], '1');
		EXPECT_EQ(bytes(*reopened)[bs], 0);
		EXPECT_EQ(bytes(*reopened)[2 * bs], '3');
	}

	TEST_F(RegionCommitTest, ASimulatedEmptiedSlotCommitsTheSentinel) {
		privateer::testing::build_committed_store(dir.path, bs, {'a'});
		auto reg = region::open(dir.path);
		ASSERT_TRUE(reg.has_value());
		// stand-in for free_region: the slot resolves to dirty_empty
		auto &table = detail_region::table_of(*reg);
		ASSERT_TRUE(table.try_claim(0, slot_state::clean, slot_state::freeing));
		table.publish(0, slot_state::dirty_empty);

		ASSERT_TRUE(reg->commit(true));
		EXPECT_EQ(table.load(0), slot_state::empty);
		EXPECT_EQ(count_block_files(dir.path), 0u);  // the emptied slot retired its name
	}

	TEST_F(RegionCommitTest, ACommitRecoversAPoisonedSlot) {
		{
			auto reg = make_region(1);
			auto &table = detail_region::table_of(reg);
			g_protect_fails.store(1);  // the handler's change fails once; the retry heals
			detail_region::mprotect_fn = failing_protect;

			std::atomic<bool> done{false};
			std::thread writer{[&] {
				(void) arm_thread_fault_stack();
				bytes(reg)[0] = 'w';  // poisons the slot and parks in the handler
				done.store(true, std::memory_order_release);
			}};
			ASSERT_TRUE(eventually([&] { return table.load(0) == slot_state::poisoned; }));

			// capture retries the protection change under the commit mutex,
			// heals the slot, and captures it; the parked store lands
			ASSERT_TRUE(reg.commit(true));
			writer.join();
			EXPECT_TRUE(done.load());
			EXPECT_TRUE(reg.check_sanity());
			EXPECT_EQ(detail_region::poisoned_slots(reg), 0u);
			EXPECT_EQ(bytes(reg)[0], 'w');

			// the store lands on either side of the capture's freeze; this
			// commit persists it whichever side it took
			ASSERT_TRUE(reg.commit(true));
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'w');
	}

	TEST_F(RegionCommitTest, ACommitOverAnUnrecoveredPoisonedSlotKeepsTheEntry) {
		privateer::testing::build_committed_store(dir.path, bs, {'a', 'b'});
		{
			auto reg = region::open(dir.path);
			ASSERT_TRUE(reg.has_value()) << to_string(reg.error());
			auto &table = detail_region::table_of(*reg);
			bytes(*reg)[bs] = 'y';  // slot 1 dirties before the seam engages

			g_protect_fails.store(-1);  // every upgrade fails: recovery cannot heal yet
			detail_region::mprotect_fn = failing_protect;
			std::atomic<bool> done{false};
			std::thread writer{[&] {
				(void) arm_thread_fault_stack();
				bytes(*reg)[0] = 'w';  // poisons slot 0 and parks in the handler
				done.store(true, std::memory_order_release);
			}};
			ASSERT_TRUE(eventually([&] { return table.load(0) == slot_state::poisoned; }));

			// The slot's content is unchanged since its entry named it, so
			// capture keeps the entry and the commit stays valid without the
			// slot. No terminal flag: the slot is still recoverable.
			ASSERT_TRUE(reg->commit(true));
			EXPECT_TRUE(reg->check_sanity());
			EXPECT_EQ(table.load(0), slot_state::poisoned);

			// heal so the writer lands, then discard its uncommitted write
			g_protect_fails.store(0);
			(void) detail_region::run_cleaner_batch(*reg, false);
			writer.join();
			EXPECT_TRUE(done.load());
			EXPECT_EQ(bytes(*reg)[0], 'w');
		}
		auto reopened = region::open(dir.path);
		ASSERT_TRUE(reopened.has_value()) << to_string(reopened.error());
		EXPECT_EQ(bytes(*reopened)[0], 'a');  // the kept entry: the pre-poisoning content
		EXPECT_EQ(bytes(*reopened)[bs], 'y');  // the rest of the commit landed
	}

	TEST_F(RegionCommitTest, RewritesAfterACommitPersistAgain) {
		auto reg = make_region(1);
		bytes(reg)[0] = 'x';
		ASSERT_TRUE(reg.commit(true));
		bytes(reg)[0] = 'y';  // re-dirties the now clean slot
		EXPECT_EQ(detail_region::table_of(reg).dirty_slots(), 1u);
		ASSERT_TRUE(reg.commit(true));
		EXPECT_EQ(bytes(reg)[0], 'y');
		EXPECT_EQ(count_block_files(dir.path), 1u);
	}

	// The downstream write pattern: the writer quiesces around every commit
	// (Tentris serializes its writer around checkpoints), so each commit is
	// a consistent cut and every cut persists what the writer wrote.
	TEST_F(RegionCommitTest, AQuiescedWriterSeesEveryCutPersist) {
		auto reg = make_region(2);
		constexpr int rounds = 10;
		std::atomic<int> phase{0};  // even: the writer's turn, odd: the committer's
		std::thread writer{[&] {
			for (int round = 0; round < rounds; ++round) {
				while (phase.load(std::memory_order_acquire) != 2 * round) {
				}
				bytes(reg)[0] = static_cast<unsigned char>(round + 1);
				bytes(reg)[bs + static_cast<uint64_t>(round)] = static_cast<unsigned char>(round + 1);
				phase.store(2 * round + 1, std::memory_order_release);
			}
		}};
		for (int round = 0; round < rounds; ++round) {
			while (phase.load(std::memory_order_acquire) != 2 * round + 1) {
			}
			ASSERT_TRUE(reg.commit(round % 2 == 0));
			phase.store(2 * round + 2, std::memory_order_release);
		}
		writer.join();
		ASSERT_TRUE(reg.commit(true));

		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		EXPECT_EQ(bytes(*reopened)[0], rounds);
		for (int round = 0; round < rounds; ++round) {
			EXPECT_EQ(bytes(*reopened)[bs + static_cast<uint64_t>(round)], round + 1);
		}
	}

	// The unsynchronized interleaving: a writer keeps dirtying slots while
	// commits run. Every commit is a cut of what the barrier captured (a cut
	// may tear mid-store, the documented consequence of writing concurrently
	// with sync), committed state is never corrupted, and the final commit
	// persists the final content. The racing plain stores are exactly what
	// TSan reports, so the test does not run there.
	TEST_F(RegionCommitTest, AnUnsynchronizedWriterNeverBreaksACommit) {
#ifdef PRIVATEER_TEST_TSAN
		GTEST_SKIP() << "the writer races the capture on purpose; the tear is the documented precondition";
#endif
		auto reg = make_region(4);
		std::atomic<bool> stop{false};
		std::thread writer{[&] {
			unsigned char value = 0;
			while (!stop.load(std::memory_order_acquire)) {
				for (uint64_t slot = 0; slot < 4; ++slot) {
					bytes(reg)[slot * bs + (value % bs)] = value;
				}
				++value;
			}
		}};
		for (int i = 0; i < 20; ++i) {
			ASSERT_TRUE(reg.commit(i % 2 == 0));
		}
		stop.store(true, std::memory_order_release);
		writer.join();
		ASSERT_TRUE(reg.commit(true));

		std::vector<unsigned char> final_content(4 * bs);
		for (size_t i = 0; i < final_content.size(); ++i) {
			final_content[i] = bytes(reg)[i];
		}
		auto reopened = region::open_read_only(dir.path);
		ASSERT_TRUE(reopened.has_value());
		for (size_t i = 0; i < final_content.size(); ++i) {
			ASSERT_EQ(bytes(*reopened)[i], final_content[i]) << "at offset " << i;
		}
	}

}  // namespace
