// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Unit tests for the durable file utilities: staged publication under both
// backings, the data barriers, and write_all.

#include <gtest/gtest.h>

#include <privateer/file_util.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace privateer;

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

	std::vector<std::string> published_entries(std::filesystem::path const &dir) {
		auto names = entries(dir);
		std::erase_if(names, [](std::string const &n) { return n.starts_with(temp_name_prefix); });
		return names;
	}

	class StagedFileTest : public ::testing::TestWithParam<temp_backing> {
	protected:
		privateer::testing::temp_dir dir;
	};

	INSTANTIATE_TEST_SUITE_P(Backings, StagedFileTest,
							 ::testing::Values(temp_backing::automatic, temp_backing::named),
							 [](auto const &info) {
								 return info.param == temp_backing::automatic ? "automatic" : "named";
							 });

	TEST_P(StagedFileTest, PublishMakesTheNameVisibleWithContent) {
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value()) << to_string(file.error());
		ASSERT_TRUE(file->write(as_bytes("hello block")));
		ASSERT_TRUE(file->sync());

		auto const published = file->publish("blob", publish_mode::fail_if_exists);
		ASSERT_TRUE(published.has_value()) << to_string(published.error());
		EXPECT_TRUE(*published);
		ASSERT_TRUE(sync_directory(dir.path));

		EXPECT_EQ(slurp(dir.path / "blob"), "hello block");
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"blob"});
	}

	TEST_P(StagedFileTest, NothingIsPublishedBeforePublish) {
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value()) << to_string(file.error());
		ASSERT_TRUE(file->write(as_bytes("pending")));

		// an anonymous backing has no entry; a named backing only the prefixed temp name
		EXPECT_TRUE(published_entries(dir.path).empty());
		if (file->anonymous()) {
			EXPECT_TRUE(entries(dir.path).empty());
		}
	}

	TEST_P(StagedFileTest, UnpublishedStagedFileLeavesNothing) {
		{
			auto file = staged_file::create_in(dir.path, GetParam());
			ASSERT_TRUE(file.has_value()) << to_string(file.error());
			ASSERT_TRUE(file->write(as_bytes("dropped")));
		}
		EXPECT_TRUE(entries(dir.path).empty());
	}

	TEST_P(StagedFileTest, FailIfExistsKeepsTheExistingFile) {
		auto first = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(first.has_value());
		ASSERT_TRUE(first->write(as_bytes("original")));
		ASSERT_TRUE(first->publish("blob", publish_mode::fail_if_exists).value_or(false));

		auto second = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(second.has_value());
		ASSERT_TRUE(second->write(as_bytes("duplicate")));
		auto const published = second->publish("blob", publish_mode::fail_if_exists);
		ASSERT_TRUE(published.has_value()) << to_string(published.error());
		EXPECT_FALSE(*published);

		EXPECT_EQ(slurp(dir.path / "blob"), "original");
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"blob"});
	}

	TEST_P(StagedFileTest, ReplaceSwapsTheContent) {
		auto first = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(first.has_value());
		ASSERT_TRUE(first->write(as_bytes("old recipe")));
		ASSERT_TRUE(first->publish("recipe", publish_mode::replace).value_or(false));

		auto second = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(second.has_value());
		ASSERT_TRUE(second->write(as_bytes("new recipe")));
		ASSERT_TRUE(second->sync());
		auto const published = second->publish("recipe", publish_mode::replace);
		ASSERT_TRUE(published.has_value()) << to_string(published.error());
		EXPECT_TRUE(*published);

		EXPECT_EQ(slurp(dir.path / "recipe"), "new recipe");
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"recipe"});
	}

	TEST_P(StagedFileTest, ReplaceCreatesAMissingName) {
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value());
		ASSERT_TRUE(file->write(as_bytes("fresh")));
		auto const published = file->publish("recipe", publish_mode::replace);
		ASSERT_TRUE(published.has_value()) << to_string(published.error());
		EXPECT_TRUE(*published);
		EXPECT_EQ(slurp(dir.path / "recipe"), "fresh");
	}

	TEST_P(StagedFileTest, PublishRejectsBadNames) {
		for (auto const *bad : {"", "a/b", ".privateer-tmp-x"}) {
			auto file = staged_file::create_in(dir.path, GetParam());
			ASSERT_TRUE(file.has_value());
			auto const published = file->publish(bad, publish_mode::fail_if_exists);
			ASSERT_FALSE(published.has_value()) << "accepted bad name: " << bad;
			EXPECT_EQ(published.error().code, errc::invalid_argument);
		}
		EXPECT_TRUE(entries(dir.path).empty());
	}

	TEST_P(StagedFileTest, PublishOnASpentFileIsRejected) {
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value());
		ASSERT_TRUE(file->write(as_bytes("once")));
		ASSERT_TRUE(file->publish("blob", publish_mode::fail_if_exists).value_or(false));

		auto const again = file->publish("blob2", publish_mode::fail_if_exists);
		ASSERT_FALSE(again.has_value());
		EXPECT_EQ(again.error().code, errc::invalid_argument);
	}

	TEST_P(StagedFileTest, WriteAllRoundTripsALargePayload) {
		std::string payload(1 << 20, '\0');
		for (size_t i = 0; i < payload.size(); ++i) {
			payload[i] = static_cast<char>('a' + i % 26);
		}
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value());
		ASSERT_TRUE(file->write(as_bytes(payload)));
		ASSERT_TRUE(file->sync());
		ASSERT_TRUE(file->publish("big", publish_mode::fail_if_exists).value_or(false));

		EXPECT_EQ(slurp(dir.path / "big"), payload);
	}

	TEST_P(StagedFileTest, MoveTransfersOwnership) {
		auto file = staged_file::create_in(dir.path, GetParam());
		ASSERT_TRUE(file.has_value());
		ASSERT_TRUE(file->write(as_bytes("moved")));

		staged_file moved = std::move(*file);
		ASSERT_TRUE(moved.publish("blob", publish_mode::fail_if_exists).value_or(false));
		EXPECT_EQ(slurp(dir.path / "blob"), "moved");
		EXPECT_EQ(entries(dir.path), std::vector<std::string>{"blob"});
	}

	TEST(StagedFile, AutomaticBackingRecorded) {
		privateer::testing::temp_dir dir;
		auto file = staged_file::create_in(dir.path, temp_backing::automatic);
		ASSERT_TRUE(file.has_value()) << to_string(file.error());
		// whether O_TMPFILE is available depends on kernel and filesystem
		::testing::Test::RecordProperty("anonymous_backing", file->anonymous() ? 1 : 0);

		auto named = staged_file::create_in(dir.path, temp_backing::named);
		ASSERT_TRUE(named.has_value());
		EXPECT_FALSE(named->anonymous());
	}

	TEST(FileUtil, SyncSucceedsOnFileAndDirectory) {
		privateer::testing::temp_dir dir;
		auto const file_path = dir.path / "f";
		int const fd = ::open(file_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
		ASSERT_GE(fd, 0);
		ASSERT_TRUE(write_all(fd, as_bytes("x")));

		EXPECT_TRUE(sync_file(fd));
		EXPECT_TRUE(sync_directory(dir.path));

		int const dirfd = ::open(dir.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		ASSERT_GE(dirfd, 0);
		EXPECT_TRUE(sync_directory(dirfd));
		::close(dirfd);
		::close(fd);
	}

	TEST(FileUtil, SyncFileReportsABadFd) {
		auto const res = sync_file(-1);
		ASSERT_FALSE(res.has_value());
		EXPECT_EQ(res.error().code, errc::io_error);
		EXPECT_EQ(res.error().sys_errno, EBADF);
	}

	TEST(FileUtil, SyncDirectoryReportsAMissingPath) {
		auto const res = sync_directory(std::filesystem::path{"/nonexistent/privateer-test"});
		ASSERT_FALSE(res.has_value());
		EXPECT_EQ(res.error().code, errc::io_error);
		EXPECT_EQ(res.error().sys_errno, ENOENT);
	}

	TEST(FileUtil, TheTempSweepTakesTheLeftoversAndNothingElse) {
		privateer::testing::temp_dir dir;
		std::ofstream{dir.path / (std::string{temp_name_prefix} + "1234-0")} << "linkat leftover";
		std::ofstream{dir.path / (std::string{temp_name_prefix} + "AbCdEf")} << "mkstemp leftover";
		std::ofstream{dir.path / "_recipe"} << "a manifest";
		std::ofstream{dir.path / ".hidden"} << "not a leftover";
		std::filesystem::create_directory(dir.path / "blocks");

		auto const removed = sweep_temp_files(dir.path);
		ASSERT_TRUE(removed.has_value()) << to_string(removed.error());
		EXPECT_EQ(*removed, 2u);
		EXPECT_EQ(entries(dir.path), (std::vector<std::string>{".hidden", "_recipe", "blocks"}));

		// a second pass finds nothing left
		auto const again = sweep_temp_files(dir.path);
		ASSERT_TRUE(again.has_value());
		EXPECT_EQ(*again, 0u);
	}

	TEST(FileUtil, TheTempSweepReportsAMissingDirectory) {
		auto const res = sweep_temp_files(std::filesystem::path{"/nonexistent/privateer-test"});
		ASSERT_FALSE(res.has_value());
		EXPECT_EQ(res.error().code, errc::io_error);
	}

}  // namespace
