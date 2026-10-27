/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

#include "ipmx_vrx.h"

#include <stdio.h>
#include <string.h>

#include "../mt_log.h"

/* ST 2110-21 network compatibility model drain factor */
#define IPMX_CINST_BETA (1.1)

void ipmx_vrx_init(struct ipmx_vrx* vrx, int npackets, double frame_time_ns,
                   double ractive, int32_t cmax) {
  struct ipmx_vrx_limit* limit = &vrx->limit;

  memset(vrx, 0, sizeof(*vrx));
  limit->cmax = cmax;
  limit->vrx_full = cmax * 2;
  limit->cinst_drain_per_ns = npackets * IPMX_CINST_BETA / frame_time_ns;
  limit->vrx_drain_per_ns = npackets / (ractive * frame_time_ns);

  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    struct ipmx_rx_tp_stat* stat = &vrx->port[i].stat;

    stat->cmax = limit->cmax;
    stat->vrx_full = limit->vrx_full;
    stat->vrx_max = INT32_MIN;
    stat->vrx_min = INT32_MAX;
  }
}

/* ST 2110-21 CINST as the VSF test tool runs it, a port bucket never below empty; the
 * timing parser's frame anchored Cinst has no empty floor and misses bursts after idle */
static int32_t ipmx_vrx_cinst(const struct ipmx_vrx_limit* limit,
                              struct ipmx_vrx_cinst* c, uint64_t pkt_ns) {
  /* re-anchor on a clock stepped back, keeping the level; forward as the VSF tool */
  if (!c->started || pkt_ns < c->last) {
    c->started = true;
    c->t0 = pkt_ns;
    c->drained = 0;
  }
  c->last = pkt_ns;
  if (pkt_ns > c->t0) {
    uint64_t drained = (double)(pkt_ns - c->t0) * limit->cinst_drain_per_ns;

    if (drained > c->drained) {
      uint64_t n = drained - c->drained;

      c->level = n >= (uint64_t)c->level ? 0 : c->level - (int32_t)n;
      c->drained = drained;
    }
  }
  return ++c->level;
}

/* Level after each arrival by packet position, draining from the CMAX-th packet. 0 is a
 * packet up to one drain period late, as the ST 2110-21 VRX; below 0 is an underflow. */
void ipmx_vrx_on_packet(struct ipmx_vrx* vrx, enum mtl_session_port port,
                        struct ipmx_vrx_frame* frame, int pkt_idx, uint64_t pkt_ns) {
  const struct ipmx_vrx_limit* limit = &vrx->limit;
  int32_t cinst = ipmx_vrx_cinst(limit, &vrx->port[port].cinst, pkt_ns);
  int32_t pos, level;

  if (!frame->pkts) frame->first_idx = pkt_idx;
  frame->pkts++;
  if (cinst > frame->cinst_max) frame->cinst_max = cinst;

  pos = pkt_idx - frame->first_idx + 1;
  if (!frame->draining) {
    if (pos < limit->cmax) return;
    frame->draining = true;
    frame->drain_start = pkt_ns;
    frame->vrx_min = pos;
    frame->vrx_max = pos;
    return;
  }

  level = pos;
  if (pkt_ns > frame->drain_start)
    level -= (int32_t)((double)(pkt_ns - frame->drain_start) * limit->vrx_drain_per_ns);
  if (level < frame->vrx_min) frame->vrx_min = level;
  if (level > frame->vrx_max) frame->vrx_max = level;
}

const char* ipmx_vrx_frame_check(const struct ipmx_vrx_limit* limit,
                                 const struct ipmx_vrx_frame* frame) {
  if (frame->cinst_max > limit->cmax) return "cinst exceed cmax";
  if (!frame->draining) return NULL;
  if (frame->vrx_max > limit->vrx_full) return "vrx exceed vrx_full";
  if (frame->vrx_min < 0) return "vrx underflow";
  return NULL;
}

void ipmx_vrx_frame_done(struct ipmx_vrx* vrx, enum mtl_session_port port,
                         const struct ipmx_vrx_frame* frame, bool complete) {
  struct ipmx_rx_tp_stat* stat = &vrx->port[port].stat;
  const char* cause;

  if (!complete || !frame->pkts) {
    stat->frames_unmeasured++;
    return;
  }

  cause = ipmx_vrx_frame_check(&vrx->limit, frame);
  if (cause) {
    stat->frames_failed++;
    snprintf(stat->failed_cause, sizeof(stat->failed_cause), "%s", cause);
  } else {
    stat->frames_compliant++;
  }
  if (frame->cinst_max > stat->cinst_max) stat->cinst_max = frame->cinst_max;
  if (!frame->draining) return;
  if (frame->vrx_max > stat->vrx_max) stat->vrx_max = frame->vrx_max;
  if (frame->vrx_min < stat->vrx_min) stat->vrx_min = frame->vrx_min;
}

void ipmx_vrx_stat_dump(const struct ipmx_vrx* vrx, int idx, enum mtl_session_port port) {
  const struct ipmx_rx_tp_stat* stat = &vrx->port[port].stat;

  info(
      "%s(%d,%d), IPMX COMPLIANT %u FAILED %u UNMEASURED %u, CINST MAX %d CMAX %d, VRX "
      "MIN %d MAX %d FULL %d\n",
      __func__, idx, port, stat->frames_compliant, stat->frames_failed,
      stat->frames_unmeasured, stat->cinst_max, stat->cmax, stat->vrx_min, stat->vrx_max,
      stat->vrx_full);
  if (stat->frames_failed)
    info("%s(%d,%d), IPMX last failed cause %s\n", __func__, idx, port,
         stat->failed_cause);
}
