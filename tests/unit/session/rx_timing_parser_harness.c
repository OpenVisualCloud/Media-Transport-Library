/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-20 (video) RX timing parser unit tests.
 *
 * Session geometry: 1080p60, 90kHz clock, 4320 packets per frame
 *   frame_time          = 1e9 / 60      = 16666666.67 ns
 *   frame_time_sampling = 90000 / 60    = 1500 ticks
 *   trs                 = frame_time * (1080/1125) / 4320 = 3703.70 ns
 *
 * rv_tp_init() derives trs and every pass criterion from that, so the harness
 * pins no limit value of its own.
 */

#include <stdlib.h>

#undef MTL_HAS_USDT
#include "common/ut_common.h"
#include "st2110/st_rx_timing_parser.c"

#define UT_RVTP_TOTAL_PKTS (4320)
/* Packets the harness sends per frame (the geometry declares 4320). */
#define UT_RVTP_PKTS_PER_FRAME (8)
/* Packet spacing of a perfectly paced frame, ns, rounded up from trs. */
#define UT_RVTP_TRS_NS (3704)

struct ut_rvtp_ctx {
  struct mtl_main_impl impl;
  struct st_rx_video_session_impl session;
};

#include "session/rx_timing_parser_harness.h"

static struct st_rv_tp_slot* ut_rvtp_slot(const ut_rvtp_ctx* ctx) {
  return &ctx->session.tp->slots[0][MTL_SESSION_PORT_P];
}

int ut_rvtp_init(void) {
  return ut_eal_init();
}

ut_rvtp_ctx* ut_rvtp_create(void) {
  ut_rvtp_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  ctx->impl.type = MT_HANDLE_MAIN;

  struct st_rx_video_session_impl* s = &ctx->session;
  s->impl = &ctx->impl;
  s->idx = 0;
  s->ops.num_port = 1;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.fps = ST_FPS_P60;
  s->frame_time = (double)NS_PER_S / 60;
  s->frame_time_sampling = (double)ST10_VIDEO_SAMPLING_RATE_90K / 60;
  s->detector.pkt_per_frame = UT_RVTP_TOTAL_PKTS;
  s->enable_timing_parser = true;
  s->enable_timing_parser_stat = true;

  if (rv_tp_init(&ctx->impl, s) < 0) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

void ut_rvtp_destroy(ut_rvtp_ctx* ctx) {
  if (!ctx) return;
  rv_tp_uinit(&ctx->session);
  free(ctx);
}

void ut_rvtp_narrow_frame(const ut_rvtp_ctx* ctx, struct ut_rvtp_frame* frame,
                          uint64_t epoch, int32_t fpt_slack_ns) {
  frame->epoch = epoch;
  frame->rtp_tmstamp = (uint32_t)(epoch * UT_RVTP_TICKS_PER_FRAME);
  frame->fpt_ns = ctx->session.tp->pass.tr_offset - fpt_slack_ns;
  frame->pkt_spacing_ns = UT_RVTP_TRS_NS;
  frame->pkts = UT_RVTP_PKTS_PER_FRAME;
}

enum st_rx_tp_compliant ut_rvtp_feed_frame(ut_rvtp_ctx* ctx,
                                           const struct ut_rvtp_frame* frame) {
  struct st_rx_video_session_impl* s = &ctx->session;
  struct st_rv_tp_slot* slot = ut_rvtp_slot(ctx);
  uint64_t epoch_ns = (uint64_t)((double)frame->epoch * s->frame_time);
  uint64_t first_pkt_ns = epoch_ns + frame->fpt_ns;

  rv_tp_slot_init(slot);
  for (int i = 0; i < frame->pkts; i++) {
    rv_tp_on_packet(s, MTL_SESSION_PORT_P, slot, frame->rtp_tmstamp,
                    first_pkt_ns + (uint64_t)frame->pkt_spacing_ns * i, i);
  }
  rv_tp_slot_parse_result(s, MTL_SESSION_PORT_P, slot);

  return slot->meta.compliant;
}

const char* ut_rvtp_last_cause(const ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.failed_cause;
}

int32_t ut_rvtp_last_rtp_ts_delta(const ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.rtp_ts_delta;
}

int32_t ut_rvtp_last_vrx_max(const ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.vrx_max;
}

int32_t ut_rvtp_last_ipt_max(const ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.ipt_max;
}

int32_t ut_rvtp_stat_vrx_max(const ut_rvtp_ctx* ctx) {
  return ctx->session.tp->stat[MTL_SESSION_PORT_P].slot.meta.vrx_max;
}

int32_t ut_rvtp_stat_ipt_max(const ut_rvtp_ctx* ctx) {
  return ctx->session.tp->stat[MTL_SESSION_PORT_P].slot.meta.ipt_max;
}

void ut_rvtp_invoke_stat(ut_rvtp_ctx* ctx) {
  rv_tp_stat(&ctx->session);
}
