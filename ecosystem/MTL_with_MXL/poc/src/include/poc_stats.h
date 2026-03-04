/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — periodic statistics reporter
 */

#ifndef POC_STATS_H
#define POC_STATS_H

#include "poc_types.h"

/* Print current stats to stdout as a JSON line.  Thread-safe for counters.
 * Resets latency windows after reporting (single-writer per window). */
void poc_stats_report(poc_stats_t *stats, const char *role);

/* Reset all counters to zero. */
void poc_stats_reset(poc_stats_t *stats);

#endif /* POC_STATS_H */
