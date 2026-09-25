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
