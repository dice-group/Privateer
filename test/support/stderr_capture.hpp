// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef PRIVATEER_TEST_STDERR_CAPTURE_HPP
#define PRIVATEER_TEST_STDERR_CAPTURE_HPP

// Redirects stderr (fd 2) into a temp file while alive; finish() restores
// stderr and returns everything that was written during the capture.

#include <cstdio>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace privateer::testing {

	struct stderr_capture {
		int saved_fd = -1;
		int file_fd = -1;
		std::string path;

		stderr_capture() {
			char tmpl[] = "/tmp/privateer-stderr-XXXXXX";
			file_fd = ::mkstemp(tmpl);
			if (file_fd < 0) {
				throw std::system_error{errno, std::system_category(), "mkstemp"};
			}
			path = tmpl;
			std::fflush(stderr);
			saved_fd = ::dup(STDERR_FILENO);
			if (saved_fd < 0 || ::dup2(file_fd, STDERR_FILENO) < 0) {
				throw std::system_error{errno, std::system_category(), "dup2"};
			}
		}

		stderr_capture(stderr_capture const &) = delete;
		stderr_capture &operator=(stderr_capture const &) = delete;

		// restores stderr and returns the captured bytes; idempotent
		std::string finish() {
			if (saved_fd >= 0) {
				std::fflush(stderr);
				::dup2(saved_fd, STDERR_FILENO);
				::close(saved_fd);
				saved_fd = -1;
			}
			std::string out;
			if (file_fd >= 0) {
				::lseek(file_fd, 0, SEEK_SET);
				char buf[4096];
				ssize_t n;
				while ((n = ::read(file_fd, buf, sizeof(buf))) > 0) {
					out.append(buf, static_cast<size_t>(n));
				}
			}
			return out;
		}

		~stderr_capture() {
			finish();
			if (file_fd >= 0) {
				::close(file_fd);
				::unlink(path.c_str());
			}
		}
	};

}  // namespace privateer::testing

#endif  // PRIVATEER_TEST_STDERR_CAPTURE_HPP
