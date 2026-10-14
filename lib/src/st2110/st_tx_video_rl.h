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
/* the last pre-arm pads are cut by up to TRIM_COARSE bytes each, in TRIM_FINE steps, as
 * the shaper drains a pad that short by its bytes */
#define ST_TX_VIDEO_RL_TRIM_COARSE (64)
#define ST_TX_VIDEO_RL_TRIM_FINE (8)

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
