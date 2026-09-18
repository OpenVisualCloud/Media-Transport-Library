/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness for the ST 2110-21 RX timing parser unit tests.
 *
 * Only the internal headers are included, not st_rx_timing_parser.c: every
 * entry point under test is non-static and resolves from libmtl.so, so the
 * tests exercise the shipped object rather than a private recompile of it.
 *
 * The session is hand-populated instead of going through st20_rx_create():
 * rv_tp_init() only reads ops.{fps,width,height,interlaced}, frame_time,
 * frame_time_sampling and detector.pkt_per_frame, and reaches the platform
 * solely through mt_socket_id() (an impl->inf[] field read) and
 * mt_rte_zmalloc_socket(). So an initialised EAL is the only dependency -- no
 * NIC, no queue, no PTP, no mtl_init().
 *
 * Arrival times are handed to rv_tp_on_packet() directly, which keeps
 * mt_mbuf_time_stamp() and the whole mbuf path out of the picture.
 */

#include "session/st20_rx_tp_harness.h"

#include <stdlib.h>

#include "common/ut_common.h"
#include "st2110/st_rx_timing_parser.h"

/* 1080p60 YUV422-10bit, the geometry of tests/single/rx_timing/video. */
#define UT_RVTP_FPS (ST_FPS_P60)
#define UT_RVTP_FRAME_TIME (1000000000.0 / 60)
#define UT_RVTP_FRAME_TIME_SAMPLING (90000.0 / 60)
#define UT_RVTP_PKTS_PER_FRAME (4115)

/* Media epoch whose 32-bit RTP timestamp is exactly 0: 2^30 * 1500 is a whole
 * multiple of 2^32, so this is a real media clock wrap point. */
#define UT_RVTP_WRAP_EPOCH ((uint64_t)1 << 30)

struct ut_rvtp_ctx {
  struct mtl_main_impl impl;
  struct st_rx_video_session_impl session;
};

int ut_rvtp_init(void) {
  return ut_eal_init();
}

ut_rvtp_ctx* ut_rvtp_create(void) {
  ut_rvtp_ctx* ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  struct st_rx_video_session_impl* s = &ctx->session;

  s->idx = 0;
  s->impl = &ctx->impl;
  s->port_maps[MTL_SESSION_PORT_P] = MTL_PORT_P;
  s->ops.num_port = 1;
  s->ops.fps = UT_RVTP_FPS;
  s->ops.width = 1920;
  s->ops.height = 1080;
  s->ops.interlaced = false;
  s->frame_time = UT_RVTP_FRAME_TIME;
  s->frame_time_sampling = UT_RVTP_FRAME_TIME_SAMPLING;
  s->detector.pkt_per_frame = UT_RVTP_PKTS_PER_FRAME;

  if (rv_tp_init(&ctx->impl, s) < 0) {
    free(ctx);
    return NULL;
  }
  /* only safe once s->tp exists, the flag is an unguarded s->tp dereference */
  s->enable_timing_parser = true;

  return ctx;
}

void ut_rvtp_destroy(ut_rvtp_ctx* ctx) {
  if (!ctx) return;
  rv_tp_uinit(&ctx->session);
  free(ctx);
}

uint32_t ut_rvtp_tick_ns(void) {
  return (uint32_t)(UT_RVTP_FRAME_TIME / UT_RVTP_FRAME_TIME_SAMPLING);
}

uint64_t ut_rvtp_wrap_epoch(void) {
  return UT_RVTP_WRAP_EPOCH;
}

static struct st_rv_tp_slot* ut_rvtp_slot(ut_rvtp_ctx* ctx) {
  return &ctx->session.tp->slots[0][MTL_SESSION_PORT_P];
}

/* Emit the packets of one frame at the ideal ST 2110-21 gapped arrival time --
 * packet i at epoch + fpt + trs * i -- then take the parser's verdict the way
 * rv_frame_notify() does.
 *
 * @dropped models an rx burst that rv_tp_pkt_handle() withheld as untrusted:
 * those packets never reach the parser, and the one that anchors the frame in
 * their place arrived inside the same burst, so it lands at the frame's first
 * packet time instead of its own gapped slot. */
static int ut_rvtp_feed(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                        uint32_t rtp_tmstamp, int dropped) {
  struct st_rx_video_session_impl* s = &ctx->session;
  struct st_rv_tp_slot* slot = ut_rvtp_slot(ctx);
  uint64_t epoch_time = (uint64_t)((double)epoch * s->frame_time);
  double trs = s->tp->trs;

  rv_tp_slot_init(slot);
  for (int i = dropped; i < UT_RVTP_PKTS_PER_FRAME; i++) {
    uint64_t pkt_time = epoch_time + fpt + (i == dropped ? 0 : (uint64_t)(trs * i));

    rv_tp_on_packet(s, MTL_SESSION_PORT_P, slot, rtp_tmstamp, pkt_time, i);
  }
  rv_tp_slot_parse_result(s, MTL_SESSION_PORT_P, slot);

  return slot->meta.compliant;
}

static uint32_t ut_rvtp_epoch_tmstamp(uint64_t epoch) {
  return (uint32_t)(uint64_t)((double)epoch * UT_RVTP_FRAME_TIME_SAMPLING);
}

int ut_rvtp_feed_frame(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                       int32_t rtp_offset) {
  return ut_rvtp_feed(ctx, epoch, fpt, ut_rvtp_epoch_tmstamp(epoch) + rtp_offset, 0);
}

int ut_rvtp_feed_frame_rtp(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                           uint32_t rtp_tmstamp) {
  return ut_rvtp_feed(ctx, epoch, fpt, rtp_tmstamp, 0);
}

int ut_rvtp_feed_frame_burst(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                             int32_t rtp_offset, int dropped) {
  return ut_rvtp_feed(ctx, epoch, fpt, ut_rvtp_epoch_tmstamp(epoch) + rtp_offset,
                      dropped);
}

const char* ut_rvtp_last_cause(ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.failed_cause;
}

int32_t ut_rvtp_last_latency(ut_rvtp_ctx* ctx) {
  return ut_rvtp_slot(ctx)->meta.latency;
}
