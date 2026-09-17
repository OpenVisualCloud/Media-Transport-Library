/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * C harness API for the ST 2110-20 (video) RX timing parser unit tests.
 *
 * The harness owns one `st_rx_video_session_impl` configured as 1080p60 over a
 * 90kHz clock with 4320 packets per frame and lets `rv_tp_init()` derive the
 * real pass criteria from it. A frame is described by `struct ut_rvtp_frame`,
 * fed packet by packet through `rv_tp_on_packet()` and then closed by
 * `rv_tp_slot_parse_result()`, so the compliance ladder and the interval stat
 * accumulation both run exactly as they do on the wire.
 *
 * Caveat: `ut_rvtp_feed_frame()` calls `rv_tp_slot_init()` itself, so these
 * tests do not pin production's own per-frame call to it in
 * `rv_slot_by_tmstamp()` (st_rx_video_session.c). Slot reuse without that
 * reset would carry a stale `rtp_ts_delta_valid` into the next frame and no
 * case here would notice.
 */

#ifndef _RX_TIMING_PARSER_HARNESS_H_
#define _RX_TIMING_PARSER_HARNESS_H_

#include <stdint.h>

#include "st_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RTP ticks one frame period advances the harness's 90kHz clock. */
#define UT_RVTP_TICKS_PER_FRAME (1500)

typedef struct ut_rvtp_ctx ut_rvtp_ctx;

/** One synthetic frame on the wire. */
struct ut_rvtp_frame {
  /** epoch (frame period index) the frame belongs to */
  uint64_t epoch;
  /** RTP timestamp every packet of the frame carries */
  uint32_t rtp_tmstamp;
  /** arrival of the first packet, ns after the epoch */
  int32_t fpt_ns;
  /** ns between two consecutive packets */
  uint32_t pkt_spacing_ns;
  /** packets in the frame */
  int pkts;
};

/* Initialise the shared DPDK EAL. Idempotent. Returns 0 on success. */
int ut_rvtp_init(void);

ut_rvtp_ctx* ut_rvtp_create(void);
void ut_rvtp_destroy(ut_rvtp_ctx* ctx);

/* Describe a frame that passes every criterion as NARROW: RTP timestamp on the
 * epoch, first packet `fpt_slack_ns` before tr_offset, perfect pacing. A larger
 * slack yields a larger vrx. */
void ut_rvtp_narrow_frame(const ut_rvtp_ctx* ctx, struct ut_rvtp_frame* frame,
                          uint64_t epoch, int32_t fpt_slack_ns);

/* Feed every packet of `frame` into the session's timing slot and parse it.
 * Returns the frame's `enum st_rx_tp_compliant` verdict. */
enum st_rx_tp_compliant ut_rvtp_feed_frame(ut_rvtp_ctx* ctx,
                                           const struct ut_rvtp_frame* frame);

/* Per-frame results of the most recent ut_rvtp_feed_frame(). */
const char* ut_rvtp_last_cause(const ut_rvtp_ctx* ctx);
int32_t ut_rvtp_last_rtp_ts_delta(const ut_rvtp_ctx* ctx);
int32_t ut_rvtp_last_vrx_max(const ut_rvtp_ctx* ctx);
int32_t ut_rvtp_last_ipt_max(const ut_rvtp_ctx* ctx);

/* Maxima accumulated across the frames of the current stat interval. */
int32_t ut_rvtp_stat_vrx_max(const ut_rvtp_ctx* ctx);
int32_t ut_rvtp_stat_ipt_max(const ut_rvtp_ctx* ctx);

/* Run the production rv_tp_stat(): logs the interval and resets it. */
void ut_rvtp_invoke_stat(ut_rvtp_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* _RX_TIMING_PARSER_HARNESS_H_ */
