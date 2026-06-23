/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — shared type definitions
 */

#ifndef POC_TYPES_H
#define POC_TYPES_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "poc_latency.h"

/* ── Video parameters ── */
typedef struct {
  uint32_t width;       /* e.g. 1920 */
  uint32_t height;      /* e.g. 1080 */
  uint32_t fps_num;     /* e.g. 30000 */
  uint32_t fps_den;     /* e.g. 1001 */
  uint32_t v210_stride; /* bytes per line, computed from width */
  uint32_t frame_size;  /* stride * height */
} poc_video_params_t;

/* ── Configuration (CLI / JSON) ── */
typedef struct {
  /* MTL */
  char mtl_port[64];    /* PCIe BDF, e.g. "0000:31:01.0" (VF) */
  uint8_t sip[4];       /* source IP for DPDK port */
  uint8_t rx_ip[4];     /* multicast/unicast group */
  uint16_t rx_udp_port; /* UDP port */
  uint8_t payload_type; /* RTP payload type (96..127) */

  /* ST 2022-7 Redundancy (R path) */
  bool redundancy;        /* enable ST 2022-7 dual-path */
  char mtl_port_r[64];    /* PCIe BDF for redundant DPDK port */
  uint8_t sip_r[4];       /* source IP for redundant port */
  uint8_t rx_ip_r[4];     /* multicast group for R path */
  uint16_t rx_udp_port_r; /* UDP port for R path */

  /* Video */
  poc_video_params_t video;

  /* MXL */
  char mxl_domain[256];     /* tmpfs path for MXL ringbuffers */
  char flow_json_path[512]; /* path to v210 flow definition JSON */
  uint32_t grain_count;     /* ring size (4..32) */

  /* Fabrics */
  char fabrics_provider[32];   /* "tcp" or "verbs" */
  char fabrics_local_ip[64];   /* local bind IP for fabrics */
  char fabrics_local_port[16]; /* local bind port ("0" = ephemeral) */
  char target_info_str[8192];  /* serialized targetInfo from Host B (sender only) */

  /* Runtime */
  uint32_t framebuff_cnt; /* MTL RX frame buffer count (2..8) */
  uint32_t queue_depth;   /* frame queue depth for bridge */
  bool accept_incomplete; /* forward incomplete frames as invalid grains */
  int worker_cpu;         /* CPU core to pin bridge worker (-1 = no pin) */
  int consumer_cpu;       /* CPU core to pin consumer thread (-1 = no pin) */
  char mtl_lcores[128];   /* MTL DPDK lcore list, e.g. "2,3" (empty=auto) */
  int duration_sec;       /* run duration, 0 = infinite */
} poc_config_t;

/* ── Frame queue entry ── */
typedef struct {
  void* frame_addr;          /* MTL hugepage buffer address */
  uint64_t timestamp;        /* RTP / PTP timestamp */
  uint64_t flow_index;       /* MXL grain flow index (zero-copy) */
  uint32_t frame_recv_size;  /* actual bytes received */
  uint32_t frame_total_size; /* expected total size */
  bool complete;             /* full frame received? */
  uint64_t enqueue_ns;       /* CLOCK_REALTIME ns when frame entered queue */
} poc_frame_entry_t;

/* ── Statistics ── */
typedef struct {
  atomic_uint_fast64_t rx_frames;
  atomic_uint_fast64_t rx_incomplete;
  atomic_uint_fast64_t rx_drops; /* queue-full drops */
  atomic_uint_fast64_t bridge_frames;
  atomic_uint_fast64_t bridge_copies;
  atomic_uint_fast64_t fabrics_transfers;
  atomic_uint_fast64_t target_events;
  atomic_uint_fast64_t consumer_frames;

  /* ── Latency windows (µs, reset every reporting period) ──
   * Single-writer: sender-side populated by bridge thread,
   *                receiver-side populated by consumer thread. */
  poc_latency_t lat_queue;   /* T1 (enqueue) → T2 (dequeue) */
  poc_latency_t lat_bridge;  /* T2 (dequeue) → T2b (RDMA posted) */
  poc_latency_t lat_rdma;    /* T2b (RDMA posted) → T3 (RDMA complete) */
  poc_latency_t lat_total;   /* T1 (enqueue) → T3 (RDMA complete) */
  poc_latency_t lat_consume; /* receiver: RDMA event → grain committed */
  poc_latency_t lat_e2e;     /* receiver: sender T1 → receiver event (PTP-synced) */
} poc_stats_t;

#endif /* POC_TYPES_H */
