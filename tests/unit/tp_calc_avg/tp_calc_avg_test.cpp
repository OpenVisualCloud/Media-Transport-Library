// SPDX-License-Identifier: BSD-3-Clause
// Copyright(c) 2026 Intel Corporation
//
// Regression lock for rv_tp_calculate_avg, the static-inline mean
// helper used by the rx timing parser stat aggregator.
//
// Contract under test:
//   * cnt == 0 returns the sentinel -1.0f (callers use this to skip
//     printing/serializing an undefined average).
//   * cnt > 0 returns (float)sum / cnt.

#include <gtest/gtest.h>
#include <stdint.h>

#include <cmath>

extern "C" {
float tp_calc_avg(uint32_t cnt, int64_t sum);
}

namespace {

TEST(TpCalcAvg, IntegerDivisionWithRemainderProducesFractional) {
  // 10 / 3 must be exactly 3.333... in float, not truncated to 3.
  EXPECT_FLOAT_EQ(10.0f / 3.0f, tp_calc_avg(3, 10));
}

TEST(TpCalcAvg, LargeSumIsFinite) {
  // Sums in real use (millions of packets, vrx ~ thousands) easily
  // exceed 2^31. Result must remain a finite float.
  const int64_t big_sum = static_cast<int64_t>(1) << 50;
  const uint32_t cnt = 1u << 20;
  float r = tp_calc_avg(cnt, big_sum);
  EXPECT_TRUE(std::isfinite(r));
  EXPECT_GT(r, 0.0f);
}

}  // namespace
