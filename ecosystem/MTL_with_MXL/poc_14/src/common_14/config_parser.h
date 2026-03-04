/* SPDX-License-Identifier: BSD-3-Clause
 * poc_14 — JSON config parser for 14-stream demo
 */

#ifndef POC14_CONFIG_PARSER_H
#define POC14_CONFIG_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#define POC14_MAX_STREAMS 16
#define POC14_MAX_LABEL   64

/* ── Per-stream configuration ── */
typedef struct {
    int      id;
    char     label[POC14_MAX_LABEL];
    char     source[16];              /* "synthetic" or "camera" */
    uint8_t  multicast_ip[4];         /* multicast group destination */
    uint8_t  camera_src_ip[4];        /* camera source IP for SSM (0.0.0.0 if synthetic) */
    uint16_t udp_port;
    char     flow_json_path[512];
    char     fabrics_local_port[16];
    int      worker_cpu;              /* bridge worker core (-1 = auto) */
    int      consumer_cpu;            /* consumer thread core (-1 = auto) */
} poc14_stream_config_t;

/* ── Global + per-stream configuration ── */
typedef struct {
    /* Global (shared by all streams) */
    char     mtl_port[64];            /* PCIe BDF for RX VF */
    uint8_t  sip[4];                  /* source IP on NIC */
    char     mxl_domain[256];         /* MXL tmpfs path */
    char     fabrics_provider[32];    /* "tcp" or "verbs" */
    char     fabrics_local_ip[64];    /* fabrics bind IP */
    char     mtl_lcores[128];         /* DPDK lcore list */
    int      framebuff_cnt;           /* MTL RX frame buffer count per session */
    bool     accept_incomplete;       /* forward incomplete frames */
    int      duration_sec;            /* run duration, 0 = infinite */

    /* Video (common to all streams) */
    uint32_t video_width;
    uint32_t video_height;
    uint32_t fps_num;
    uint32_t fps_den;

    /* TX-specific globals */
    char     tx_port[64];             /* PCIe BDF for TX VF */
    uint8_t  tx_sip[4];              /* TX source IP */
    char     tx_lcores[128];          /* TX DPDK lcores */
    uint8_t  payload_type;            /* RTP payload type */

    /* Easy-switch knob: limit to first N streams (0 = use all in array) */
    uint32_t active_streams;

    /* Auto CPU assignment: first CPU to use (worker_cpu/consumer_cpu = -1) */
    int      auto_cpu_start;          /* default 21 — avoids CPUs 0-20 */

    /* Per-stream */
    uint32_t            num_streams;
    poc14_stream_config_t streams[POC14_MAX_STREAMS];
} poc14_config_t;

/**
 * Parse a streams_14.json config file into poc14_config_t.
 * Returns 0 on success, -1 on error.
 */
int poc14_config_parse(const char *json_path, poc14_config_t *out);

/**
 * Print parsed config summary to stdout.
 */
void poc14_config_print(const poc14_config_t *cfg);

#endif /* POC14_CONFIG_PARSER_H */
