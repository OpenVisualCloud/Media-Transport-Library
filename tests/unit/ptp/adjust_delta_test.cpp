/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the delta bookkeeping and `locked` detection in the tail of
 * `ptp_adjust_delta`. With no_timesync=true every PHC adjustment routes into
 * the no_timesync_delta accumulator, so the bookkeeping runs with no NIC.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='PtpAdjustDelta*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PtpAdjustDeltaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }
  ut_ptp_ctx* ctx_ = nullptr;
};

/* ptp_delta is the signed running sum; stat_delta_min/max track signed
 * extremes; stat_delta_cnt and delta_result_cnt count every call. */
TEST_F(PtpAdjustDeltaTest, SignedDeltaBookkeeping) {
  const int64_t deltas[] = {50, -30, 0, 1000000};
  for (int64_t d : deltas) ut_ptp_adjust_delta(ctx_, d, false);

  EXPECT_EQ(ut_ptp_ptp_delta(ctx_), 50 - 30 + 0 + 1000000);
  EXPECT_EQ(ut_ptp_stat_delta_max(ctx_), 1000000);
  EXPECT_EQ(ut_ptp_stat_delta_min(ctx_), -30);
  EXPECT_EQ(ut_ptp_stat_delta_cnt(ctx_), 4);
  EXPECT_EQ(ut_ptp_delta_result_cnt(ctx_), 4u);
  /* under no_timesync the same signed sum lands in the accumulator */
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 50 - 30 + 0 + 1000000);
}

/* `locked` requires BOTH stat_delta_min and stat_delta_max within (0,100) in
 * absolute value -- a small negative AND a small positive delta -- sustained
 * past the stat_sync_keep > 100 threshold. */
TEST_F(PtpAdjustDeltaTest, LockedAfterSustainedSmallDeltas) {
  ut_ptp_adjust_delta(ctx_, 50, false);  /* max=50, min=0   */
  ut_ptp_adjust_delta(ctx_, -50, false); /* max=50, min=-50 */
  EXPECT_FALSE(ut_ptp_locked(ctx_));

  for (int i = 0; i < 210; i++) ut_ptp_adjust_delta(ctx_, 50, false);
  EXPECT_TRUE(ut_ptp_locked(ctx_));
}

/* A delta >= 100ns pushes stat_delta_max out of range, resetting the keep
 * counter and preventing lock. */
TEST_F(PtpAdjustDeltaTest, LargeDeltaResetsSyncKeep) {
  ut_ptp_adjust_delta(ctx_, 50, false);
  ut_ptp_adjust_delta(ctx_, -50, false);
  for (int i = 0; i < 5; i++) ut_ptp_adjust_delta(ctx_, 50, false);
  EXPECT_GT(ut_ptp_stat_sync_keep(ctx_), 0);

  ut_ptp_adjust_delta(ctx_, 200, false); /* max becomes 200 -> out of range */
  EXPECT_EQ(ut_ptp_stat_sync_keep(ctx_), 0);
  EXPECT_FALSE(ut_ptp_locked(ctx_));
}
