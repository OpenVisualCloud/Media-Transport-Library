/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _UT_RL_VRX_MODEL_H_
#define _UT_RL_VRX_MODEL_H_

#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" {
#include "st2110/st_fmt.h"
}

/* The receiver side of the RL tests, independent of the lib's plan. */

constexpr uint32_t kUtRlRate90k = 90000;

/* A reclaiming port (DPDK 26.07 iavf/ice): rs_thresh descriptors a batch, the default,
 * and what freeing a done batch adds to a burst on an E810 VF at 720p59.94. A batch of
 * the frame before is cold at the warm-up open (p50); one freed soon after it completed,
 * by a waiting reclaim, or of pads, which share one mbuf, is hot. */
constexpr uint16_t kUtRlRsThresh = 32;
constexpr uint64_t kUtRlColdBatchFreeNs = 1350;
constexpr uint64_t kUtRlHotBatchFreeNs = 650;

/* the most the 90 kHz snap moves a target, epoch + tr_offset - vrx trs, later at mul/den
 * fps: 59.94 epochs take two phases on the tick grid, 50 one */
inline long double ut_rl_snap_loss(uint32_t mul, uint32_t den, long double tr_offset,
                                   uint32_t vrx, long double trs) {
  long double loss = 0;
  for (uint64_t k = 2; k < 4; k++) {
    const uint64_t start =
        (uint64_t)(st_epoch_to_tai(k, mul, den) + tr_offset - vrx * trs);
    loss = std::max(
        loss, (long double)st_tai_round_to_media_clk_ns(start, kUtRlRate90k) - start);
  }
  return loss;
}

inline long double ut_rl_snap_loss_5994(long double tr_offset, uint32_t vrx,
                                        long double trs) {
  return ut_rl_snap_loss(60000, 1001, tr_offset, vrx, trs);
}

/* ice_sched_bw_to_rl_profile(): the tokens of a timeslot of 32 << encode psm_clk cycles,
 * and the encode it takes, for a rate in kbps */
inline int64_t ut_ice_tokens(int64_t psm_hz, int64_t kbps, int encode) {
  int64_t bytes_per_sec = kbps * 1000 / 8;
  int64_t ts_rate = psm_hz / ((int64_t)1 << encode) / 32;
  if (ts_rate <= 0) return 0;
  return (bytes_per_sec * 10000 / ts_rate + 5000) / 10000;
}

inline int ut_ice_encode(int64_t psm_hz, int64_t kbps) {
  for (int i = 0; i < 64; i++)
    if (ut_ice_tokens(psm_hz, kbps, i) > 128) return i;
  return -1;
}

/* the psm_clks ice_get_psm_clk_freq() reads */
constexpr int64_t kUtIcePsmHz[] = {367647059, 390625000, 416666667, 446428571};

/* the tokens of the longest timeslot ice programs on any psm_clk for a queue of bps
 * bytes/s, which iavf or the PMD truncates to kbps */
inline int64_t ut_ice_timeslot_tokens(uint64_t bps) {
  const int64_t kbps = (int64_t)(bps / 1000 * 8);
  int64_t most = 0;
  for (int64_t psm : kUtIcePsmHz) {
    const int encode = ut_ice_encode(psm, kbps);
    if (encode >= 0) most = std::max(most, ut_ice_tokens(psm, kbps, encode));
  }
  return most;
}

/* VRX of a frame, the SMPTE ST 2110-21 virtual receive buffer: packet j drains at
 * tpr0 + j trs, and must be in by then. EBU LIST (pi-list vrx_calculator.cpp) counts
 * drained = (int)((t - TPR0 + Trs) / Trs), so a packet up to a trs past its slot still
 * reads VRX 0 there: this model is one trs stricter. The sender's packet 0 leaves at
 * launch, the next ones p apart, with a pad wherever the transmitter puts one, after
 * packet j when (j + 1 + pad_interval / 2) wraps pad_interval, and extra_pads more right
 * behind packet 0. Returns the fewest packets buffered at a drain slot of the first n. */
inline int ut_rl_vrx(long double launch, long double tpr0, long double trs, long double p,
                     long double pad_interval, int n, int extra_pads = 0) {
  int vrx = 1 << 30;
  int out = 0;
  for (int k = 0; k < n; k++) {
    const long double drain = tpr0 + k * trs;
    while (true) {
      const int pads =
          (int)floorl((out + pad_interval / 2) / pad_interval) + (out ? extra_pads : 0);
      if (launch + (out + pads) * p > drain) break;
      out++;
    }
    vrx = std::min(vrx, out - k);
  }
  return vrx;
}

#endif
