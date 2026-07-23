// Own test binary: it defines a strong metall_log, which must replace
// privateer's weak built-in sink at link time. The other test binaries keep
// the built-in sink, so both linking modes are covered.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include <privateer/logger.hpp>
#include <support/stderr_capture.hpp>

namespace {

	struct captured_message {
		metall_log_level lvl;
		std::string file;
		size_t line;
		std::string message;
	};

	std::vector<captured_message> g_captured;

}  // namespace

extern "C" void metall_log(metall_log_level lvl, char const *file_name, size_t line_no, char const *message) {
	g_captured.push_back({lvl, file_name, line_no, message});
}

namespace {

	using privateer::log_level;

	TEST(LoggerOverride, StrongSymbolReceivesAllLevels) {
		g_captured.clear();
		privateer::testing::stderr_capture cap;
		// verbose is below the built-in sink's filter; the override sees it anyway,
		// which proves the filter lives in the sink, not in the macro
		PRIVATEER_LOG(log_level::verbose, "to the override");
		size_t const line = __LINE__ - 1;
		EXPECT_TRUE(cap.finish().empty()) << "the built-in stderr sink must be replaced";
		ASSERT_EQ(g_captured.size(), 1u);
		EXPECT_EQ(g_captured[0].lvl, metall_verbose);
		EXPECT_NE(g_captured[0].file.find("logger_override_tests.cpp"), std::string::npos);
		EXPECT_EQ(g_captured[0].line, line);
		EXPECT_EQ(g_captured[0].message, "to the override");
	}

	TEST(LoggerOverride, FormatOverloadRoutesToOverride) {
		g_captured.clear();
		PRIVATEER_LOG(log_level::critical, "epoch {}", 42);
		ASSERT_EQ(g_captured.size(), 1u);
		EXPECT_EQ(g_captured[0].lvl, metall_critical);
		EXPECT_EQ(g_captured[0].message, "epoch 42");
	}

}  // namespace
