/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "main/tsc_calibrate_harness.h"

/* A clock read right after a sleep runs cold: 1 us more than the 50 ns warm read. */
TEST(MtTscCalibration, SlowFirstReadAfterSleepDoesNotBiasTscHz) {
  constexpr uint64_t kTscHz = 2400000000ull;
  constexpr uint64_t kReadNs = 50;
  constexpr uint64_t kColdReadNs = 1000;
  /* 1 ppm of tsc_hz is 0.8 us over an 800 ms pacing lead. */
  constexpr uint64_t kMaxErrorHz = kTscHz / 1000000;

  const uint64_t tsc_hz = ut_tsc_calibrate(kTscHz, kReadNs, kColdReadNs);

  EXPECT_NEAR(static_cast<double>(tsc_hz), static_cast<double>(kTscHz), kMaxErrorHz);
}

/* The fixed-point mt_get_tsc() gives the exact conversion of the TSC, within 1 ns. */
TEST(MtTscCalibration, TscToNsMatchesTheExactConversion) {
  for (uint64_t tsc_hz :
       {1000000000ull, 2400000000ull, 2593904896ull, 3500000000ull, 5000000000ull}) {
    uint64_t lo_ns = 0, hi_ns = 0;
    const uint64_t ns = ut_tsc_to_ns(tsc_hz, &lo_ns, &hi_ns);

    EXPECT_GE(ns + 1, lo_ns) << "tsc_hz " << tsc_hz;
    EXPECT_LE(ns, hi_ns + 1) << "tsc_hz " << tsc_hz;
  }
}

/* tsc_hz is 0 before EAL init: mt_get_tsc() then gives 0, not a divide by zero. */
TEST(MtTscCalibration, ZeroTscHzGivesZeroNs) {
  uint64_t lo_ns = 0, hi_ns = 0;

  EXPECT_EQ(ut_tsc_to_ns(0, &lo_ns, &hi_ns), 0u);
}
