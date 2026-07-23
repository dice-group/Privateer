// Crash tests for the staged publication: a child process is killed at each
// phase boundary. The properties checked on the survivor side: a published
// name exists exactly from the publish step on, with its full content, and a
// kill before publish leaves at most temp litter that the open-time sweep
// pattern (unlink names under temp_name_prefix) removes completely.

#include <gtest/gtest.h>

#include <privateer/file_util.hpp>

#include "support/sandbox.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <csignal>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace privateer;
using privateer::testing::subprocess_result;

namespace {

	std::span<std::byte const> as_bytes(std::string_view text) {
		return std::as_bytes(std::span{text.data(), text.size()});
	}

	std::string slurp(std::filesystem::path const &path) {
		std::ifstream in{path, std::ios::binary};
		return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
	}

	std::vector<std::string> entries(std::filesystem::path const &dir) {
		std::vector<std::string> names;
		for (auto const &entry : std::filesystem::directory_iterator{dir}) {
			names.push_back(entry.path().filename().string());
		}
		std::ranges::sort(names);
		return names;
	}

	// the open-time sweep pattern: remove every name under temp_name_prefix
	void sweep(std::filesystem::path const &dir) {
		for (auto const &entry : std::filesystem::directory_iterator{dir}) {
			if (entry.path().filename().string().starts_with(temp_name_prefix)) {
				std::filesystem::remove(entry.path());
			}
		}
	}

	// publishes name with content, used to seed a pre-existing file
	void publish_now(std::filesystem::path const &dir, std::string const &name, std::string_view content) {
		auto file = staged_file::create_in(dir);
		ASSERT_TRUE(file.has_value()) << to_string(file.error());
		ASSERT_TRUE(file->write(as_bytes(content)));
		ASSERT_TRUE(file->sync(sync_policy::full));
		ASSERT_TRUE(file->publish(name, publish_mode::replace).value_or(false));
	}

	class StagedFileCrashTest : public ::testing::TestWithParam<temp_backing> {
	protected:
		privateer::testing::temp_dir dir;
	};

	INSTANTIATE_TEST_SUITE_P(Backings, StagedFileCrashTest,
							 ::testing::Values(temp_backing::automatic, temp_backing::named),
							 [](auto const &info) {
								 return info.param == temp_backing::automatic ? "automatic" : "named";
							 });

	TEST_P(StagedFileCrashTest, KilledBeforePublishLeavesOnlySweepableLitter) {
		auto const res = PRIVATEER_SANDBOX {
			auto file = staged_file::create_in(dir.path, GetParam());
			if (!file || !file->write(as_bytes("doomed")) || !file->sync(sync_policy::full)) {
				return 1;
			}
			::raise(SIGKILL);
			return 0;
		};
		ASSERT_EQ(res, subprocess_result::killed);

		EXPECT_EQ(slurp(dir.path / "blob"), "");
		sweep(dir.path);
		EXPECT_TRUE(entries(dir.path).empty());
	}

	TEST_P(StagedFileCrashTest, KilledAfterPublishKeepsTheFullContent) {
		auto const res = PRIVATEER_SANDBOX {
			auto file = staged_file::create_in(dir.path, GetParam());
			if (!file || !file->write(as_bytes("survives")) || !file->sync(sync_policy::full)) {
				return 1;
			}
			auto const published = file->publish("blob", publish_mode::fail_if_exists);
			if (!published.value_or(false) || !sync_directory(dir.path, sync_policy::full)) {
				return 1;
			}
			::raise(SIGKILL);
			return 0;
		};
		ASSERT_EQ(res, subprocess_result::killed);

		EXPECT_EQ(slurp(dir.path / "blob"), "survives");
		sweep(dir.path);
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"blob"});
	}

	TEST_P(StagedFileCrashTest, ReplaceKilledBeforePublishKeepsTheOldContent) {
		publish_now(dir.path, "recipe", "old recipe");

		auto const res = PRIVATEER_SANDBOX {
			auto file = staged_file::create_in(dir.path, GetParam());
			if (!file || !file->write(as_bytes("new recipe")) || !file->sync(sync_policy::full)) {
				return 1;
			}
			::raise(SIGKILL);
			return 0;
		};
		ASSERT_EQ(res, subprocess_result::killed);

		EXPECT_EQ(slurp(dir.path / "recipe"), "old recipe");
		sweep(dir.path);
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"recipe"});
	}

	TEST_P(StagedFileCrashTest, ReplaceKilledAfterPublishShowsTheNewContent) {
		publish_now(dir.path, "recipe", "old recipe");

		auto const res = PRIVATEER_SANDBOX {
			auto file = staged_file::create_in(dir.path, GetParam());
			if (!file || !file->write(as_bytes("new recipe")) || !file->sync(sync_policy::full)) {
				return 1;
			}
			auto const published = file->publish("recipe", publish_mode::replace);
			if (!published.value_or(false) || !sync_directory(dir.path, sync_policy::full)) {
				return 1;
			}
			::raise(SIGKILL);
			return 0;
		};
		ASSERT_EQ(res, subprocess_result::killed);

		EXPECT_EQ(slurp(dir.path / "recipe"), "new recipe");
		sweep(dir.path);
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"recipe"});
	}

}  // namespace
