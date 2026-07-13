/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "session/st40_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint64_t kCurrentEpoch = kCurrentTai / kFramePeriodNs;
constexpr uint64_t kInitialEpoch = kCurrentEpoch - 1;
constexpr uint64_t kTargetTai = kCurrentTai + kFramePeriodNs / 2;
constexpr uint64_t kAlignedTargetTai = kCurrentTai + kFramePeriodNs;
constexpr uint64_t kPastTai = kCurrentTai - kFramePeriodNs / 2;
constexpr uint64_t kMediaClockRate = ST10_VIDEO_SAMPLING_RATE_90K;
constexpr uint64_t kMediaClockModulus = static_cast<uint64_t>(UINT32_MAX) + 1;
constexpr uint64_t kStaleEpoch = 1000;
constexpr uint64_t kWrapTestMediaClockEra = 30000;
constexpr uint64_t kZeroTimestampMediaClockEra = 37300;

constexpr uint64_t MediaClockToNanoseconds(uint64_t ticks) {
  return (static_cast<__uint128_t>(ticks) * kNanosecondsPerSecond + kMediaClockRate / 2) /
         kMediaClockRate;
}

constexpr uint64_t AlignToFrame(uint64_t tai) {
  return ((tai + kFramePeriodNs / 2) / kFramePeriodNs) * kFramePeriodNs;
}
}  // namespace

class St40TxPacingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_txa_destroy(ctx_);
  }

  void ExpectNoPacingStats() {
    EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
    EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
    EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
    EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
    EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
  }

  ut_txa_ctx* ctx_ = nullptr;
};

TEST_F(St40TxPacingTest, SteadyStateAdvancesByOneEpoch) {
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, 0), kCurrentEpoch);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, OnwardBoundaryIsInclusive) {
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, 7 * kFramePeriodNs, 0), kCurrentEpoch);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, OnwardGapBeyondBoundaryResyncs) {
  constexpr uint64_t onward_recovery_tai = 6 * kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, onward_recovery_tai, 0),
            onward_recovery_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 4u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, RepeatedOnwardRecoveryAccumulatesExactGaps) {
  constexpr uint64_t first_recovery_tai = 6 * kFramePeriodNs;
  constexpr uint64_t second_recovery_tai = 8 * kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, first_recovery_tai, 0),
            first_recovery_tai / kFramePeriodNs);
  ut_txa_set_cur_epochs(ctx_, 12);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, second_recovery_tai, 0),
            second_recovery_tai / kFramePeriodNs);

  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 9u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, AppTargetDoesNotReportStaleOnwardState) {
  ut_txa_set_cur_epochs(ctx_, kStaleEpoch);
  constexpr uint64_t target_tai = 12 * kFramePeriodNs;
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, target_tai),
            target_tai / kFramePeriodNs);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, AppTargetDoesNotReportStaleLateState) {
  ut_txa_set_cur_epochs(ctx_, 2);
  constexpr uint64_t target_tai = 100 * kFramePeriodNs;
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, target_tai),
            target_tai / kFramePeriodNs);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, RepeatedLateFramesAccumulateExactDropsAndCallbacks) {
  constexpr uint64_t late_tai = 13 * kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, 2);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, 0), kCurrentEpoch);
  ut_txa_set_cur_epochs(ctx_, kCurrentEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, late_tai, 0), late_tai / kFramePeriodNs);

  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 9u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 2);
  EXPECT_EQ(ut_txa_notify_late_last_delta(ctx_), 2u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
}

TEST_F(St40TxPacingTest, RequiredTaiRoundingToZeroFallsBackToCurrentEpoch) {
  constexpr uint64_t subframe_target_tai = 10;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, subframe_target_tai), kCurrentEpoch);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, FutureValidationBoundaryIsInclusive) {
  constexpr uint64_t future_boundary_tai = kCurrentTai + kNanosecondsPerSecond;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, future_boundary_tai),
            future_boundary_tai / kFramePeriodNs);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, TimestampBeyondFutureBoundaryIsPreservedAndFlagged) {
  constexpr uint64_t beyond_future_boundary_tai =
      kCurrentTai + kNanosecondsPerSecond + kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, beyond_future_boundary_tai),
            beyond_future_boundary_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, RepeatedInvalidTimestampsAccumulateWithoutEpochRecovery) {
  constexpr uint64_t invalid_past_tai = 5 * kFramePeriodNs;
  constexpr uint64_t invalid_future_tai =
      kCurrentTai + kNanosecondsPerSecond + kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, invalid_past_tai),
            invalid_past_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, kCurrentTai, invalid_future_tai),
            invalid_future_tai / kFramePeriodNs);

  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 2u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, CurEpochWrapResyncsWithoutRecoveryStats) {
  ut_txa_set_cur_epochs(ctx_, UINT64_MAX);
  constexpr uint64_t current_tai = 5 * kFramePeriodNs;
  EXPECT_EQ(ut_txa_calc_epoch(ctx_, current_tai, 0), current_tai / kFramePeriodNs);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, TimestampIsIgnoredWithoutUserPacing) {
  EXPECT_EQ(ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai), 0u);
}

TEST_F(St40TxPacingTest, UserPacingAlignsTimestampToEpoch) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai =
      ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai);
  ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txa_cur_epochs(ctx_), kAlignedTargetTai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc + kAlignedTargetTai - kCurrentTai);
}

TEST_F(St40TxPacingTest, FrameTaskletUserPacingAlignsFirstPacketTarget) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(
      ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai, &packet_tsc), 0);

  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_notify_frame_done_idx(ctx_), 0u);
  EXPECT_TRUE(ut_txa_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txa_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + kAlignedTargetTai - kCurrentTai);
  EXPECT_EQ(ut_txa_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_bytes(ctx_), ut_txa_packet_len(ctx_));
  EXPECT_GT(ut_txa_packet_len(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_recoverable_error(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_unrecoverable_error(ctx_), 0u);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, FrameTaskletExactUserPacingUsesFirstPacketTargetVerbatim) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(
      ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai, &packet_tsc), 0);

  EXPECT_EQ(ut_txa_get_next_frame_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 1);
  EXPECT_TRUE(ut_txa_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txa_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + kTargetTai - kCurrentTai);
  EXPECT_EQ(ut_txa_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_bytes(ctx_), ut_txa_packet_len(ctx_));
  EXPECT_GT(ut_txa_packet_len(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, FrameTaskletLateRecoveryCountsExactSkippedSlots) {
  ut_txa_set_cur_epochs(ctx_, 2);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc), 0);

  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 7u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 1);
  EXPECT_EQ(ut_txa_notify_late_last_delta(ctx_), 7u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
}

TEST_F(St40TxPacingTest, FrameTaskletOnwardRecoveryCountsExactGap) {
  constexpr uint64_t onward_recovery_tai = 6 * kFramePeriodNs;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, onward_recovery_tai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc), 0);

  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 4u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
  EXPECT_EQ(ut_txa_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
}

TEST_F(St40TxPacingTest, FrameTaskletInvalidTimestampStillBuildsFrame) {
  constexpr uint64_t invalid_past_tai = 5 * kFramePeriodNs;
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, invalid_past_tai,
                                     &packet_tsc),
            0);

  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 1u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 1);
  EXPECT_TRUE(ut_txa_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txa_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_port_frames(ctx_), 1u);
}

TEST_F(St40TxPacingTest, ExactUserPacingUsesTimestampVerbatim) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kTargetTai), 0);

  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), kTargetTai);
  EXPECT_EQ(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc + kTargetTai - kCurrentTai);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, ExactPastHalfFrameTimestampCountsErrorAndMismatch) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kPastTai), 0);

  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, ExactTimestampOneNanosecondPastCountsErrorAndMismatch) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kCurrentTai - 1), 0);

  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 1u);
  EXPECT_EQ(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, ExactTimestampAtCurrentTimeDoesNotCountMismatch) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kCurrentTai), 0);

  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, ExactTimestampOneNanosecondFutureIsValid) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kCurrentTai + 1), 0);

  EXPECT_EQ(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc + 1);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, RepeatedEpochMismatchAccumulates) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kPastTai), 0);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ASSERT_EQ(ut_txa_sync_pacing(ctx_, kPastTai), 0);

  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 2u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 2u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_late_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, MediaClockUserPacingAlignsConvertedTimestamp) {
  ut_txa_set_user_pacing(ctx_, true);
  constexpr uint64_t media_clock_timestamp = 945;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai = ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                                     media_clock_timestamp);
  ASSERT_EQ(required_tai, MediaClockToNanoseconds(media_clock_timestamp));
  ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), kAlignedTargetTai);
}

TEST_F(St40TxPacingTest, MediaClockExactUserPacingUsesConvertedTimestampVerbatim) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  constexpr uint64_t media_clock_timestamp = 945;
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai = ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                                     media_clock_timestamp);
  ASSERT_EQ(required_tai, MediaClockToNanoseconds(media_clock_timestamp));
  ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), MediaClockToNanoseconds(media_clock_timestamp));
}

TEST_F(St40TxPacingTest, MediaClockExactTaskletUnwrapsFutureAcrossUint32Wrap) {
  constexpr uint64_t media_clock_era = kWrapTestMediaClockEra;
  constexpr uint64_t current_ticks =
      media_clock_era * kMediaClockModulus + UINT32_MAX - 9;
  constexpr uint64_t target_ticks = (media_clock_era + 1) * kMediaClockModulus + 5;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t target_tai = MediaClockToNanoseconds(target_ticks);
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, current_tai / kFramePeriodNs);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 5, &packet_tsc),
            0);

  EXPECT_EQ(packet_tsc, kCurrentTsc + target_tai - current_tai);
  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), target_tai);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, MediaClockAlignedTaskletUnwrapsFutureAcrossUint32Wrap) {
  constexpr uint64_t media_clock_era = kWrapTestMediaClockEra;
  constexpr uint64_t current_ticks =
      media_clock_era * kMediaClockModulus + UINT32_MAX - 9;
  constexpr uint64_t target_ticks = (media_clock_era + 1) * kMediaClockModulus + 100;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t aligned_target_tai =
      AlignToFrame(MediaClockToNanoseconds(target_ticks));
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, current_tai / kFramePeriodNs);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(
      ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 100, &packet_tsc), 0);

  EXPECT_EQ(packet_tsc, kCurrentTsc + aligned_target_tai - current_tai);
  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), aligned_target_tai);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, MediaClockExactTaskletFallsBackForPastWrappedTarget) {
  constexpr uint64_t media_clock_era = kWrapTestMediaClockEra;
  constexpr uint64_t current_ticks = (media_clock_era + 1) * kMediaClockModulus + 10;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t fallback_tai = ((current_tai / kFramePeriodNs) + 1) * kFramePeriodNs;
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, current_tai / kFramePeriodNs);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, UINT32_MAX - 5,
                                     &packet_tsc),
            0);

  EXPECT_EQ(packet_tsc, kCurrentTsc + fallback_tai - current_tai);
  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), fallback_tai);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_), fallback_tai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), fallback_tai);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, MediaClockZeroUnwrapsAfterUint32WrapForUserPacing) {
  constexpr uint64_t media_clock_era = kZeroTimestampMediaClockEra;
  constexpr uint64_t current_ticks = media_clock_era * kMediaClockModulus + UINT32_MAX;
  constexpr uint64_t target_ticks = (media_clock_era + 1) * kMediaClockModulus;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t target_tai = MediaClockToNanoseconds(target_ticks);
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);

  EXPECT_EQ(ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 0),
            target_tai);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, MediaClockZeroUnwrapsAfterUint32WrapForExactPacing) {
  constexpr uint64_t media_clock_era = kZeroTimestampMediaClockEra;
  constexpr uint64_t current_ticks = media_clock_era * kMediaClockModulus + UINT32_MAX;
  constexpr uint64_t target_ticks = (media_clock_era + 1) * kMediaClockModulus;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t target_tai = MediaClockToNanoseconds(target_ticks);
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);

  EXPECT_EQ(ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 0),
            target_tai);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, MediaClockZeroExactTaskletUsesWrappedFirstPacketTarget) {
  constexpr uint64_t media_clock_era = kZeroTimestampMediaClockEra;
  constexpr uint64_t current_ticks = media_clock_era * kMediaClockModulus + UINT32_MAX;
  constexpr uint64_t target_ticks = (media_clock_era + 1) * kMediaClockModulus;
  constexpr uint64_t current_tai = MediaClockToNanoseconds(current_ticks);
  constexpr uint64_t target_tai = MediaClockToNanoseconds(target_ticks);
  constexpr uint64_t target_epoch = (target_tai + kFramePeriodNs / 2) / kFramePeriodNs;
  uint64_t packet_tsc = 0;
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, current_tai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 0, 1), 0);
  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 0);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
  EXPECT_EQ(ut_txa_cur_epochs(ctx_), target_epoch);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc + target_tai - current_tai);
  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 1);
  ASSERT_EQ(ut_txa_pop_packet_tsc(ctx_, &packet_tsc), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + target_tai - current_tai);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_), target_epoch);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), target_tai);
  EXPECT_EQ(ut_txa_notify_frame_done_tfmt(ctx_), ST10_TIMESTAMP_FMT_TAI);
  ExpectNoPacingStats();
}

TEST_F(St40TxPacingTest, ExactTargetOneNanosecondBelowOneSecondWaits) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI,
                                         kCurrentTai + kNanosecondsPerSecond - 1, 1),
            0);

  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 0);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 0);
}

TEST_F(St40TxPacingTest, ExactTargetAtOneSecondWaits) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI,
                                         kCurrentTai + kNanosecondsPerSecond, 1),
            0);

  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 0);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 0u);
  EXPECT_EQ(ut_txa_notify_frame_done_calls(ctx_), 0);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc + kNanosecondsPerSecond);
  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u);
}

TEST_F(St40TxPacingTest, ExactTargetBeyondOneSecondFallsBackWithoutWaiting) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  ASSERT_EQ(ut_txa_prepare_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI,
                                         kCurrentTai + kNanosecondsPerSecond + 1, 1),
            0);

  EXPECT_EQ(ut_txa_step_frame_tasklet(ctx_), 1);
  EXPECT_EQ(ut_txa_queued_packets(ctx_), 1u);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St40TxPacingTest, ForwardAlignedTargetUpdatesCompletionMetadata) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(
      ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai, &packet_tsc), 0);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_), kAlignedTargetTai / kFramePeriodNs);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txa_notify_frame_done_tfmt(ctx_), ST10_TIMESTAMP_FMT_TAI);
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ctx_),
            kAlignedTargetTai * kMediaClockRate / kNanosecondsPerSecond);
}

TEST_F(St40TxPacingTest, ForwardExactTargetUpdatesCompletionMetadata) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(
      ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai, &packet_tsc), 0);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_),
            (kTargetTai + kFramePeriodNs / 2) / kFramePeriodNs);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), kTargetTai);
  EXPECT_EQ(ut_txa_notify_frame_done_tfmt(ctx_), ST10_TIMESTAMP_FMT_TAI);
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ctx_),
            kTargetTai * kMediaClockRate / kNanosecondsPerSecond);
}

TEST_F(St40TxPacingTest, BackwardExactTargetFallsBackCompletionMetadata) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;

  ASSERT_EQ(ut_txa_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai - 1,
                                     &packet_tsc),
            0);
  EXPECT_EQ(ut_txa_notify_frame_done_epoch(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txa_notify_frame_done_timestamp(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ctx_),
            kCurrentTai * kMediaClockRate / kNanosecondsPerSecond);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St40TxPacingTest, OversizedMediaClockUserPacingCountsOneError) {
  ut_txa_set_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       (uint64_t)UINT32_MAX + 1),
            0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St40TxPacingTest, OversizedMediaClockExactPacingCountsOneError) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       (uint64_t)UINT32_MAX + 1),
            0u);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St40TxPacingTest, ExactTimestampNearUint64MaxSaturatesTscTarget) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, 2 * kCurrentTai);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, UINT64_MAX), 0);

  EXPECT_EQ((uint64_t)ut_txa_tsc_time_cursor(ctx_), UINT64_MAX);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, AlignedTimestampNearUint64MaxDoesNotSendImmediately) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, UINT64_MAX), 0);

  EXPECT_GT(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, ExactTimestampAtInt64MaxRemainsInFuture) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, INT64_MAX), 0);

  EXPECT_GT(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, AlignedTimestampAtInt64MaxRemainsInFuture) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  ASSERT_EQ(ut_txa_sync_pacing(ctx_, INT64_MAX), 0);

  EXPECT_GT(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc);
  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_drop(ctx_), 0u);
  EXPECT_EQ(ut_txa_stat_epoch_onward(ctx_), 0u);
}

TEST_F(St40TxPacingTest, ExactZeroTimestampIsFlaggedAndFallsBackToEpochPacing) {
  ut_txa_set_user_pacing(ctx_, true);
  ut_txa_set_exact_user_pacing(ctx_, true);
  ut_txa_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);

  uint64_t required_tai = ut_txa_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
  ASSERT_EQ(required_tai, 0u);
  ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0);

  EXPECT_EQ(ut_txa_stat_error_user_timestamp(ctx_), 1u);
  EXPECT_EQ(ut_txa_cur_epochs(ctx_), kCurrentEpoch);
  EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txa_tsc_time_cursor(ctx_), kCurrentTsc);
}
