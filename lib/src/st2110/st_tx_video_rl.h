/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _ST_LIB_TX_VIDEO_RL_HEAD_H_
#define _ST_LIB_TX_VIDEO_RL_HEAD_H_

#include <math.h>
#include <rte_common.h>
#include <stdbool.h>
#include <stdint.h>

#define ST_TX_VIDEO_RL_MAX_PADS_PER_BURST (32)
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

/* ice_sched_bw_to_rl_profile() picks the shortest timeslot, 32 << encode psm_clk cycles,
 * that adds more than 128 B of tokens. That is twice what the next shorter one adds, so
 * under 2 * 128.5 B, unless even the shortest (encode 0) adds more. */
#define ST_TX_VIDEO_RL_TOKENS_MAX (257)
/* ICE_PSM_CLK_367MHZ_IN_HZ, the slowest psm_clk ice reads */
#define ST_TX_VIDEO_RL_PSM_CLK_MIN (367647059)
/* The longest token timeslot, bounded by aim, a plan accepts: packet 0 then leaves up to
 * 2 * aim later than on the hold path, and a drain below ~128.5 MB/s holds. */
#define ST_TX_VIDEO_RL_TIMESLOT_MAX_NS (2 * NS_PER_US)

/* the most bytes ice adds per token timeslot on a queue programmed at bps bytes/s */
static inline long double st_tx_video_rl_tokens(uint64_t bps) {
  return RTE_MAX((long double)ST_TX_VIDEO_RL_TOKENS_MAX,
                 (long double)bps / (ST_TX_VIDEO_RL_PSM_CLK_MIN / 32.0L) + 0.5L);
}

/* the RL pre-arm plan of a port, set by tv_rl_plan_init() */
struct st_tx_video_rl_plan {
  long double pad_ns;    /* shaper time of one NORMAL pad, 0 without a plan */
  long double bucket_ns; /* shaper time of a full bucket plus the timeslot aim */
  int64_t trim_pads;     /* st_tx_video_rl_trim_pads() of pad_size */
  uint16_t pad_size;     /* bytes of the NORMAL pad */
  bool armed;            /* false: packet 0 is held until its target */
};

/* a pre-arm burst of n NORMAL pads, the first coarse of them TRIM_COARSE bytes short and
 * the next fine ones TRIM_FINE bytes short */
struct st_tx_video_rl_burst {
  unsigned int n;
  unsigned int coarse;
  unsigned int fine;
  bool last; /* the burst that reaches launch, the only one with cut pads */
};

/* the pads the last burst keeps, enough to cut up to one pad: pad_size / TRIM_COARSE
 * coarse cuts, then TRIM_COARSE / TRIM_FINE fine ones, at most
 * ST_TX_VIDEO_RL_MAX_PADS_PER_BURST */
static inline int64_t st_tx_video_rl_trim_pads(uint16_t pad_size) {
  return RTE_MIN(pad_size / ST_TX_VIDEO_RL_TRIM_COARSE +
                     ST_TX_VIDEO_RL_TRIM_COARSE / ST_TX_VIDEO_RL_TRIM_FINE,
                 ST_TX_VIDEO_RL_MAX_PADS_PER_BURST);
}

/* Plans the next pre-arm burst. drain is the shaper time of one NORMAL pad, launch when
 * packet 0 should start and drained when the shaper finishes what is queued, all in ns.
 * While more than trim_pads pads are needed, the burst is up to
 * ST_TX_VIDEO_RL_MAX_PADS_PER_BURST of them, keeping trim_pads for the last one, which
 * rounds up to whole pads and cuts the overshoot in whole fine trims. */
static inline struct st_tx_video_rl_burst st_tx_video_rl_next_burst(long double drain,
                                                                    uint16_t pad_size,
                                                                    int64_t trim_pads,
                                                                    long double launch,
                                                                    long double drained) {
  struct st_tx_video_rl_burst b = {0};
  int64_t needed = (int64_t)ceill((launch - drained) / drain);
  unsigned int n;

  b.last = needed <= trim_pads;
  n = b.last ? needed : RTE_MIN(needed - trim_pads, ST_TX_VIDEO_RL_MAX_PADS_PER_BURST);
  b.n = n;
  if (b.last) {
    uint32_t cut = (uint32_t)((n * drain - (launch - drained)) * pad_size / drain);

    b.coarse = RTE_MIN(cut / ST_TX_VIDEO_RL_TRIM_COARSE, n);
    b.fine =
        RTE_MIN((cut - b.coarse * ST_TX_VIDEO_RL_TRIM_COARSE) / ST_TX_VIDEO_RL_TRIM_FINE,
                n - b.coarse);
  }
  return b;
}

/* the shaper time a burst adds: a cut pad drains by its bytes */
static inline long double st_tx_video_rl_burst_ns(const struct st_tx_video_rl_burst* b,
                                                  long double drain, uint16_t pad_size) {
  unsigned int n = b->n;
  uint32_t cut =
      b->coarse * ST_TX_VIDEO_RL_TRIM_COARSE + b->fine * ST_TX_VIDEO_RL_TRIM_FINE;

  return n * drain - cut * drain / pad_size;
}

#endif
