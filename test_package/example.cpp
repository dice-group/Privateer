// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

// Smoke test for the packaged library: create a datastore, dirty one block
// through the write barrier, and commit it durably.

#include <privateer/region.hpp>
#include <privateer/version.hpp>
#include <privateer/vm.hpp>

#include <cstdio>
#include <filesystem>

int main() {
	std::printf("privateer %s\n", privateer::version());

	std::filesystem::path const dir = std::filesystem::temp_directory_path() / "privateer-test-package";
	std::filesystem::remove_all(dir);

	privateer::region_options options;
	options.block_size = privateer::page_size();

	auto region = privateer::region::create(dir, 4 * privateer::page_size(), options);
	if (!region) {
		std::printf("create failed: %s\n", to_string(region.error()).c_str());
		return 1;
	}
	if (auto extended = region->extend(privateer::page_size()); !extended) {
		std::printf("extend failed: %s\n", to_string(extended.error()).c_str());
		return 1;
	}

	static_cast<unsigned char volatile *>(region->segment())[0] = 'p';

	if (auto committed = region->commit(true); !committed) {
		std::printf("commit failed: %s\n", to_string(committed.error()).c_str());
		return 1;
	}
	if (!region->check_sanity()) {
		std::printf("the region reports a failure\n");
		return 1;
	}

	std::filesystem::remove_all(dir);
	return 0;
}
