// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>

#include <privateer/logger.hpp>
#include <support/stderr_capture.hpp>

namespace {

	using privateer::log_level;
	using privateer::set_default_log_min_level;
	using privateer::testing::stderr_capture;

	// restores the built-in sink's default filter after each test
	struct DefaultLogger : ::testing::Test {
		void TearDown() override {
			set_default_log_min_level(log_level::warning);
		}
	};

	TEST_F(DefaultLogger, PrintsAtOrAboveMinLevel) {
		set_default_log_min_level(log_level::info);
		stderr_capture cap;
		PRIVATEER_LOG(log_level::info, "info message");
		PRIVATEER_LOG(log_level::error, "error message");
		std::string const out = cap.finish();
		EXPECT_NE(out.find("privateer [info] logger_tests.cpp:"), std::string::npos) << out;
		EXPECT_NE(out.find("info message"), std::string::npos) << out;
		EXPECT_NE(out.find("privateer [error] logger_tests.cpp:"), std::string::npos) << out;
		EXPECT_NE(out.find("error message"), std::string::npos) << out;
	}

	TEST_F(DefaultLogger, FiltersBelowMinLevel) {
		set_default_log_min_level(log_level::info);
		stderr_capture cap;
		PRIVATEER_LOG(log_level::debug, "debug message");
		PRIVATEER_LOG(log_level::verbose, "verbose message");
		EXPECT_TRUE(cap.finish().empty());
	}

	TEST_F(DefaultLogger, MinLevelIsAdjustable) {
		set_default_log_min_level(log_level::critical);
		stderr_capture cap;
		PRIVATEER_LOG(log_level::error, "suppressed error");
		PRIVATEER_LOG(log_level::critical, "critical message");
		std::string const out = cap.finish();
		EXPECT_EQ(out.find("suppressed error"), std::string::npos) << out;
		EXPECT_NE(out.find("privateer [critical]"), std::string::npos) << out;
	}

	TEST_F(DefaultLogger, DefaultMinLevelIsWarning) {
		stderr_capture cap;
		PRIVATEER_LOG(log_level::info, "hidden info");
		PRIVATEER_LOG(log_level::warning, "shown warning");
		std::string const out = cap.finish();
		EXPECT_EQ(out.find("hidden info"), std::string::npos) << out;
		EXPECT_NE(out.find("shown warning"), std::string::npos) << out;
	}

	TEST_F(DefaultLogger, FormatOverloadFormats) {
		set_default_log_min_level(log_level::info);
		stderr_capture cap;
		PRIVATEER_LOG(log_level::info, "slot {} of {}", 3, 128);
		EXPECT_NE(cap.finish().find("slot 3 of 128"), std::string::npos);
	}

	TEST_F(DefaultLogger, StringOverloadAccepted) {
		set_default_log_min_level(log_level::info);
		stderr_capture cap;
		std::string const msg = "string message";
		PRIVATEER_LOG(log_level::info, msg);
		EXPECT_NE(cap.finish().find("string message"), std::string::npos);
	}

}  // namespace
