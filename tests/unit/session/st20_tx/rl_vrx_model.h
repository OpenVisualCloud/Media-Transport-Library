/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef _UT_RL_VRX_MODEL_H_
#define _UT_RL_VRX_MODEL_H_

#include <algorithm>
#include <cstdint>

/* The receiver side of the RL tests, independent of the lib's plan. */

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

#endif
