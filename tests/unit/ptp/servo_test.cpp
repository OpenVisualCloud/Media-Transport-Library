/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the PI servo state machine (`pi_sample`) and the LOCKED-path
 * ppb -> frequency conversion `-1 * (long)(ppb * 65.536)`.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='PiServo*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PiServoTest : public ::testing::Test {
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

/* count 0/1 -> UNLOCKED, ppb == 0, count advances. */
TEST_F(PiServoTest, FirstTwoSamplesUnlocked) {
  int state = -1;
  EXPECT_DOUBLE_EQ(ut_pi_sample(ctx_, 10.0, 0.0, &state), 0.0);
  EXPECT_EQ(state, UT_PTP_SERVO_UNLOCKED);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 1);

  EXPECT_DOUBLE_EQ(ut_pi_sample(ctx_, 20.0, 5.0, &state), 0.0);
  EXPECT_EQ(state, UT_PTP_SERVO_UNLOCKED);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 2);
}

/* count 2 computes drift = (offset1 - offset0) / (local1 - local0).
 * Negative slope. */
TEST_F(PiServoTest, DriftNegativeSlope) {
  int state = -1;
  ut_pi_sample(ctx_, 100.0, 0.0, &state); /* offset[0]=100 local[0]=0  */
  ut_pi_sample(ctx_, 50.0, 10.0, &state); /* offset[1]=50  local[1]=10 */
  EXPECT_DOUBLE_EQ(ut_pi_sample(ctx_, 0.0, 0.0, &state), 0.0);
  EXPECT_EQ(state, UT_PTP_SERVO_UNLOCKED);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 3);
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), (50.0 - 100.0) / (10.0 - 0.0));
}

/* count 2 with a fractional slope. */
TEST_F(PiServoTest, DriftFractionalSlope) {
  int state = -1;
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  ut_pi_sample(ctx_, 1.0, 3.0, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), 1.0 / 3.0);
}

/* count 3 -> JUMP, ppb == 0, count becomes 4. */
TEST_F(PiServoTest, ThirdSampleJump) {
  int state = -1;
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  ut_pi_sample(ctx_, 0.0, 1.0, &state);
  ut_pi_sample(ctx_, 0.0, 2.0, &state);
  EXPECT_DOUBLE_EQ(ut_pi_sample(ctx_, 0.0, 3.0, &state), 0.0);
  EXPECT_EQ(state, UT_PTP_SERVO_JUMP);
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 4);
}

/* count 4 -> LOCKED, ppb == 0.3*offset + (drift_before + 0.7*offset). */
TEST_F(PiServoTest, FourthSampleLocked) {
  int state = -1;
  const double o0 = 40.0, l0 = 0.0;
  const double o1 = 60.0, l1 = 8.0;
  ut_pi_sample(ctx_, o0, l0, &state);
  ut_pi_sample(ctx_, o1, l1, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state); /* sets drift */
  const double drift_before = (o1 - o0) / (l1 - l0);
  ASSERT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), drift_before);

  ut_pi_sample(ctx_, 0.0, 0.0, &state); /* JUMP, count -> 4 */
  ASSERT_EQ(ut_ptp_servo_count(ctx_), 4);

  const double offset = 12.0;
  const double ppb = ut_pi_sample(ctx_, offset, 0.0, &state);
  EXPECT_EQ(state, UT_PTP_SERVO_LOCKED);
  const double drift_after = drift_before + 0.7 * offset;
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), drift_after);
  EXPECT_DOUBLE_EQ(ppb, 0.3 * offset + drift_after);
  /* count stays 4 on the LOCKED branch */
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 4);
}

/* count >= 4 stays LOCKED and accumulates the integral term every sample:
 * drift(k) = drift(k-1) + 0.7*offset, ppb = 0.3*offset + drift(k).
 * Pins the continuous PI tracking of servo State 4 across multiple samples
 * (requirements doc section 6, "State 4 Continuous Lock PI Tracking"). */
TEST_F(PiServoTest, LockedIntegralAccumulatesAcrossSamples) {
  int state = -1;
  /* reach count 4 with a flat first window so drift_before == 0 */
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  ut_pi_sample(ctx_, 0.0, 1.0, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state); /* count 2 -> drift = 0/1 = 0 */
  ut_pi_sample(ctx_, 0.0, 0.0, &state); /* count 3 -> JUMP, count -> 4 */
  ASSERT_EQ(ut_ptp_servo_count(ctx_), 4);
  ASSERT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), 0.0);

  /* first LOCKED sample, offset 10 */
  double ppb = ut_pi_sample(ctx_, 10.0, 0.0, &state);
  EXPECT_EQ(state, UT_PTP_SERVO_LOCKED);
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), 7.0); /* 0 + 0.7*10 */
  EXPECT_DOUBLE_EQ(ppb, 10.0);                     /* 0.3*10 + 7  */

  /* second LOCKED sample, offset 20: the integrator keeps accumulating */
  ppb = ut_pi_sample(ctx_, 20.0, 0.0, &state);
  EXPECT_EQ(state, UT_PTP_SERVO_LOCKED);
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), 21.0); /* 7 + 0.7*20 */
  EXPECT_DOUBLE_EQ(ppb, 27.0);                      /* 0.3*20 + 21 */
  EXPECT_EQ(ut_ptp_servo_count(ctx_), 4);
}

/* At LOCKED a zero offset leaves the integral (drift) untouched and the
 * frequency output equals the held drift -- the steady-state behaviour that
 * lets the loop hold a frequency once converged (the Ki integrator memory). */
TEST_F(PiServoTest, LockedZeroOffsetHoldsFrequency) {
  int state = -1;
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  ut_pi_sample(ctx_, 0.0, 1.0, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state);  /* count -> 4 */
  ut_pi_sample(ctx_, 30.0, 0.0, &state); /* first LOCKED: drift -> 21 */
  const double held = ut_ptp_servo_drift(ctx_);
  ASSERT_DOUBLE_EQ(held, 21.0);

  const double ppb = ut_pi_sample(ctx_, 0.0, 0.0, &state);
  EXPECT_EQ(state, UT_PTP_SERVO_LOCKED);
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), held); /* unchanged: + 0.7*0 */
  EXPECT_DOUBLE_EQ(ppb, held);                      /* 0.3*0 + drift      */
}

/* Requirements doc section 6, State 2 "Authoritative Specification Mismatch":
 * the first-order drift seed is the raw dimensionless ratio
 * (offset1-offset0)/(local1-local0) and is NOT scaled by 1e9 into ppb.
 * With ns-scale timestamps this makes the seed ~1e9x too small (effectively
 * defunct), forcing the LOCKED integral to acquire the lock. Pinned here so a
 * future 1e9 correction is a deliberate, visible change. */
TEST_F(PiServoTest, DriftSeedIsRawRatioNotPpbScaled) {
  int state = -1;
  const double o0 = 1000.0, l0 = 0.0;         /* ns */
  const double o1 = 1100.0, l1 = 125000000.0; /* +100ns over an 8ms window */
  ut_pi_sample(ctx_, o0, l0, &state);
  ut_pi_sample(ctx_, o1, l1, &state);
  ut_pi_sample(ctx_, 0.0, 0.0, &state); /* count 2 -> drift seeded */

  const double raw_ratio = (o1 - o0) / (l1 - l0); /* 8e-7 */
  EXPECT_DOUBLE_EQ(ut_ptp_servo_drift(ctx_), raw_ratio);
  /* the ppb-correct value would have been raw_ratio * 1e9 == 800 */
  EXPECT_LT(ut_ptp_servo_drift(ctx_), 1.0);
}

/* The ppb -> scaled-ppm frequency conversion used on the LOCKED path.
 * The (long) cast truncates toward zero. */
TEST(PiServoFreq, ConversionTruncatesTowardZero) {
  EXPECT_EQ(ut_ppb_to_freq(0.0), 0);
  EXPECT_EQ(ut_ppb_to_freq(1.0), -65);  /* 65.536  -> 65  */
  EXPECT_EQ(ut_ppb_to_freq(-1.0), 65);  /* -65.536 -> -65 */
  EXPECT_EQ(ut_ppb_to_freq(2.0), -131); /* 131.072 -> 131 */
  EXPECT_EQ(ut_ppb_to_freq(0.9), -58);  /* 58.9824 -> 58 (trunc, not 59) */
  EXPECT_EQ(ut_ppb_to_freq(-0.9), 58);  /* -58.9824 -> -58 (trunc toward 0) */
}
