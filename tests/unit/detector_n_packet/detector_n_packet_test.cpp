// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation

#include <gtest/gtest.h>

extern "C" {
void test_calc_n_packet(int p0, int p1, int p2, int* out_pkt_per_frame);
}

namespace {

int run(int p0, int p1, int p2) {
  int out = -1;
  test_calc_n_packet(p0, p1, p2, &out);
  return out;
}

TEST(DetectorNPacket, EqualConsecutiveDeltasYieldPktPerFrame) {
  // (p1-p0) == (p2-p1) → pkt_per_frame = (p1 - p0).
  EXPECT_EQ(4320, run(0, 4320, 8640));
  EXPECT_EQ(8640, run(1000, 9640, 18280));
  EXPECT_EQ(1, run(0, 1, 2));
}

TEST(DetectorNPacket, MismatchedDeltasLeaveSentinel) {
  // total0 != total1 → production logs an error and does not write
  // pkt_per_frame. The wrapper preloads -1.
  EXPECT_EQ(-1, run(0, 4320, 8641));
  EXPECT_EQ(-1, run(0, 100, 250));
}

TEST(DetectorNPacket, ZeroDeltaIsAccepted) {
  // Edge case: a stuck counter (p0 == p1 == p2) currently yields 0.
  // Documenting the behavior; if the helper later rejects this it
  // should set pkt_per_frame back to its prior value (-1 here).
  EXPECT_EQ(0, run(42, 42, 42));
}

}  // namespace
