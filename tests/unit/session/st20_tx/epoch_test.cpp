/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the epoch/onward-resync math in
 * `calc_frame_count_since_epoch()` (st_tx_video_session.c): steady-state
 * advance, the max_onward_epochs boundary, the PTP-step resync fix, the
 * late-frame drop path, and required_tai validation.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxEpochTest.*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentEpoch = kCurrentTai / kFramePeriodNs;
constexpr uint64_t kInitialEpoch = kCurrentEpoch - 1;
constexpr uint64_t kStaleEpoch = 1000;
constexpr uint64_t kPastTai = 5 * kFramePeriodNs;
constexpr uint64_t kOnwardRecoveryTai = 6 * kFramePeriodNs;
constexpr uint64_t kSecondOnwardRecoveryTai = 8 * kFramePeriodNs;
constexpr uint64_t kLateTai = 13 * kFramePeriodNs;
constexpr uint64_t kHalfFrameTargetTai = kCurrentTai + kFramePeriodNs / 2;
constexpr uint64_t kFutureBoundaryTai = kCurrentTai + kNanosecondsPerSecond;
constexpr uint64_t kBeyondFutureBoundaryTai = kFutureBoundaryTai + kFramePeriodNs;
constexpr uint64_t kLateTargetTai = 100 * kFramePeriodNs;
}  // namespace

class St20TxEpochTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFramePeriodNs);
    ut_txv_set_max_onward_epochs(ctx_, 3);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  void ExpectNoStatsOrLateCallback() {
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
    EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxEpochTest, SteadyStateAdvancesByOneEpoch) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* next_free_frame_slot = 10 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, 0);
  EXPECT_EQ(frame_count, kCurrentEpoch);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, OnwardGapAtBoundaryDoesNotResync) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* next=10 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(
      ctx_, 7 * kFramePeriodNs, 0); /* tai=7, onward=3==max */
  EXPECT_EQ(frame_count, kCurrentEpoch);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, CurrentTimeAtNextSlotBoundaryDoesNotReportLate) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, 0);

  EXPECT_EQ(frame_count, kCurrentEpoch);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, OnwardGapBeyondBoundaryResyncs) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* next=10 */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, kOnwardRecoveryTai,
                                                             0); /* tai=6, onward=4>max */
  EXPECT_EQ(frame_count, kOnwardRecoveryTai / kFramePeriodNs);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 4u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxEpochTest, RepeatedOnwardRecoveryAccumulatesExactGaps) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txv_calc_frame_count_since_epoch(ctx_, kOnwardRecoveryTai, 0),
            kOnwardRecoveryTai / kFramePeriodNs);
  ut_txv_set_cur_epochs(ctx_, 12);
  EXPECT_EQ(ut_txv_calc_frame_count_since_epoch(ctx_, kSecondOnwardRecoveryTai, 0),
            kSecondOnwardRecoveryTai / kFramePeriodNs);

  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 9u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxEpochTest, LargePtpStepResyncsInOneCall) {
  constexpr uint64_t stale_epoch = kStaleEpoch * kStaleEpoch;
  constexpr uint64_t recovered_epoch = kPastTai / kFramePeriodNs;
  constexpr uint64_t onward_gap = stale_epoch + 1 - recovered_epoch;
  ut_txv_set_cur_epochs(ctx_, stale_epoch);
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kPastTai, 0); /* tai=5 */
  EXPECT_EQ(frame_count, kPastTai / kFramePeriodNs);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), onward_gap);

  /* caller commits the resync (as tv_sync_pacing does) and time advances by
   * one frame -- confirm the gap stays closed, no further resync needed */
  ut_txv_set_cur_epochs(ctx_, frame_count);
  uint64_t frame_count2 =
      ut_txv_calc_frame_count_since_epoch(ctx_, kOnwardRecoveryTai, 0);
  EXPECT_EQ(frame_count2, kOnwardRecoveryTai / kFramePeriodNs);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), onward_gap);
}

TEST_F(St20TxEpochTest, LatePathBumpsDropAndNotifiesFrameLate) {
  ut_txv_set_cur_epochs(ctx_, 2); /* next_free_frame_slot=3 */
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, 0); /* tai=10 */
  EXPECT_EQ(frame_count, kCurrentEpoch);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 7u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_late_last_delta(ctx_), 7u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, RepeatedLateFramesAccumulateExactDropsAndCallbacks) {
  ut_txv_set_cur_epochs(ctx_, 2);
  EXPECT_EQ(ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, 0), kCurrentEpoch);
  ut_txv_set_cur_epochs(ctx_, kCurrentEpoch);
  EXPECT_EQ(ut_txv_calc_frame_count_since_epoch(ctx_, kLateTai, 0),
            kLateTai / kFramePeriodNs);

  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 9u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 2);
  EXPECT_EQ(ut_txv_notify_late_last_delta(ctx_), 2u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, ValidRequiredTaiDoesNotChangeAnyStatistic) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, kHalfFrameTargetTai);

  EXPECT_EQ(frame_count, (kHalfFrameTargetTai + kFramePeriodNs / 2) / kFramePeriodNs);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, RequiredTaiInThePastFlagsStat) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* next=10, onward=0 (normal path) */
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, kPastTai);
  EXPECT_EQ(frame_count,
            kPastTai / kFramePeriodNs); /* required_tai-derived value, not overridden */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxEpochTest, RequiredTaiTooFarFutureFlagsStat) {
  ut_txv_set_cur_epochs(ctx_,
                        kInitialEpoch); /* current frame count = 10, limit = +1000 */
  uint64_t required_tai = kBeyondFutureBoundaryTai;
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, required_tai);
  EXPECT_EQ(frame_count, required_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxEpochTest, AppTargetDoesNotReportStaleOnwardState) {
  constexpr uint64_t target_tai = 12 * kFramePeriodNs;
  ut_txv_set_cur_epochs(ctx_, kStaleEpoch);
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, target_tai);
  EXPECT_EQ(frame_count, target_tai / kFramePeriodNs);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, RequiredTaiExactlyAtInThePastBoundaryDoesNotFlag) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* current frame count = 10 */
  ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai,
                                      kCurrentTai); /* required frame count == 10 */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, RequiredTaiExactlyAtFutureBoundaryDoesNotFlag) {
  ut_txv_set_cur_epochs(ctx_,
                        kInitialEpoch); /* current frame count = 10, limit = +1000 */
  ut_txv_calc_frame_count_since_epoch(
      ctx_, kCurrentTai, kFutureBoundaryTai); /* required frame count == 1010 */
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxEpochTest, RepeatedInvalidTimestampsAccumulateWithoutEpochRecovery) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, kPastTai),
            kPastTai / kFramePeriodNs);
  EXPECT_EQ(
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, kBeyondFutureBoundaryTai),
      kBeyondFutureBoundaryTai / kFramePeriodNs);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 2u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxEpochTest, CurEpochsWraparoundResyncsWithoutRecoveryStats) {
  ut_txv_set_cur_epochs(ctx_, UINT64_MAX);
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, kPastTai, 0);
  EXPECT_EQ(frame_count, kPastTai / kFramePeriodNs);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, CurTaiNearUint64MaxNoCrash) {
  ut_txv_set_cur_epochs(ctx_, 0); /* next_free_frame_slot=1 */
  uint64_t cur_tai = UINT64_MAX;
  uint64_t expected_frame_count_tai = cur_tai / kFramePeriodNs;
  uint64_t frame_count = ut_txv_calc_frame_count_since_epoch(ctx_, cur_tai, 0);
  EXPECT_EQ(frame_count, expected_frame_count_tai);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), expected_frame_count_tai - 1);
}

TEST_F(St20TxEpochTest, AppTargetDoesNotReportStaleLateState) {
  ut_txv_set_cur_epochs(ctx_, 2);
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, kLateTargetTai);
  EXPECT_EQ(frame_count, kLateTargetTai / kFramePeriodNs);
  ExpectNoStatsOrLateCallback();
}

TEST_F(St20TxEpochTest, RequiredTaiRoundingToZeroFallsBackToCurrentTime) {
  ut_txv_set_cur_epochs(ctx_, 9); /* next_free_frame_slot=10, onward=0 (normal path) */
  uint64_t required_tai = 10;
  uint64_t frame_count =
      ut_txv_calc_frame_count_since_epoch(ctx_, kCurrentTai, required_tai);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(frame_count, kCurrentEpoch);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}
