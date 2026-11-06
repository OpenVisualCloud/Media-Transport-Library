/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _IPMX_VRX_H_
#define _IPMX_VRX_H_

#include <stdbool.h>
#include <stdint.h>

#include "ipmx/ipmx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TR-10-1 section 8.1 limits and drain rates of one RX session */
struct ipmx_vrx_limit {
  int32_t cmax;
  int32_t vrx_full;
  double cinst_drain_per_ns; /* NPACKETS x 1.1 / TFRAME, ST 2110-21 network model */
  double vrx_drain_per_ns;   /* NPACKETS / ((height / vtotal) x TFRAME) */
};

/* the ST 2110-21 network compatibility bucket of a port, it spans frames */
struct ipmx_vrx_cinst {
  bool started;
  uint64_t t0;
  uint64_t last;
  uint64_t drained;
  int32_t level;
};

/* one frame on one port, all zero before its first packet */
struct ipmx_vrx_frame {
  uint32_t pkts;
  int first_idx;
  bool draining;
  uint64_t drain_start;
  int32_t cinst_max;
  int32_t vrx_min;
  int32_t vrx_max;
};

struct ipmx_vrx_port {
  struct ipmx_vrx_cinst cinst;
  struct ipmx_rx_tp_stat stat;
};

struct ipmx_vrx {
  struct ipmx_vrx_limit limit;
  struct ipmx_vrx_port port[MTL_SESSION_PORT_MAX];
};

/* cmax is the timing parser's cinst_max_wide; ractive is height / vtotal */
void ipmx_vrx_init(struct ipmx_vrx* vrx, int npackets, double frame_time_ns,
                   double ractive, int32_t cmax);
void ipmx_vrx_on_packet(struct ipmx_vrx* vrx, enum mtl_session_port port,
                        struct ipmx_vrx_frame* frame, int pkt_idx, uint64_t pkt_ns);
/* NULL if the frame is within the limits, else the cause */
const char* ipmx_vrx_frame_check(const struct ipmx_vrx_limit* limit,
                                 const struct ipmx_vrx_frame* frame);
/* fold a finished frame into the port stat, judged only if the port got all of it */
void ipmx_vrx_frame_done(struct ipmx_vrx* vrx, enum mtl_session_port port,
                         const struct ipmx_vrx_frame* frame, bool complete);
/* the stat since the timing parser started, for its periodic dump */
void ipmx_vrx_stat_dump(const struct ipmx_vrx* vrx, int idx, enum mtl_session_port port);

struct st_rx_video_session_impl;
int ipmx_vrx_stat_get(struct st_rx_video_session_impl* s, enum mtl_session_port port,
                      struct ipmx_rx_tp_stat* stat);

#ifdef __cplusplus
}
#endif

#endif
