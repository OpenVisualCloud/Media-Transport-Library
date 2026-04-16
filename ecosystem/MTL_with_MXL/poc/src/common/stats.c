/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — periodic statistics reporter
 */

#include <stdio.h>
#include <time.h>

#include "poc_influxdb.h"
#include "poc_stats.h"

void poc_stats_report(poc_stats_t* stats, const char* role) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  /* Snapshot latency windows (single-writer, so no lock needed) */
  poc_latency_t lq = stats->lat_queue;
  poc_latency_t lb = stats->lat_bridge;
  poc_latency_t lr = stats->lat_rdma;
  poc_latency_t lt = stats->lat_total;
  poc_latency_t lc = stats->lat_consume;
  poc_latency_t le = stats->lat_e2e;

  printf(
      "{\"role\":\"%s\","
      "\"time_s\":%ld.%03ld,"
      "\"rx_frames\":%lu,"
      "\"rx_incomplete\":%lu,"
      "\"rx_drops\":%lu,"
      "\"bridge_frames\":%lu,"
      "\"bridge_copies\":%lu,"
      "\"fabrics_transfers\":%lu,"
      "\"target_events\":%lu,"
      "\"consumer_frames\":%lu,"
      "\"lat_queue_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu},"
      "\"lat_bridge_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu},"
      "\"lat_rdma_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu},"
      "\"lat_total_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu},"
      "\"lat_consume_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu},"
      "\"lat_e2e_us\":{\"min\":%lu,\"max\":%lu,\"avg\":%lu,\"n\":%lu}}\n",
      role, ts.tv_sec, ts.tv_nsec / 1000000,
      (unsigned long)atomic_load(&stats->rx_frames),
      (unsigned long)atomic_load(&stats->rx_incomplete),
      (unsigned long)atomic_load(&stats->rx_drops),
      (unsigned long)atomic_load(&stats->bridge_frames),
      (unsigned long)atomic_load(&stats->bridge_copies),
      (unsigned long)atomic_load(&stats->fabrics_transfers),
      (unsigned long)atomic_load(&stats->target_events),
      (unsigned long)atomic_load(&stats->consumer_frames),
      (unsigned long)(lq.count ? lq.min_us : 0), (unsigned long)lq.max_us,
      (unsigned long)poc_latency_avg(&lq), (unsigned long)lq.count,
      (unsigned long)(lb.count ? lb.min_us : 0), (unsigned long)lb.max_us,
      (unsigned long)poc_latency_avg(&lb), (unsigned long)lb.count,
      (unsigned long)(lr.count ? lr.min_us : 0), (unsigned long)lr.max_us,
      (unsigned long)poc_latency_avg(&lr), (unsigned long)lr.count,
      (unsigned long)(lt.count ? lt.min_us : 0), (unsigned long)lt.max_us,
      (unsigned long)poc_latency_avg(&lt), (unsigned long)lt.count,
      (unsigned long)(lc.count ? lc.min_us : 0), (unsigned long)lc.max_us,
      (unsigned long)poc_latency_avg(&lc), (unsigned long)lc.count,
      (unsigned long)(le.count ? le.min_us : 0), (unsigned long)le.max_us,
      (unsigned long)poc_latency_avg(&le), (unsigned long)le.count);
  fflush(stdout);

  /* Push to InfluxDB (no-op if not configured) */
  poc_influxdb_push(stats, role);

  /* Reset latency windows for next reporting period */
  poc_latency_reset(&stats->lat_queue);
  poc_latency_reset(&stats->lat_bridge);
  poc_latency_reset(&stats->lat_rdma);
  poc_latency_reset(&stats->lat_total);
  poc_latency_reset(&stats->lat_consume);
  poc_latency_reset(&stats->lat_e2e);
}

void poc_stats_reset(poc_stats_t* stats) {
  atomic_store(&stats->rx_frames, 0);
  atomic_store(&stats->rx_incomplete, 0);
  atomic_store(&stats->rx_drops, 0);
  atomic_store(&stats->bridge_frames, 0);
  atomic_store(&stats->bridge_copies, 0);
  atomic_store(&stats->fabrics_transfers, 0);
  atomic_store(&stats->target_events, 0);
  atomic_store(&stats->consumer_frames, 0);
  poc_latency_reset(&stats->lat_queue);
  poc_latency_reset(&stats->lat_bridge);
  poc_latency_reset(&stats->lat_rdma);
  poc_latency_reset(&stats->lat_total);
  poc_latency_reset(&stats->lat_consume);
  poc_latency_reset(&stats->lat_e2e);
}
