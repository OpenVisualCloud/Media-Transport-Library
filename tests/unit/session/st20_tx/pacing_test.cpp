/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins two things in st_tx_video_session.c:
 *  1. The cursor-derivation math in `tv_sync_pacing()`: normal tsc/ptp cursor
 *     derivation, the ST20_TX_FLAG_EXACT_USER_PACING bypass, and the negative
 *     time_to_tx_ns clamp.
 *  2. `tv_update_rtp_time_stamp()`'s contract that the TAI instant it returns
 *     (reported to the app as frame->timestamp / frame->tx_st22_meta.timestamp)
 *     always reconstructs frame->rtp_timestamp via st10_tai_to_media_clk(),
 *     across USER_TIMESTAMP, RTP_TIMESTAMP_EPOCH, rtp_timestamp_delta_us, and
 *     the ST10_TIMESTAMP_FMT_MEDIA_CLK input format -- both at the shared
 *     tv_tasklet_frame() call site and the tv_tasklet_st22() one. The returned
 *     instant must track whichever basis actually produced
 *     frame->rtp_timestamp, not unconditionally `pacing->ptp_time_cursor`, or
 *     the two fields would silently disagree.
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
constexpr uint64_t kOffTickExactTargetTai = kTargetTai + 1;
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

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, kOffTickExactTargetTai), 0);

  EXPECT_EQ(ut_txv_ptp_time_cursor(ctx_), kOffTickExactTargetTai);
  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_),
            kCurrentTsc + kOffTickExactTargetTai - kCurrentTai);
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
  // Plain pacing (no USER_TIMESTAMP, no RTP_TIMESTAMP_EPOCH, no
  // rtp_timestamp_delta_us) is the most common production path: here
  // frame->rtp_timestamp is derived directly from frame->timestamp, so the
  // two must always reconstruct each other via st10_tai_to_media_clk().
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_TRUE(ut_txv_frame_is_waiting(ctx_));
  EXPECT_EQ(ut_txv_frame_refcnt(ctx_), 0);
  EXPECT_EQ(packet_tsc, kCurrentTsc + kAlignedTargetTai - kCurrentTai);
  EXPECT_EQ(packet_ptp, kAlignedTargetTai);
  EXPECT_EQ(ut_txv_stat_port_build(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_port_frames(ctx_), 1u);
  EXPECT_EQ(ut_txv_stat_exceed_frame_time(ctx_), 0u);
  ExpectNoPacingStats();
}

// USER_PACING + USER_TIMESTAMP with a TAI timestamp that does not land on an
// epoch boundary. kTargetTai (mid-frame) makes tv_sync_pacing() align the
// actual TX instant (packet_ptp) to the next epoch, kAlignedTargetTai -- but
// tv_update_rtp_time_stamp()'s USER_TIMESTAMP branch must still derive
// frame->rtp_timestamp from the raw, unaligned kTargetTai the app supplied
// (that IS the documented USER_TIMESTAMP contract: "assign the rtp timestamp
// to the value of timestamp"). If frame->timestamp were instead hardcoded to
// the aligned/scheduled instant (kAlignedTargetTai) rather than the raw one
// the RTP value actually came from, converting the reported frame->timestamp
// back to media-clock ticks would give a different number than the reported
// frame->rtp_timestamp -- a constant offset between the two fields.
TEST_F(St20TxSyncPacingTest, FrameTaskletUserTimestampReportsRtpConsistentTai) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai,
                                     &packet_tsc, &packet_ptp),
            0);

  // The wire schedule still targets the aligned epoch -- USER_TIMESTAMP only
  // changes what gets reported, never the actual pacing decision.
  EXPECT_EQ(packet_ptp, kAlignedTargetTai);
  // Reported frame->timestamp is the same raw instant the RTP timestamp came
  // from, not the aligned instant tv_sync_pacing() actually scheduled.
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kTargetTai);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(kTargetTai, ST10_VIDEO_SAMPLING_RATE_90K));
  // Whatever frame->timestamp is, converting it to media-clock ticks must
  // reproduce frame->rtp_timestamp.
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K))
      << "rtp_timestamp reported via notify_frame_done must derive from the same "
         "TAI instant reported in frame->timestamp";
}

// The non-USER_TIMESTAMP branch of tv_update_rtp_time_stamp() computes
// tai_for_rtp_ts (epoch or ptp_time_cursor, per RTP_TIMESTAMP_EPOCH) and feeds
// it into st10_tai_to_media_clk() to get frame->rtp_timestamp, so that same
// tai_for_rtp_ts -- not pacing->ptp_time_cursor -- must be what gets reported
// as frame->timestamp. RTP_TIMESTAMP_EPOCH is the case where those two values
// genuinely differ: it derives the RTP timestamp from the bare epoch,
// deliberately omitting tr_offset. The 100us tr_offset here makes packet_ptp
// (the actual TX target, which still includes tr_offset) diverge from the
// epoch by an amount large enough to cross a 90kHz tick boundary (900 vs
// 909), so reporting ptp_time_cursor instead of tai_for_rtp_ts would make
// frame->timestamp fail to reconstruct frame->rtp_timestamp.
TEST_F(St20TxSyncPacingTest, FrameTaskletRtpTimestampEpochReportsRtpConsistentTai) {
  ut_txv_set_rtp_timestamp_epoch(ctx_, true);
  ut_txv_set_tr_offset(ctx_, 100000.0L); /* pushes ptp_time_cursor off the epoch */
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  // Actual TX target: epoch + tr_offset, unaffected by RTP_TIMESTAMP_EPOCH.
  EXPECT_EQ(packet_ptp, kCurrentTai + 100000u);
  // Reported frame->timestamp must track the bare epoch instead: that is what
  // RTP_TIMESTAMP_EPOCH actually derived frame->rtp_timestamp from.
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kCurrentTai);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(kCurrentTai, ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
}

// The same tai_for_rtp_ts-vs-ptp_time_cursor distinction, via the other input
// that reaches it: a nonzero ops.rtp_timestamp_delta_us adds delta_ns onto
// tai_for_rtp_ts before it is converted to media-clock ticks, but the actual
// TX target (packet_ptp / pacing->ptp_time_cursor) is never shifted by the
// delta -- the delta exists purely to offset the RTP value. 500us is
// comfortably more than one 90kHz tick (~11.1us), so reporting
// ptp_time_cursor instead (ignoring the delta) would make frame->timestamp
// reconstruct to 900 while frame->rtp_timestamp is 945.
TEST_F(St20TxSyncPacingTest, FrameTaskletRtpTimestampDeltaReportsRtpConsistentTai) {
  constexpr uint64_t kDeltaNs = 500000; /* 500us */
  ut_txv_set_rtp_timestamp_delta_us(ctx_, kDeltaNs / 1000);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(
      ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, &packet_tsc, &packet_ptp),
      0);

  // The delta never touches the actual TX schedule.
  EXPECT_EQ(packet_ptp, kCurrentTai);
  // But the reported frame->timestamp must include it, since that is what
  // frame->rtp_timestamp was actually derived from.
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kCurrentTai + kDeltaNs);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(kCurrentTai + kDeltaNs, ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
}

// In the USER_TIMESTAMP branch, when the app's timestamp arrives as
// ST10_TIMESTAMP_FMT_MEDIA_CLK (a documented valid input, see
// include/st20_api.h), st10_get_media_clk() returns it verbatim -- it has no
// notion of ptp_time_cursor at all. The correct way to recover a reportable
// TAI instant from it is st10_get_tai(), which turns the MEDIA_CLK value back
// into TAI ns; falling back to pacing->ptp_time_cursor for this format would
// reintroduce the same mismatch as the non-MEDIA_CLK case above.
// kMediaClockTimestamp=990 ticks is exactly 11ms -- deliberately the *next*
// epoch's boundary (kAlignedTargetTai), not the one default (non-USER_PACING)
// pacing schedules for this frame (kCurrentTai, 10ms) -- so a wrong
// ptp_time_cursor fallback is distinguishable from the correct answer.
TEST_F(St20TxSyncPacingTest, FrameTaskletUserTimestampMediaClkReportsRtpConsistentTai) {
  constexpr uint32_t kMediaClockTimestamp = 990;
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                     kMediaClockTimestamp, &packet_tsc, &packet_ptp),
            0);

  // USER_TIMESTAMP without USER_PACING: the app's timestamp only feeds the
  // RTP derivation, not the TX schedule -- packet_ptp keeps advancing on the
  // default (non-user) epoch, unrelated to kMediaClockTimestamp.
  EXPECT_EQ(packet_ptp, kCurrentTai);
  // Reported frame->timestamp must be the TAI instant st10_get_tai() derived
  // from kMediaClockTimestamp, not the unrelated TX schedule above.
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), kMediaClockTimestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
}

// Documents existing, unfixed behavior: tv_update_rtp_time_stamp()'s
// USER_TIMESTAMP branch does `timestamp += delta_ns`, unconditionally
// treating rtp_timestamp_delta_us as nanoseconds. When tfmt is
// ST10_TIMESTAMP_FMT_TAI that's correct, but when tfmt is
// ST10_TIMESTAMP_FMT_MEDIA_CLK, `timestamp` is in media-clock ticks, not
// nanoseconds -- so the "nanosecond" delta is silently added as extra ticks.
// This test does not assert that combination is correct; it pins the current
// arithmetic exactly (mirroring the same unit-mismatched addition) so a
// future change to this code path is a deliberate, reviewed decision, not an
// accidental behavior change. It also verifies that frame->rtp_timestamp and
// frame->timestamp are both derived from the same (corrupted) intermediate
// value, so one still reconstructs the other -- only the absolute values are
// questionable, not the reported pair's internal consistency.
TEST_F(St20TxSyncPacingTest,
       FrameTaskletUserTimestampMediaClkWithDeltaPinsKnownUnitMismatch) {
  constexpr uint32_t kMediaClockTimestamp = 990;
  constexpr int32_t kDeltaUs = 500;
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_rtp_timestamp_delta_us(ctx_, kDeltaUs);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                     kMediaClockTimestamp, &packet_tsc, &packet_ptp),
            0);

  // Mirrors the production `timestamp += delta_ns` unit mismatch: delta_us
  // converted to "nanoseconds" gets added directly onto media-clock ticks.
  const uint64_t corrupted_ticks = kMediaClockTimestamp + (uint64_t)kDeltaUs * 1000;
  const uint32_t expected_rtp_timestamp = (uint32_t)corrupted_ticks;
  const uint64_t expected_timestamp =
      st10_media_clk_to_ns(expected_rtp_timestamp, ST10_VIDEO_SAMPLING_RATE_90K);

  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), expected_rtp_timestamp);
  // Even under this known-bad input, the pair stays self-consistent.
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
}

// tv_tasklet_st22() (compressed video) has its own, separate call site that
// assigns frame->tx_st22_meta.timestamp from tv_update_rtp_time_stamp()'s
// return value -- same shared function as tv_tasklet_frame(), but a distinct
// line of production code. This repeats the USER_TIMESTAMP + mid-frame TAI
// scenario from the first test above through that call site, so a regression
// that only touches one of the two assignments is still caught.
TEST_F(St20TxSyncPacingTest, St22FrameStepUserTimestampReportsRtpConsistentTai) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kInitialEpoch);
  ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t frame_timestamp = 0;
  uint32_t frame_rtp_timestamp = 0;

  ASSERT_EQ(ut_txv_run_st22_next_frame_step(ctx_, ST10_TIMESTAMP_FMT_TAI, kTargetTai,
                                            &frame_timestamp, &frame_rtp_timestamp),
            0);

  EXPECT_EQ(frame_timestamp, kTargetTai);
  EXPECT_EQ(frame_rtp_timestamp,
            st10_tai_to_media_clk(kTargetTai, ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_EQ(frame_rtp_timestamp,
            st10_tai_to_media_clk(frame_timestamp, ST10_VIDEO_SAMPLING_RATE_90K));
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

  EXPECT_EQ(ut_txv_tsc_time_cursor(ctx_), kCurrentTsc + 1);
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
