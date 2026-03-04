/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — InfluxDB push with stream_id tag
 */

#ifndef POC14_INFLUXDB_H
#define POC14_INFLUXDB_H

#include "poc_types.h"
#include <stdbool.h>

/**
 * Initialize the poc_14 InfluxDB client.
 * Uses environment variables: INFLUXDB_URL, INFLUXDB_TOKEN,
 * INFLUXDB_ORG, INFLUXDB_BUCKET, POC_HOST.
 * Returns true if enabled.
 */
bool poc14_influxdb_init(void);

/**
 * Push per-stream stats to InfluxDB with stream_id tag.
 * role: "sender" or "receiver"
 * stream_id: 0..15
 */
void poc14_influxdb_push(poc_stats_t *stats, const char *role, int stream_id);

/**
 * Cleanup InfluxDB resources.
 */
void poc14_influxdb_cleanup(void);

#endif /* POC14_INFLUXDB_H */
