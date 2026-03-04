/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — InfluxDB v2 line-protocol push
 *
 * Optional: when INFLUXDB_URL env-var is set, every stats-report
 * cycle also HTTP-POSTs metrics to InfluxDB.  Requires libcurl.
 *
 * Environment variables:
 *   INFLUXDB_URL    – e.g. "http://localhost:8086"  (required to enable)
 *   INFLUXDB_TOKEN  – API token                     (required)
 *   INFLUXDB_ORG    – organization   (default "mtl-mxl")
 *   INFLUXDB_BUCKET – bucket         (default "poc")
 *   POC_HOST        – host tag       (default hostname)
 */

#ifndef POC_INFLUXDB_H
#define POC_INFLUXDB_H

#include "poc_types.h"
#include <stdbool.h>

/* One-time init.  Returns true if InfluxDB push is enabled. */
bool poc_influxdb_init(void);

/* Push current stats snapshot as InfluxDB line-protocol points.
 * Silently returns if InfluxDB is not configured. */
void poc_influxdb_push(const poc_stats_t *stats, const char *role);

/* Cleanup (curl teardown). */
void poc_influxdb_cleanup(void);

#endif /* POC_INFLUXDB_H */
