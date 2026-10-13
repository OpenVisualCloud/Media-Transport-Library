/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_TX_VIDEO_RL_HEAD_H_
#define _ST_LIB_TX_VIDEO_RL_HEAD_H_

/* RL pre-arm: packet 0 aims this far past its target, as the shaper releases a queued
 * pkt within -1.7..+2.4 us of the plan, so it is never early */
#define ST_TX_VIDEO_RL_LAUNCH_MARGIN_NS (2 * (uint64_t)NS_PER_US)
/* a pad burst returning later than this past its plan stalled: a healthy one returns
 * well within it, a timer tick costs 2-10 us */
#define ST_TX_VIDEO_RL_STALL_NS (2 * NS_PER_US)
/* max NORMAL pad copies per warm-up or pad retry burst */
#define ST_TX_VIDEO_PAD_BURST (32)
/* bytes, ICE_SCHED_DFLT_BURST_SIZE of the MTL-patched kernel ice (patches/ice_drv) that
 * RL requires; the stock 15 KB bucket would launch packet 0 ~10 pkts early */
#define ST_TX_VIDEO_RL_VF_BURST_SIZE (2048)
/* the last pre-arm pads are cut by up to TRIM_COARSE bytes each, in TRIM_FINE steps, as
 * the shaper drains a pad that short by its bytes */
#define ST_TX_VIDEO_RL_TRIM_COARSE (64)
#define ST_TX_VIDEO_RL_TRIM_FINE (8)

/* the trim_pad[] of a port: the NORMAL pad less TRIM_COARSE or TRIM_FINE bytes */
enum st_tx_video_rl_trim {
  ST_TX_VIDEO_RL_TRIM_PAD_COARSE = 0,
  ST_TX_VIDEO_RL_TRIM_PAD_FINE,
  ST_TX_VIDEO_RL_TRIM_PAD_MAX,
};

#endif
