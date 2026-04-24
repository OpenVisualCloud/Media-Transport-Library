// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>
#include <stdint.h>

extern "C" {
void test_calc_dim(int max_line_num, int interlaced, uint32_t* out_w, uint32_t* out_h);
}

namespace {

struct Dim {
  uint32_t w, h;
};
Dim run(int max_line_num, bool interlaced) {
  Dim d{0, 0};
  test_calc_dim(max_line_num, interlaced ? 1 : 0, &d.w, &d.h);
  return d;
}

// Progressive table: max_line_num is (height - 1).
TEST(DetectorDimension, ProgressiveStandardResolutions) {
  EXPECT_EQ(480u, run(479, false).h);
  EXPECT_EQ(640u, run(479, false).w);
  EXPECT_EQ(720u, run(719, false).h);
  EXPECT_EQ(1280u, run(719, false).w);
  EXPECT_EQ(1080u, run(1079, false).h);
  EXPECT_EQ(1920u, run(1079, false).w);
  EXPECT_EQ(2160u, run(2159, false).h);
  EXPECT_EQ(3840u, run(2159, false).w);
  EXPECT_EQ(4320u, run(4319, false).h);
  EXPECT_EQ(7680u, run(4319, false).w);
}

// Interlaced table: each field has half the lines, so max_line_num
// is roughly (height/2 - 1).
TEST(DetectorDimension, InterlacedStandardResolutions) {
  EXPECT_EQ(480u, run(239, true).h);
  EXPECT_EQ(640u, run(239, true).w);
  EXPECT_EQ(720u, run(359, true).h);
  EXPECT_EQ(1280u, run(359, true).w);
  EXPECT_EQ(1080u, run(539, true).h);
  EXPECT_EQ(1920u, run(539, true).w);
  EXPECT_EQ(2160u, run(1079, true).h);
  EXPECT_EQ(3840u, run(1079, true).w);
  EXPECT_EQ(4320u, run(2159, true).h);
  EXPECT_EQ(7680u, run(2159, true).w);
}

TEST(DetectorDimension, UnknownLineNumLeavesDimensionsUnset) {
  // Detector helper falls through to err() on unknown line counts and
  // leaves meta->width / meta->height at their pre-existing values.
  // The wrapper zeroes them, so they must remain zero.
  Dim d = run(123, false);
  EXPECT_EQ(0u, d.w);
  EXPECT_EQ(0u, d.h);

  d = run(0, true);
  EXPECT_EQ(0u, d.w);
  EXPECT_EQ(0u, d.h);
}

TEST(DetectorDimension, InterlacedAndProgressiveTablesAreDistinct) {
  // max_line_num=1079 means 2160p in interlaced mode but 1080p
  // progressive — the interlaced/progressive flag must steer the
  // lookup, not just be ignored.
  EXPECT_NE(run(1079, true).h, run(1079, false).h);
  EXPECT_NE(run(2159, true).h, run(2159, false).h);
}

}  // namespace
