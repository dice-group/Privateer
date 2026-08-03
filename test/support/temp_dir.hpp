// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_TEST_TEMP_DIR_HPP
#define PRIVATEER_TEST_TEMP_DIR_HPP

// mkdtemp'd directory for tests, removed recursively on destruction. The
// parent is the system temp directory unless the caller names another one.

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace privateer::testing {

	struct temp_dir {
		std::filesystem::path path;

		explicit temp_dir(std::filesystem::path const &parent = std::filesystem::temp_directory_path()) {
			std::string const templ = (parent / "privateer-test-XXXXXX").string();
			std::vector<char> name{templ.begin(), templ.end()};
			name.push_back('\0');
			if (::mkdtemp(name.data()) == nullptr) {
				throw std::system_error{errno, std::system_category(), "mkdtemp"};
			}
			path = name.data();
		}

		temp_dir(temp_dir const &) = delete;
		temp_dir &operator=(temp_dir const &) = delete;

		~temp_dir() {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

}  // namespace privateer::testing

#endif  // PRIVATEER_TEST_TEMP_DIR_HPP
