/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the cursor-derivation math in `tv_sync_pacing()` (st_tx_video_session.c):
 * normal tsc/ptp cursor derivation, the ST20_TX_FLAG_EXACT_USER_PACING
 * bypass, and the negative time_to_tx_ns clamp.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxSyncPacingTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint64_t kCurrentEpoch = kCurrentTai / kFramePeriodNs;
constexpr uint64_t kInitialEpoch = kCurrentEpoch - 1;
constexpr uint64_t kStaleEpoch = 1000;
constexpr uint64_t kTargetTai = kCurrentTai + kFramePeriodNs / 2;
constexpr uint64_t kAlignedTargetTai = kCurrentTai + kFramePeriodNs;
constexpr uint64_t kPastTai = kCurrentTai - kFramePeriodNs / 2;
constexpr uint64_t kTrOffsetNs = kNanosecondsPerMillisecond / 1000;
constexpr uint64_t kPacketIntervalNs = kTrOffsetNs / 10;
constexpr uint32_t kVirtualReceiverBufferPackets = 2;
constexpr uint64_t kReceiverScheduleOffsetNs =
    kTrOffsetNs - kVirtualReceiverBufferPackets * kPacketIntervalNs;
constexpr uint64_t kInvalidMediaClockTimestamp = 10 * ST10_VIDEO_SAMPLING_RATE_90K;
}  // namespace

class St20TxSyncPacingTest : public ::testing::Test {
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
  void ExpectNoPacingStats() {
    EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
    EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
    EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxSyncPacingTest, NormalCursorDerivation) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* steady state -> frame_count becomes 10 */
  ut_txv_set_tr_offset(ctx_, kTrOffsetNs);
  ut_txv_set_vrx(ctx_, kVirtualReceiverBufferPackets);
  ut_txv_set_trs(ctx_, kPacketIntervalNs);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  EXPECT_EQ(ut_txv_cur_epochs(ctx_), kCurrentEpoch);
  /* kReceiverScheduleOffsetNs (800ns) is less than half a 90kHz tick (~5555ns),
   * so tv_sync_pacing()'s media-clock snap rounds it back down to kCurrentTai. */
  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_tsc_time_frame_start(ctx_), kCurrentTsc);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, ExactUserPacingBypassesTransmissionStartTime) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kTargetTai), 0);

  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kTargetTai);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc + kTargetTai - kCurrentTai);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, TimestampIsIgnoredWithoutUserPacing) {
  EXPECT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai), 0u);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, ZeroTimestampWithoutExactPacingFallsBackWithoutError) {
  ut_txv_set_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, 0), 0u);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, UserPacingAlignsTimestampToReceiverSchedule) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_tr_offset(ctx_, kTrOffsetNs);
  ut_txv_set_vrx(ctx_, kVirtualReceiverBufferPackets);
  ut_txv_set_trs(ctx_, kPacketIntervalNs);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai =
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai);
  ASSERT_EQ(required_tai, kTargetTai);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txv_cur_epochs(ctx_), kAlignedTargetTai / kFramePeriodNs);
  /* Same sub-tick snap as NormalCursorDerivation: kReceiverScheduleOffsetNs
   * rounds back down to kAlignedTargetTai. */
  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc + kAlignedTargetTai - kCurrentTai);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, FrameTaskletUserPacingAlignsFirstPacketTarget) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai,
                                     &packet_tsc, &packet_ptp),
            0);

  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_idx(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kAlignedTargetTai / kFramePeriodNs);
  EXPECT_TRUE(ut_txv_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txv_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + kAlignedTargetTai - kCurrentTai);
  EXPECT_EQ(packet_ptp, kAlignedTargetTai);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_exceed_frame_time(ctx_), 0u);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, FrameTaskletExactUserPacingUsesFirstPacketTargetVerbatim) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai,
                                     &packet_tsc, &packet_ptp),
            0);

  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_idx(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kTargetTai);
  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_),
            (kTargetTai + kFramePeriodNs / 2) / kFramePeriodNs);
  EXPECT_TRUE(ut_txv_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txv_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + kTargetTai - kCurrentTai);
  EXPECT_EQ(packet_ptp, kTargetTai);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_exceed_frame_time(ctx_), 0u);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, FrameTaskletLateRecoveryCountsExactSkippedSlots) {
  ut_txv_set_cur_epochs(ctx_, 2);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 7u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_late_last_delta(ctx_), 7u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
}

TEST_F(St20TxSyncPacingTest, FrameTaskletOnwardRecoveryCountsExactGap) {
  constexpr uint64_t onward_recovery_tai = 6 * kFramePeriodNs;
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, onward_recovery_tai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 4u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  EXPECT_EQ(packet_ptp, onward_recovery_tai);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
}

TEST_F(St20TxSyncPacingTest, FrameTaskletInvalidTimestampStillBuildsFrame) {
  constexpr uint64_t invalid_past_tai = 5 * kFramePeriodNs;
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, invalid_past_tai,
                                     &packet_tsc, &packet_ptp),
            0);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_TRUE(ut_txv_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
}

TEST_F(St20TxSyncPacingTest, ExactPastHalfFrameTimestampCountsOneError) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kPastTai), 0);

  EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, ExactTimestampOneNanosecondPastCountsOneError) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kCurrentTai - 1), 0);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, ExactTimestampAtCurrentTimeIsValid) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kCurrentTai), 0);

  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, ExactTimestampOneNanosecondFutureIsValid) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kCurrentTai + 1), 0);

  /* 1ns is far inside the same 90kHz tick as kCurrentTai, so the media-clock
   * snap rounds it back down to kCurrentTai. */
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  ExpectNoPacingStats();
}

TEST_F(St20TxSyncPacingTest, ConsecutiveUserTimestampsReplaceInternalEpochState) {
  constexpr uint64_t first_target_tai = 12 * kFramePeriodNs;
  constexpr uint64_t second_target_tai = 13 * kFramePeriodNs;
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kStaleEpoch);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, first_target_tai), 0);
  EXPECT_EQ(ut_txv_cur_epochs(ctx_), first_target_tai / kFramePeriodNs);

  ut_txv_set_mock_ptp_time(ctx_, kAlignedTargetTai);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, second_target_tai), 0);
  EXPECT_EQ(ut_txv_cur_epochs(ctx_), second_target_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, InvalidMediaClockTimestampFallsBackToDefaultPacing) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai = ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                                     kInvalidMediaClockTimestamp);
  ASSERT_EQ(required_tai, 0u);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txv_cur_epochs(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, ExactUnsupportedMediaClockCountsOneError) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       kInvalidMediaClockTimestamp),
            0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St20TxSyncPacingTest, ExactTimestampNearUint64MaxSaturatesTscTarget) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, 2 * kCurrentTai);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, UINT64_MAX), 0);

  EXPECT_EQ((uint64_t)ut_txv_tsc_time_cursor(ctx_), UINT64_MAX);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxSyncPacingTest, AlignedTimestampNearUint64MaxDoesNotSendImmediately) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, UINT64_MAX), 0);

  EXPECT_GT(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxSyncPacingTest, ExactTimestampAtInt64MaxRemainsInFuture) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, INT64_MAX), 0);

  EXPECT_GT(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxSyncPacingTest, AlignedTimestampAtInt64MaxRemainsInFuture) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, INT64_MAX), 0);

  EXPECT_GT(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St20TxSyncPacingTest, ExactUserPacingWithZeroRequiredTaiShouldBeFlagged) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, /*timestamp=*/0),
            0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u)
      << "EXACT_USER_PACING with timestamp=0 should be flagged as an invalid "
         "request";
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, ExactUserPacingWithZeroTimestampFallsBackToDefaultPacing) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai = ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
  ASSERT_EQ(required_tai, 0u);
  ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txv_cur_epochs(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, FrameTaskletExactZeroTimestampUsesDefaultFirstPacketTarget) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  EXPECT_EQ(ut_txv_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kCurrentEpoch);
  EXPECT_TRUE(ut_txv_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txv_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc);
  EXPECT_EQ(packet_ptp, kCurrentTai);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_exceed_frame_time(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, RtpLevelExactUserPacingWithZeroRequiredTaiNeverFlags) {
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, /*required_tai=*/0), 0);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u)
      << "tv_sync_pacing() alone (the RTP-level call path) must never flag "
         "stat_error_user_timestamp regardless of EXACT_USER_PACING";
  EXPECT_EQ(ut_txv_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txv_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txv_notify_late_calls(ctx_), 0);
}

TEST_F(St20TxSyncPacingTest, NegativeTimeToTxClampsToZero) {
  constexpr uint32_t large_vrx_packets = 2 * 1000;
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch); /* steady state -> frame_count becomes 10 */
  ut_txv_set_tr_offset(ctx_, 0.0L);
  ut_txv_set_vrx(ctx_, large_vrx_packets);
  ut_txv_set_trs(ctx_, kPacketIntervalNs);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txv_tsc_time_frame_start(ctx_), kCurrentTsc);
  ExpectNoPacingStats();
}

class St20TxTransmitterBoundaryTest
    : public St20TxSyncPacingTest,
      public ::testing::WithParamInterface<enum ut_txv_pacing_way> {};

TEST_P(St20TxTransmitterBoundaryTest, TargetOneNanosecondBelowOneSecondWaits) {
  int bursts_before_target = -1;
  int bursts_at_target = -1;

  ASSERT_EQ(ut_txv_run_transmitter_boundary(ctx_, GetParam(), kNanosecondsPerSecond - 1,
                                            &bursts_before_target, &bursts_at_target),
            0);
  EXPECT_EQ(bursts_before_target, 0);
  EXPECT_EQ(bursts_at_target, 1);
}

TEST_P(St20TxTransmitterBoundaryTest, TargetAtOneSecondWaits) {
  int bursts_before_target = -1;
  int bursts_at_target = -1;

  ASSERT_EQ(ut_txv_run_transmitter_boundary(ctx_, GetParam(), kNanosecondsPerSecond,
                                            &bursts_before_target, &bursts_at_target),
            0);
  EXPECT_EQ(bursts_before_target, 0);
  EXPECT_EQ(bursts_at_target, 1);
}

TEST_P(St20TxTransmitterBoundaryTest, TargetBeyondOneSecondDoesNotRemainPending) {
  int bursts_before_target = -1;
  int bursts_at_target = -1;

  ASSERT_EQ(ut_txv_run_transmitter_boundary(ctx_, GetParam(), kNanosecondsPerSecond + 1,
                                            &bursts_before_target, &bursts_at_target),
            0);
  EXPECT_EQ(bursts_before_target, 1);
  EXPECT_EQ(bursts_at_target, 1);
}

INSTANTIATE_TEST_SUITE_P(AllSoftwareWaitPaths, St20TxTransmitterBoundaryTest,
                         ::testing::Values(UT_TXV_PACING_TSC, UT_TXV_PACING_PTP,
                                           UT_TXV_PACING_RL));

TEST_F(St20TxSyncPacingTest, ExactTargetBeyondOneSecondFallsBackBeforePacketBuild) {
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI,
                                     kCurrentTai + kNanosecondsPerSecond + 1, &packet_tsc,
                                     &packet_ptp),
            0);
  EXPECT_EQ(packet_tsc, kCurrentTsc);
  EXPECT_EQ(packet_ptp, kCurrentTai);
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_notify_frame_done_epoch(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}
