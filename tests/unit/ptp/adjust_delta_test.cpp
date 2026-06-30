/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the branch selection of `ptp_adjust_delta` introduced by the
 * MT_IF_FEATURE_TIMESYNC_ADJUST_FREQ runtime gate, plus the delta
 * bookkeeping and `locked` detection. With no_timesync=true every PHC
 * adjustment routes into the no_timesync_delta accumulator.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='PtpAdjustDelta*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"
#include "ptp/timesync_mock.h"

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

/* Feature CLEAR: legacy step path. adjust_time runs unconditionally, so
 * with no_timesync the delta lands in no_timesync_delta; bookkeeping runs;
 * the servo is never touched. */
TEST_F(PtpAdjustDeltaTest, FeatureClearTakesLegacyStepPath) {
  ut_ptp_set_feature_adjust_freq(ctx_, false);
  const int64_t delta = 42;

  ut_ptp_adjust_delta(ctx_, delta, false);

  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), delta);
  EXPECT_EQ(ut_ptp_ptp_delta(ctx_), delta);
  EXPECT_EQ(ut_ptp_stat_delta_cnt(ctx_), 1);
  EXPECT_EQ(ut_ptp_delta_result_cnt(ctx_), 1u);
  EXPECT_EQ(ut_ptp_stat_delta_max(ctx_), delta);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 0); /* servo untouched */
}

/* Feature SET + error_correct=true: the servo branch is skipped entirely
 * (count stays 0, no adjustment), but the bookkeeping tail still runs. */
TEST_F(PtpAdjustDeltaTest, FeatureSetErrorCorrectSkipsServo) {
  ut_ptp_set_feature_adjust_freq(ctx_, true);
  ut_ptp_set_t2(ctx_, 1000);
  const int64_t delta = 77;

  ut_ptp_adjust_delta(ctx_, delta, true);

  EXPECT_EQ(ut_ptp_servo_count(ctx_), 0);       /* pi_sample not consumed */
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 0); /* no adjustment performed */
  EXPECT_EQ(ut_ptp_ptp_delta(ctx_), delta);     /* bookkeeping still runs */
  EXPECT_EQ(ut_ptp_stat_delta_cnt(ctx_), 1);
}

/* Feature SET + error_correct=false: walk the servo
 * UNLOCKED->UNLOCKED->UNLOCKED->JUMP->LOCKED.
 * JUMP performs adjust_time; LOCKED performs adjust_freq -- both route
 * into no_timesync_delta under no_timesync, so it changes by `delta` on
 * the JUMP iteration and again on the LOCKED iteration. */
TEST_F(PtpAdjustDeltaTest, FeatureSetWalksServoToLocked) {
  ut_ptp_set_feature_adjust_freq(ctx_, true);
  ut_ptp_set_t2(ctx_, 5000);
  const int64_t delta = 10;

  /* count 0 -> UNLOCKED */
  ut_ptp_adjust_delta(ctx_, delta, false);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 1);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 0);

  /* count 1 -> UNLOCKED */
  ut_ptp_adjust_delta(ctx_, delta, false);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 2);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 0);

  /* count 2 -> UNLOCKED (drift set) */
  ut_ptp_adjust_delta(ctx_, delta, false);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 3);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 0);

  /* count 3 -> JUMP: adjust_time accumulates delta */
  ut_ptp_adjust_delta(ctx_, delta, false);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 4);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), delta);

  /* count 4 -> LOCKED: adjust_freq also accumulates delta under no_timesync */
  ut_ptp_adjust_delta(ctx_, delta, false);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 4);
  EXPECT_EQ(ut_ptp_no_timesync_delta(ctx_), 2 * delta);

  /* ptp_delta accumulates every call regardless of branch */
  EXPECT_EQ(ut_ptp_ptp_delta(ctx_), 5 * delta);
}

/* Frequency is the preferred discipline: with the feature available, once the
 * servo reaches LOCKED every adjustment must drive rte_eth_timesync_adjust_freq
 * -- a real frequency (incval) correction -- not a time step. Only the single
 * warm-up JUMP sample is allowed to step time. Driven with no_timesync=false so
 * the production wrappers reach the gmock seam. */
TEST_F(PtpAdjustDeltaTest, LockedPathAdjustsFrequencyNotTime) {
  using ::testing::_;
  using ::testing::AnyNumber;
  using ::testing::AtLeast;
  using ::testing::Ne;
  using ::testing::NiceMock;
  using ::testing::Return;

  ut_ptp_set_no_timesync(ctx_, false);
  ut_ptp_set_feature_adjust_freq(ctx_, true);
  const int64_t delta = 10;

  NiceMock<TimesyncMock> ts;
  /* Exactly one warm-up time step (the JUMP), carrying `delta`. */
  EXPECT_CALL(ts, adjust_time(_, delta)).Times(1).WillOnce(Return(0));
  /* Frequency disciplined on every LOCKED sample, at least one with a
   * non-zero scaled-ppm correction. Later expectations match first, so
   * zero-ppm calls (if any) fall through to the AnyNumber catch-all. */
  EXPECT_CALL(ts, adjust_freq(_, _)).Times(AnyNumber()).WillRepeatedly(Return(0));
  EXPECT_CALL(ts, adjust_freq(_, Ne(0))).Times(AtLeast(1)).WillRepeatedly(Return(0));

  uint64_t t2 = 1000;
  for (int i = 0; i < 7; i++) {
    ut_ptp_set_t2(ctx_, t2); /* distinct local ts keeps the drift seed finite */
    t2 += 1000;
    ut_ptp_adjust_delta(ctx_, delta, false);
  }
  /* gmock verifies adjust_time==1 and adjust_freq>=1 (non-zero) at scope exit. */
}

/* Feature CLEAR: frequency is never adjusted -- every sample steps time via
 * rte_eth_timesync_adjust_time, regardless of servo state. */
TEST_F(PtpAdjustDeltaTest, FeatureClearNeverAdjustsFrequency) {
  using ::testing::_;
  using ::testing::Return;

  ut_ptp_set_no_timesync(ctx_, false);
  ut_ptp_set_feature_adjust_freq(ctx_, false);

  TimesyncMock ts;
  EXPECT_CALL(ts, adjust_freq(_, _)).Times(0);
  EXPECT_CALL(ts, adjust_time(_, _)).Times(10).WillRepeatedly(Return(0));

  for (int i = 0; i < 10; i++) ut_ptp_adjust_delta(ctx_, 10, false);
}

/* Signed accumulation: ptp_delta is the signed sum; stat_delta_min/max
 * track signed extremes; stat_delta_cnt counts every call. */
TEST_F(PtpAdjustDeltaTest, SignedDeltaBookkeeping) {
  ut_ptp_set_feature_adjust_freq(ctx_, false);
  const int64_t deltas[] = {50, -30, 0, 1000000};
  for (int64_t d : deltas) ut_ptp_adjust_delta(ctx_, d, false);

  EXPECT_EQ(ut_ptp_ptp_delta(ctx_), 50 - 30 + 0 + 1000000);
  EXPECT_EQ(ut_ptp_stat_delta_max(ctx_), 1000000);
  EXPECT_EQ(ut_ptp_stat_delta_min(ctx_), -30);
  EXPECT_EQ(ut_ptp_stat_delta_cnt(ctx_), 4);
}

/* `locked` requires BOTH stat_delta_min and stat_delta_max within (0,100)
 * in absolute value -- i.e. a small negative AND a small positive delta --
 * sustained past the stat_sync_keep > 100 threshold. */
TEST_F(PtpAdjustDeltaTest, LockedAfterSustainedSmallDeltas) {
  ut_ptp_set_feature_adjust_freq(ctx_, false);
  ut_ptp_adjust_delta(ctx_, 50, false);  /* max=50, min=0 */
  ut_ptp_adjust_delta(ctx_, -50, false); /* max=50, min=-50 */
  EXPECT_FALSE(ut_ptp_locked(ctx_));

  for (int i = 0; i < 210; i++) ut_ptp_adjust_delta(ctx_, 50, false);
  EXPECT_TRUE(ut_ptp_locked(ctx_));
}

/* A delta >= 100 pushes stat_delta_max out of range, resetting the keep
 * counter and preventing lock. */
TEST_F(PtpAdjustDeltaTest, LargeDeltaResetsSyncKeep) {
  ut_ptp_set_feature_adjust_freq(ctx_, false);
  ut_ptp_adjust_delta(ctx_, 50, false);
  ut_ptp_adjust_delta(ctx_, -50, false);
  for (int i = 0; i < 5; i++) ut_ptp_adjust_delta(ctx_, 50, false);
  EXPECT_GT(ut_ptp_stat_sync_keep(ctx_), 0);

  ut_ptp_adjust_delta(ctx_, 200, false); /* max becomes 200 -> out of range */
  EXPECT_EQ(ut_ptp_stat_sync_keep(ctx_), 0);
  EXPECT_FALSE(ut_ptp_locked(ctx_));
}

/* Requirements doc section 6.3 "Counter Decimation": once the port is LOCKED,
 * an out-of-range delta (|d| >= 100ns) must reset stat_sync_keep to 0 AND drop
 * the port back out of the locked state. Unlike LargeDeltaResetsSyncKeep this
 * first drives the port to locked=true, so it pins the demotion, not just the
 * counter reset. */
TEST_F(PtpAdjustDeltaTest, LockedClearsOnLargeDelta) {
  ut_ptp_set_feature_adjust_freq(ctx_, false);
  ut_ptp_adjust_delta(ctx_, 50, false);  /* max=50, min=0  */
  ut_ptp_adjust_delta(ctx_, -50, false); /* max=50, min=-50 */
  for (int i = 0; i < 210; i++) ut_ptp_adjust_delta(ctx_, 50, false);
  ASSERT_TRUE(ut_ptp_locked(ctx_));

  ut_ptp_adjust_delta(ctx_, 200, false); /* |200| >= 100 -> decimate */
  EXPECT_EQ(ut_ptp_stat_sync_keep(ctx_), 0);
  EXPECT_FALSE(ut_ptp_locked(ctx_));
}
