// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#include <privateer/resident.hpp>

#include <charconv>
#include <cstdio>
#include <string>

namespace privateer {

	namespace {

		// finds "<key>\t...<value> kB" and returns value * 1024
		bool parse_kb_line(std::string_view content, std::string_view key, uint64_t &bytes) {
			size_t pos = 0;
			while (pos < content.size()) {
				size_t const eol = content.find('\n', pos);
				std::string_view const line =
						content.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
				if (line.starts_with(key)) {
					size_t digits = key.size();
					while (digits < line.size() && (line[digits] == ' ' || line[digits] == '\t')) {
						++digits;
					}
					uint64_t value = 0;
					auto const [end, ec] =
							std::from_chars(line.data() + digits, line.data() + line.size(), value);
					if (ec != std::errc{} || end == line.data() + digits) {
						return false;
					}
					bytes = value * 1024;
					return true;
				}
				if (eol == std::string_view::npos) {
					break;
				}
				pos = eol + 1;
			}
			return false;
		}

	}  // namespace

	result<resident_usage> parse_smaps_rollup(std::string_view content) {
		resident_usage usage;
		if (!parse_kb_line(content, "Pss:", usage.pss)) {
			return fail(errc::io_error, "smaps_rollup holds no Pss line");
		}
		if (!parse_kb_line(content, "Private_Dirty:", usage.private_dirty)) {
			return fail(errc::io_error, "smaps_rollup holds no Private_Dirty line");
		}
		return usage;
	}

	result<resident_usage> read_resident_usage() {
#ifdef __linux__
		std::FILE *const file = std::fopen("/proc/self/smaps_rollup", "r");
		if (file == nullptr) {
			return fail_errno(errc::io_error, "open /proc/self/smaps_rollup");
		}
		std::string content;
		char buffer[4096];
		for (;;) {
			size_t const n = std::fread(buffer, 1, sizeof(buffer), file);
			content.append(buffer, n);
			if (n < sizeof(buffer)) {
				break;
			}
		}
		bool const failed = std::ferror(file) != 0;
		std::fclose(file);
		if (failed) {
			return fail(errc::io_error, "read /proc/self/smaps_rollup");
		}
		return parse_smaps_rollup(content);
#else
		return fail(errc::invalid_argument, "resident accounting is Linux-only");
#endif
	}

}  // namespace privateer
