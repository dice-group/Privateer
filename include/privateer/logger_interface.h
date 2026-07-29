// Copyright 2026 Data Science Group (DICE), Paderborn University. See LICENSE-UPB.
// SPDX-License-Identifier: MIT

#ifndef METALL_LOGGER_INTERFACE_H
#define METALL_LOGGER_INTERFACE_H

/* Copy of metall's logger_interface.h with the same include guard: when both
 * headers appear in one translation unit, the first one wins and the second
 * is skipped. The declarations must stay ABI-identical to metall's.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* log message level, higher is more severe */
typedef enum metall_log_level {
  metall_critical = 5,
  metall_error = 4,
  metall_warning = 3,
  metall_info = 2,
  metall_debug = 1,
  metall_verbose = 0,
} metall_log_level;

/* One log sink per process. Privateer compiles in a weak default (stderr with
 * a level filter, src/logger.cpp); a strong definition anywhere in the
 * consuming process replaces it.
 */
void metall_log(metall_log_level lvl, const char *file_name, size_t line_no, const char *message);

#ifdef __cplusplus
}
#endif

#endif  // METALL_LOGGER_INTERFACE_H
