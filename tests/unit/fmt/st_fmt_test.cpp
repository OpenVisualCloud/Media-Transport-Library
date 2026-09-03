/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pure-function pin for st_frame_period_ns(), the frame period the pipeline TX
 * sessions cache at create and their DROP_WHEN_LATE window measures against.
 * Expected values are 1e9 * den / mul truncated, so a swapped mul/den moves
 * them; they are bit-identical to the double form the callers used before.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='StFramePeriodNs*'
 */

#include <errno.h>
#include <gtest/gtest.h>

extern "C" {
#include "st2110/st_fmt.h"
}

TEST(StFramePeriodNs, EveryFpsInTheTimingTable) {
  const struct {
    enum st_fps fps;
    uint64_t period_ns;
  } cases[] = {
      {ST_FPS_P120, 8333333}, {ST_FPS_P119_88, 8341666}, {ST_FPS_P100, 10000000},
      {ST_FPS_P60, 16666666}, {ST_FPS_P59_94, 16683333}, {ST_FPS_P50, 20000000},
      {ST_FPS_P30, 33333333}, {ST_FPS_P29_97, 33366666}, {ST_FPS_P25, 40000000},
      {ST_FPS_P24, 41666666}, {ST_FPS_P23_98, 41708333},
  };

  for (auto& c : cases) {
    uint64_t period_ns = 0;
    ASSERT_EQ(st_frame_period_ns(c.fps, &period_ns), 0) << "fps " << c.fps;
    EXPECT_EQ(period_ns, c.period_ns) << "fps " << c.fps;
  }
}

TEST(StFramePeriodNs, InvalidFpsLeavesPeriodUntouched) {
  uint64_t period_ns = 0xdeadbeef;

  EXPECT_EQ(st_frame_period_ns(ST_FPS_MAX, &period_ns), -EINVAL);
  EXPECT_EQ(period_ns, 0xdeadbeefu);
}
