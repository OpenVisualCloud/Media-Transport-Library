/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pure-function pin for st_frame_period_ns(), the frame period the pipeline TX
 * sessions cache at create and their DROP_WHEN_LATE window measures against.
 * Expected values are 1e9 * den / mul truncated, so a swapped mul/den moves
 * them; they are bit-identical to the double form the callers used before.
 *
 * Also pins st_frame_rate_to_st_fps()'s snap warning: its match windows are up to a full
 * fps wide, so an off-list rate is reclassified as a neighbour and paced at the wrong
 * period. The warning is the only signal the caller gets, so both directions of
 * ST_FPS_SNAP_TOLERANCE matter - silent for every rate a conformant caller can express,
 * loud for every rate that is really being substituted.
 *
 * Run:   ./build_unit/tests/unit/UnitTest
 *          --gtest_filter='StFramePeriodNs*:StFrameRateToStFps*'
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <stdarg.h>
#include <stdio.h>

extern "C" {
#include <mtl_api.h>

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

static int g_warn_cnt;
static char g_warn_msg[256];

static void ut_fps_log_printer(enum mtl_log_level level, const char* format, ...) {
  va_list args;

  if (level != MTL_LOG_LEVEL_WARNING) return;

  g_warn_cnt++;
  va_start(args, format);
  vsnprintf(g_warn_msg, sizeof(g_warn_msg), format, args);
  va_end(args);
}

class StFrameRateToStFps : public ::testing::Test {
 protected:
  void SetUp() override {
    g_warn_cnt = 0;
    g_warn_msg[0] = '\0';
    mtl_set_log_printer(ut_fps_log_printer);
  }
  void TearDown() override {
    mtl_set_log_printer(NULL);
  }
};

/* Every rate a conformant caller can hand over: the integer rates, the four fractional
 * rates as the exact rational a GStreamer/FFmpeg caller passes, and the same four as the
 * rounded decimal the table names them by. None of these may warn. */
TEST_F(StFrameRateToStFps, ConformantRatesAreSilent) {
  const struct {
    double framerate;
    enum st_fps fps;
  } cases[] = {
      {120.0, ST_FPS_P120},
      {120000.0 / 1001, ST_FPS_P119_88},
      {119.88, ST_FPS_P119_88},
      {100.0, ST_FPS_P100},
      {60.0, ST_FPS_P60},
      {60000.0 / 1001, ST_FPS_P59_94},
      {59.94, ST_FPS_P59_94},
      {50.0, ST_FPS_P50},
      {30.0, ST_FPS_P30},
      {30000.0 / 1001, ST_FPS_P29_97},
      {29.97, ST_FPS_P29_97},
      {25.0, ST_FPS_P25},
      {24.0, ST_FPS_P24},
      {24000.0 / 1001, ST_FPS_P23_98},
      {23.98, ST_FPS_P23_98},
      /* Roundings of mul/den that differ from the name the table carries, so unlike
       * 119.880 or 29.970 these are not the name in disguise and do reach the tolerance.
       * 23.9765 is the closest conformant input to it: 4.76e-4 of 1e-3. */
      {119.8801, ST_FPS_P119_88},
      {59.9401, ST_FPS_P59_94},
      {23.976, ST_FPS_P23_98},
      {23.9765, ST_FPS_P23_98},
  };

  for (auto& c : cases) {
    g_warn_cnt = 0;
    EXPECT_EQ(st_frame_rate_to_st_fps(c.framerate), c.fps) << c.framerate;
    EXPECT_EQ(g_warn_cnt, 0) << c.framerate << " warned: " << g_warn_msg;
  }
}

/* The snapped return values themselves are asserted by St_Fps* in the integration suite
 * and are deliberately unchanged; what is pinned here is that snapping is no longer
 * silent, which is all a GStreamer or FFmpeg caller ever sees. */
TEST_F(StFrameRateToStFps, OffListRatesWarnAndStillSnap) {
  const struct {
    double framerate;
    enum st_fps fps;
  } cases[] = {
      {26.0, ST_FPS_P25},
      {51.0, ST_FPS_P50},
      {31.0, ST_FPS_P30},
      {23.0, ST_FPS_P23_98},
      {49.0, ST_FPS_P50},
      {24.99, ST_FPS_P24},
      /* the near misses: a fraction of an fps off is still a wrong pacing period, and a
       * tolerance sized to the table's rounded names would let all of these through */
      {29.99, ST_FPS_P29_97},
      {29.98, ST_FPS_P29_97},
      {25.01, ST_FPS_P25},
      {119.94, ST_FPS_P119_88},
      {120.06, ST_FPS_P120},
      {59.95, ST_FPS_P59_94},
      /* the far side of the tolerance from 23.9765 above: 1.476e-3 of 1e-3, so the two
       * together bracket ST_FPS_SNAP_TOLERANCE to within ~3x */
      {23.9775, ST_FPS_P23_98},
  };

  for (auto& c : cases) {
    g_warn_cnt = 0;
    EXPECT_EQ(st_frame_rate_to_st_fps(c.framerate), c.fps) << c.framerate;
    EXPECT_EQ(g_warn_cnt, 1) << c.framerate << " did not warn";
  }
}

/* Outside every window the existing err() already fires, so no warning is added. */
TEST_F(StFrameRateToStFps, RateOutsideEveryWindowIsRejected) {
  EXPECT_EQ(st_frame_rate_to_st_fps(48.0), ST_FPS_MAX);
  EXPECT_EQ(g_warn_cnt, 0);
}
