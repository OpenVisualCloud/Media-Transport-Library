/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the ST 2110-21 RX timing parser unit tests.
 *
 * Wraps one st_rx_video_session_impl configured as 1080p60 YUV422-10bit, 4115
 * packets per frame -- the geometry the nightly rx_timing acceptance suite runs
 * -- and feeds it synthetic packet arrival times. MTL internal types stay
 * opaque so the C++ test layer never includes libmtl headers.
 *
 * ut_rvtp_feed_frame*() emits every packet of one frame at the ideal
 * ST 2110-21 gapped arrival time, then asks the parser for its verdict exactly
 * as rv_frame_notify() does in production.
 */

#ifndef _ST20_RX_TP_HARNESS_H_
#define _ST20_RX_TP_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "mtl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mirrors enum st_rx_tp_compliant so the test layer needs no MTL header. */
enum ut_rvtp_compliant {
  UT_RVTP_FAILED = 0,
  UT_RVTP_WIDE = 1,
  UT_RVTP_NARROW = 2,
};

/**
 * First packet time and RTP offset that MTL's own TX produces for this
 * geometry, as measured by the nightly rx_timing runs on real hardware. A frame
 * fed with this pair is narrow compliant with room to spare on every
 * ST 2110-21 metric: vrx 4 of 9, cinst 0 of 5, rtp_offset 56 of 59.
 */
#define UT_RVTP_FPT_NOMINAL (625203)
#define UT_RVTP_RTP_OFFSET_NOMINAL (56)

/** Media epoch to start a stream at. Must be non-zero: rv_tp_on_packet()
 * uses a zero cur_epochs as its "first packet of this frame" marker. */
#define UT_RVTP_EPOCH_BASE ((uint64_t)1000000)

typedef struct ut_rvtp_ctx ut_rvtp_ctx;

/** Initialise DPDK EAL. Idempotent, shared with the other harnesses. */
int ut_rvtp_init(void);

/** Create a 1080p60 session with the timing parser enabled. */
ut_rvtp_ctx* ut_rvtp_create(void);
void ut_rvtp_destroy(ut_rvtp_ctx* ctx);

/** Nanoseconds per media clock tick, i.e. the quantum the sender's RTP
 * timestamp -- and so every measurement derived from it -- is rounded to. */
uint32_t ut_rvtp_tick_ns(void);

/** Media epoch whose 32-bit RTP timestamp is exactly 0, i.e. a real media
 * clock wrap. A live stream reaches one every 2^32 / 90000 ~= 13.2 hours. */
uint64_t ut_rvtp_wrap_epoch(void);

/**
 * Feed one frame for media epoch @epoch whose packets arrive at the ideal
 * gapped spacing starting @fpt ns after the epoch, with the sender's RTP
 * timestamp @rtp_offset ticks past that epoch. Returns the frame's
 * enum ut_rvtp_compliant verdict.
 */
int ut_rvtp_feed_frame(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt, int32_t rtp_offset);

/** As ut_rvtp_feed_frame(), but the sender's RTP timestamp is given outright
 * rather than derived from @epoch. Use to build rtp_ts_delta corner cases. */
int ut_rvtp_feed_frame_rtp(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                           uint32_t rtp_tmstamp);

/**
 * As ut_rvtp_feed_frame(), but the frame's first @dropped packets arrived in an
 * rx burst that rv_tp_pkt_handle() withheld from the parser as untrusted. The
 * packet that anchors the frame in their place therefore has pkt_idx @dropped
 * and arrived compressed against them, at the frame's first packet time.
 */
int ut_rvtp_feed_frame_burst(ut_rvtp_ctx* ctx, uint64_t epoch, int32_t fpt,
                             int32_t rtp_offset, int dropped);

/** failed_cause the parser recorded for the most recently fed frame. */
const char* ut_rvtp_last_cause(ut_rvtp_ctx* ctx);
/** latency(ns) the parser measured for the most recently fed frame. */
int32_t ut_rvtp_last_latency(ut_rvtp_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif
