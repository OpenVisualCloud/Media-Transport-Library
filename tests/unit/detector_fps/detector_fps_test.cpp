// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>

#include "../../../include/st_api.h"

extern "C" {
void test_calc_fps(int rtp_tm_0, int rtp_tm_1, int rtp_tm_2, int* out_fps);
}

namespace {

// d0 / d1 are the consecutive RTP-timestamp deltas the detector
// computes from rtp_tm[1]-rtp_tm[0] and rtp_tm[2]-rtp_tm[1].
//
// The production helper accepts |d0 - d1| <= 1 jitter and then maps a
// table of canonical d0 values to the corresponding ST_FPS_*. The
// non-trivial entries are the dual-d0 ones (1501/1502 → P59_94,
// 3753/3754 → P23_98) — those exist precisely to absorb 1-tick PTP
// jitter at frame boundaries that would otherwise round to the wrong
// canonical d0.

int run(int d0, int d1) {
  int fps = -1;
  test_calc_fps(0, d0, d0 + d1, &fps);
  return fps;
}

TEST(DetectorFps, ZeroJitterCanonicalDeltasMap) {
  EXPECT_EQ((int)ST_FPS_P120, run(750, 750));
  EXPECT_EQ((int)ST_FPS_P100, run(900, 900));
  EXPECT_EQ((int)ST_FPS_P60, run(1500, 1500));
  EXPECT_EQ((int)ST_FPS_P50, run(1800, 1800));
  EXPECT_EQ((int)ST_FPS_P30, run(3000, 3000));
  EXPECT_EQ((int)ST_FPS_P29_97, run(3003, 3003));
  EXPECT_EQ((int)ST_FPS_P25, run(3600, 3600));
  EXPECT_EQ((int)ST_FPS_P24, run(3750, 3750));
}

TEST(DetectorFps, OneTickJitterIsAbsorbed) {
  // |d0 - d1| == 1 is the documented tolerance. Canonical d0 must
  // still classify even when d1 differs by one tick.
  EXPECT_EQ((int)ST_FPS_P60, run(1500, 1501));
  EXPECT_EQ((int)ST_FPS_P60, run(1500, 1499));
  EXPECT_EQ((int)ST_FPS_P30, run(3000, 3001));
}

TEST(DetectorFps, NineFiveNineFourDualDeltas) {
  // 59.94Hz lands on either d0=1501 or d0=1502 depending on phase.
  EXPECT_EQ((int)ST_FPS_P59_94, run(1501, 1501));
  EXPECT_EQ((int)ST_FPS_P59_94, run(1502, 1502));
  EXPECT_EQ((int)ST_FPS_P59_94, run(1501, 1502));
  EXPECT_EQ((int)ST_FPS_P59_94, run(1502, 1501));
}

TEST(DetectorFps, TwentyThreeNineEightDualDeltas) {
  EXPECT_EQ((int)ST_FPS_P23_98, run(3753, 3753));
  EXPECT_EQ((int)ST_FPS_P23_98, run(3754, 3754));
  EXPECT_EQ((int)ST_FPS_P23_98, run(3753, 3754));
}

TEST(DetectorFps, TwoTickJitterIsRejected) {
  // |d0 - d1| > 1 must NOT classify; production leaves meta->fps
  // untouched (test_calc_fps preloads ST_FPS_MAX as the sentinel).
  EXPECT_EQ((int)ST_FPS_MAX, run(1500, 1502));
  EXPECT_EQ((int)ST_FPS_MAX, run(3000, 3003));
}

TEST(DetectorFps, UnknownDeltaIsRejected) {
  // d0 not in the canonical table → fps stays at sentinel.
  EXPECT_EQ((int)ST_FPS_MAX, run(1234, 1234));
  EXPECT_EQ((int)ST_FPS_MAX, run(0, 0));
}

}  // namespace
