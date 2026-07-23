#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <set>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <privateer/error.hpp>

namespace {

	using privateer::errc;
	using privateer::error;
	using privateer::fail;
	using privateer::fail_errno;
	using privateer::result;

	constexpr std::array all_codes{
			errc::invalid_argument,
			errc::io_error,
			errc::datastore_missing,
			errc::datastore_exists,
			errc::datastore_inconsistent,
			errc::recipe_corrupt,
			errc::recipe_unsupported,
			errc::option_mismatch,
			errc::block_file_invalid,
			errc::memlock_limit_too_low,
			errc::vma_budget_exceeded,
			errc::hash_collision,
			errc::region_poisoned,
			errc::shutting_down,
			errc::capacity_exceeded,
	};

	TEST(ErrorCodes, NamesAreNonEmptyAndDistinct) {
		std::set<std::string> seen;
		for (errc const code : all_codes) {
			std::string const n = privateer::name(code);
			EXPECT_FALSE(n.empty());
			EXPECT_NE(n, "unknown_error");
			EXPECT_TRUE(seen.insert(n).second) << n << " is not unique";
		}
	}

	TEST(ErrorCodes, UnknownValueHasFallbackName) {
		EXPECT_STREQ(privateer::name(static_cast<errc>(-1)), "unknown_error");
	}

	TEST(ErrorMessage, CodeOnly) {
		EXPECT_EQ(privateer::to_string(error{errc::recipe_corrupt}), "recipe_corrupt");
	}

	TEST(ErrorMessage, CodeAndContext) {
		EXPECT_EQ(privateer::to_string(error{errc::region_poisoned, 0, "commit capture"}),
				  "region_poisoned: commit capture");
	}

	TEST(ErrorMessage, CodeContextAndErrno) {
		std::string const msg = privateer::to_string(error{errc::io_error, ENOENT, "open"});
		EXPECT_NE(msg.find("io_error: open"), std::string::npos) << msg;
		EXPECT_NE(msg.find("errno 2"), std::string::npos) << msg;
		// the errno text is locale and platform wording, only assert it is there
		EXPECT_GT(msg.size(), std::string{"io_error: open (errno 2: )"}.size()) << msg;
	}

	TEST(ErrorHelpers, FailBuildsUnexpected) {
		result<int> const res = fail(errc::shutting_down, "sync");
		ASSERT_FALSE(res.has_value());
		EXPECT_EQ(res.error().code, errc::shutting_down);
		EXPECT_EQ(res.error().sys_errno, 0);
		EXPECT_STREQ(res.error().context, "sync");
	}

	TEST(ErrorHelpers, FailErrnoCapturesTheFailedCall) {
		auto open_missing = []() -> result<int> {
			int const fd = ::open("/nonexistent/privateer-error-test", O_RDONLY);
			if (fd < 0) {
				return fail_errno(errc::io_error, "open");
			}
			return fd;
		};
		result<int> const res = open_missing();
		ASSERT_FALSE(res.has_value());
		EXPECT_EQ(res.error().code, errc::io_error);
		EXPECT_EQ(res.error().sys_errno, ENOENT);
	}

	TEST(Result, VoidSpecializationWorks) {
		result<> const ok{};
		EXPECT_TRUE(ok.has_value());
		result<> const bad = fail(errc::datastore_inconsistent);
		ASSERT_FALSE(bad.has_value());
		EXPECT_EQ(bad.error().code, errc::datastore_inconsistent);
	}

	TEST(Result, ErrorPropagatesThroughAndThen) {
		auto const chained = result<int>{7}
									 .and_then([](int v) -> result<int> {
										 return fail(errc::vma_budget_exceeded, "extend");
									 })
									 .and_then([](int v) -> result<int> {
										 ADD_FAILURE() << "must not run after an error";
										 return v;
									 });
		ASSERT_FALSE(chained.has_value());
		EXPECT_EQ(chained.error().code, errc::vma_budget_exceeded);
	}

}  // namespace
