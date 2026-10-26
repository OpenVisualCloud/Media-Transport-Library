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

#include <string>

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
// include/st20_api.h), the raw 32-bit tick count is unwrapped against
// pacing->ptp_time_cursor (the anchor tv_sync_pacing() just computed) rather
// than expanded from zero, so the reported TAI instant lands near the
// session's real clock instead of near the MEDIA_CLK epoch.
// kMediaClockTimestamp=990 ticks is exactly 11ms -- deliberately the *next*
// epoch's boundary (kAlignedTargetTai), not the one default (non-USER_PACING)
// pacing schedules for this frame (kCurrentTai, 10ms) -- so an unanchored
// fallback would be distinguishable from the correct answer.
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
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), kAlignedTargetTai);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), kMediaClockTimestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
}

// rtp_timestamp_delta_us must be applied in the TAI/ns domain even when the
// app's timestamp arrives as ST10_TIMESTAMP_FMT_MEDIA_CLK: the raw ticks are
// first unwrapped to a TAI instant anchored on pacing->ptp_time_cursor, then
// the delta (converted to ns) is added, then the result is re-derived back to
// media-clock ticks -- never added directly onto the raw tick count.
TEST_F(St20TxSyncPacingTest, FrameTaskletUserTimestampMediaClkAppliesDeltaInTaiDomain) {
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

  // kMediaClockTimestamp unwraps to kAlignedTargetTai (see the test above);
  // the 500us delta is then added on top of that TAI instant, in ns.
  const uint64_t expected_timestamp = kAlignedTargetTai + (uint64_t)kDeltaUs * 1000;
  const uint32_t expected_rtp_timestamp =
      st10_tai_to_media_clk(expected_timestamp, ST10_VIDEO_SAMPLING_RATE_90K);

  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), expected_rtp_timestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
  // Plausibility, not just round-trip self-consistency: within one frame
  // period of the session's real clock (kCurrentTai), not near zero.
  EXPECT_NEAR((double)ut_txv_notify_frame_done_timestamp(ctx_), (double)kCurrentTai,
              2.0 * (double)kFramePeriodNs);
}

// Same MEDIA_CLK unwrap, but anchored on a large, realistic TAI epoch instead
// of a small round number near zero -- proving the reconstructed
// frame->timestamp tracks the session's actual clock and not
// st10_media_clk_to_ns()'s zero-based expansion of the raw ticks (which would
// land tens of seconds after the TAI epoch, not near kRealisticTai).
TEST_F(St20TxSyncPacingTest, FrameTaskletUserTimestampMediaClkAnchorsToRealisticEpoch) {
  constexpr uint64_t kRealisticTaiMs = 1755000000000ULL; /* ~2025, in ms */
  constexpr uint64_t kRealisticTai = kRealisticTaiMs * kNanosecondsPerMillisecond;
  const uint32_t anchor_ticks =
      st10_tai_to_media_clk(kRealisticTai, ST10_VIDEO_SAMPLING_RATE_90K);
  const uint32_t kMediaClockTimestamp = anchor_ticks + 90; /* +1ms, wraps mod 2^32 */
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kRealisticTaiMs - 1);
  ut_txv_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                     kMediaClockTimestamp, &packet_tsc, &packet_ptp),
            0);

  const uint64_t expected_timestamp = kRealisticTai + kFramePeriodNs;
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), kMediaClockTimestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_NEAR((double)ut_txv_notify_frame_done_timestamp(ctx_), (double)kRealisticTai,
              (double)kFramePeriodNs);
}

// Same anchored unwrap as above, but with the app's timestamp slightly
// *behind* the anchor tick (media_ts = anchor_ticks - N) rather than ahead of
// it, so st10_media_clk_to_tai()'s negative-diff_ticks path actually runs. A
// wrong wrap direction here would land roughly half a 32-bit tick cycle away
// from kRealisticTai (many hours), not one frame period behind it.
TEST_F(St20TxSyncPacingTest,
       FrameTaskletUserTimestampMediaClkUnwrapsTimestampBehindAnchor) {
  constexpr uint64_t kRealisticTaiMs = 1755000000000ULL; /* ~2025, in ms */
  constexpr uint64_t kRealisticTai = kRealisticTaiMs * kNanosecondsPerMillisecond;
  const uint32_t anchor_ticks =
      st10_tai_to_media_clk(kRealisticTai, ST10_VIDEO_SAMPLING_RATE_90K);
  const uint32_t kMediaClockTimestamp = anchor_ticks - 90; /* -1ms, wraps mod 2^32 */
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_cur_epochs(ctx_, kRealisticTaiMs - 1);
  ut_txv_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  uint64_t packet_tsc = 0;
  uint64_t packet_ptp = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                     kMediaClockTimestamp, &packet_tsc, &packet_ptp),
            0);

  const uint64_t expected_timestamp = kRealisticTai - kFramePeriodNs;
  EXPECT_EQ(ut_txv_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_), kMediaClockTimestamp);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(ut_txv_notify_frame_done_timestamp(ctx_),
                                  ST10_VIDEO_SAMPLING_RATE_90K));
  EXPECT_NEAR((double)ut_txv_notify_frame_done_timestamp(ctx_), (double)kRealisticTai,
              (double)kFramePeriodNs);
}

// st10_media_clk_to_tai() must round-trip exactly for every tick, including at
// a rounding-tie boundary: anchor=10,050,000ns @ 90kHz is exactly tick 904.5
// (a tie, so st10_tai_to_media_clk() rounds it down to 904), and media_ts=909
// is only 5 ticks away. Rounding the anchor and the tick delta as two
// independent steps can push the result across a tick boundary (904.5 + 5 ==
// 909.5, which rounds up to 910) even though the un-rounded math lands
// exactly on tick 909 -- so the only correct check is the exact round-trip,
// not proximity to the anchor.
TEST_F(St20TxSyncPacingTest, UpdateRtpTimeStampMediaClkRoundTripsAtRoundingTie) {
  constexpr uint64_t kTieAnchorTai = 10050000; /* exactly tick 904.5 @ 90kHz */
  constexpr uint32_t kMediaClockTimestamp = 909;
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_ptp_time_cursor(ctx_, kTieAnchorTai);

  ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, kMediaClockTimestamp);

  EXPECT_EQ(ut_txv_rtp_time_stamp(ctx_), kMediaClockTimestamp);
}

// st10_media_clk_to_tai()'s sampling_rate==0 guard must not divide by zero in
// this tasklet-context path; it should degrade the same way the downstream
// st10_tai_to_media_clk() guard already does regardless (returns 0).
TEST_F(St20TxSyncPacingTest, UpdateRtpTimeStampMediaClkZeroSamplingRateDoesNotCrash) {
  ut_txv_set_user_timestamp(ctx_, true);
  ut_txv_set_sampling_clock_rate(ctx_, 0);
  ut_txv_set_ptp_time_cursor(ctx_, kCurrentTai);

  ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, 909);

  EXPECT_EQ(ut_txv_rtp_time_stamp(ctx_), 0u);
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

/* Launch-time pacing leaves the wait to the NIC: the transmitter hands the packet over at
 * once, with its PTP target in the tx timestamp field and the flag the driver keys on. */
TEST_F(St20TxSyncPacingTest, LaunchTimeHandsThePacketOverWithItsPtpTarget) {
  uint64_t launch_time = 0;
  bool launch_flag = false;

  ASSERT_EQ(ut_txv_run_launch_time(ctx_, kCurrentTai, &launch_time, &launch_flag), 0);
  EXPECT_EQ(launch_time, kCurrentTai);
  EXPECT_TRUE(launch_flag);
}

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

/* tv_init_pacing()'s per-port RL warm-up plan: the shaper sends pad_interval + 1
 * packets in pad_interval trs, its full burst bucket, filled at the rate programmed on
 * the queue, leaves at once, and it releases packets on a grid of token timeslots. A plan
 * needs a measured pad_interval, a known bucket and a programmed rate, else packet 0 is
 * held. */
class St20TxRlPlanTest : public ::testing::Test {
 protected:
  static constexpr int kPortP = 0;
  static constexpr int kPortR = 1;
  static constexpr long double kNormalPadSize = 1262;
  static constexpr long double kVfBurstSize = 2048;
  /* tv_rl_bps() of 1080p59.94, 4320 pkts of 1262 B a frame */
  static constexpr uint64_t kRlBps = 326783616;
  /* and of 2160p59.94, 17280 pkts, where half the 512-descriptor ring binds before the
   * bucket refill */
  static constexpr uint64_t kUhdRlBps = 1307134465;
  /* the most tokens ice adds a timeslot below 1.47 GB/s, st_tx_video_rl_tokens() */
  static constexpr long double kQuantumBytes = 257;
  static constexpr long double kTimeslotMaxNs = 2000; /* ST_TX_VIDEO_RL_TIMESLOT_MAX_NS */
  static constexpr double kTolNs = 1e-6;
  const float kTrained[2] = {164, 170};

  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  /* The NORMAL pad's: training drains a frame of 1200-byte pkts of 2.5-byte pixels, a
   * pkt crossing a line end EXTRA and 6 bytes longer, then NORMAL ones to a multiple of
   * 32. Lines 1920 or 3840 pixels wide split into whole pkts: the mean pad is NORMAL. */
  long double Drain(long double pad_interval, uint32_t width = 1920,
                    uint32_t height = 1080) {
    const uint64_t line = width * 5 / 2, frame = line * height;
    const uint64_t pkts = (frame + 1199) / 1200;
    uint64_t extra = 0;
    for (uint64_t i = 0; i < pkts; i++)
      if ((i * 1200 + 1200) > (i * 1200 / line + 1) * line) extra++;
    const long double pads = pkts + 32 - pkts % 32;
    const long double share = kNormalPadSize * pads / (kNormalPadSize * pads + 6 * extra);
    return ut_txv_pacing_trs(ctx_) * pad_interval / (pad_interval + 1) * share;
  }
  /* the shaper time of one token timeslot at most, the plan's aim past the target */
  static long double Aim(long double drain) {
    return kQuantumBytes * drain / kNormalPadSize;
  }
  /* a full bucket at the rate programmed on the queue, plus the aim */
  static long double Credit(long double burst, uint64_t bps, long double drain) {
    return burst * 1e9L / bps + Aim(drain);
  }
  void ExpectHold(int port) {
    EXPECT_FALSE(ut_txv_rl_armed(ctx_, port));
    EXPECT_EQ((double)ut_txv_rl_drain(ctx_, port), 0.0);
    EXPECT_EQ((double)ut_txv_rl_credit(ctx_, port), 0.0);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxRlPlanTest, DrainFollowsEachPortsTrainedPadInterval) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 2, 0, false, kTrained, false), 0);

  EXPECT_NEAR((double)ut_txv_rl_drain(ctx_, kPortP), (double)Drain(164), kTolNs);
  EXPECT_NEAR((double)ut_txv_rl_drain(ctx_, kPortR), (double)Drain(170), kTolNs);
}

/* what the pre-arm reads besides the drain: the NORMAL pad and the pads its last
 * burst keeps for the cut, 1262 / 64 coarse + 64 / 8 fine ones */
TEST_F(St20TxRlPlanTest, PlannedPortIsArmedWithItsNormalPad) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 2, 0, false, kTrained, false), 0);

  for (int port : {kPortP, kPortR}) {
    SCOPED_TRACE(port);
    EXPECT_TRUE(ut_txv_rl_armed(ctx_, port));
    EXPECT_EQ(ut_txv_rl_pad_size(ctx_, port), (uint16_t)kNormalPadSize);
    EXPECT_EQ(ut_txv_rl_trim_pads(ctx_, port), 1262 / 64 + 64 / 8);
  }
}

TEST_F(St20TxRlPlanTest, UserPadIntervalHolds) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 200, false, nullptr, false), 0);

  ExpectHold(kPortP);
}

TEST_F(St20TxRlPlanTest, StaticPadIntervalHolds) {
  /* g_cvl_static_pad_tables has 1080p59 gpm */
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, true, nullptr, false), 0);

  ExpectHold(kPortP);
}

TEST_F(St20TxRlPlanTest, VfCreditIsThePatchedKernelIceBucket) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  EXPECT_NEAR((double)ut_txv_rl_credit(ctx_, kPortP),
              (double)Credit(kVfBurstSize, kRlBps, Drain(164)), kTolNs);
}

/* the bucket fills at the rate programmed, not at the faster one training measures on a
 * NIC whose clock runs ahead of the psm_clk ice computes the profile with */
TEST_F(St20TxRlPlanTest, CreditFollowsTheRateProgrammedOnTheQueue) {
  constexpr uint64_t kRetrainedBps = 306359640;
  ut_txv_set_rl_queue_bps(ctx_, kPortP, kRetrainedBps);

  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  EXPECT_NEAR((double)ut_txv_rl_credit(ctx_, kPortP),
              (double)Credit(kVfBurstSize, kRetrainedBps, Drain(164)), kTolNs);
}

/* Pins the hold, not the bps guard: without it, B / 0 is an infinite bucket the ring
 * check rejects all the same. The guard keeps the division off a zero rate. */
TEST_F(St20TxRlPlanTest, PortWithoutAProgrammedRateHolds) {
  ut_txv_set_rl_queue_bps(ctx_, kPortP, 0);

  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  ExpectHold(kPortP);
}

TEST_F(St20TxRlPlanTest, PfWithDefaultBucketHolds) {
  ut_txv_set_ice_pf(ctx_, kPortP, 0);

  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  ExpectHold(kPortP);
}

/* 2160p59.94, so that the largest bucket still refills before the warm-up */
TEST_F(St20TxRlPlanTest, PfCreditIsTheBucketTheIcePmdPrograms) {
  /* ice_cfg_rl_burst_size(): nearest 64 B up to 2047 * 64 B, else nearest 1 KB */
  const uint32_t cases[][2] = {
      {4000, 4032}, {3990, 3968}, {131008, 131008}, {131040, 131072}, {150000, 149504},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c[0]);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, 3840, 2160, false, ST_FPS_P59_94);
    ut_txv_set_ice_pf(ctx, kPortP, c[0]);
    EXPECT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);
    EXPECT_NEAR((double)ut_txv_rl_credit(ctx, kPortP),
                (double)Credit(c[1], kUhdRlBps, ut_txv_rl_drain(ctx, kPortP)), kTolNs);
    ut_txv_destroy(ctx);
  }
}

/* 2160p59.94: ~332 pads of credit, more than the 512-descriptor ring should hold, in a
 * bucket that refills before the warm-up */
TEST_F(St20TxRlPlanTest, PfBucketBeyondHalfTheTxRingHolds) {
  ut_txv_set_rl_format(ctx_, 3840, 2160, false, ST_FPS_P59_94);
  ut_txv_set_ice_pf(ctx_, kPortP, 400000);

  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  ExpectHold(kPortP);
}

/* The bucket refills between the previous frame's last pkt and the warm-up open,
 * frame_time * (1 - reactive) - warm_pkts * trs, 192.8 us at 1080p59.94: a larger one
 * would meet the pads short of full. */
TEST_F(St20TxRlPlanTest, PfBucketThatCannotRefillBeforeTheWarmUpHolds) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);
  const long double frame_ns = 1e9L * 1001 / 60000;
  const long double idle_ns = frame_ns * (1 - 1080.0L / 1125) -
                              ut_txv_pacing_warm_pkts(ctx_) * ut_txv_pacing_trs(ctx_);
  /* the largest bucket ice_cfg_rl_burst_size() programs that refills in time */
  const uint32_t fits = (uint32_t)(idle_ns * kRlBps / 1e9L) / 64 * 64;

  for (uint32_t burst : {fits, fits + 64}) {
    SCOPED_TRACE(burst);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_ice_pf(ctx, kPortP, burst);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);

    EXPECT_EQ(ut_txv_rl_armed(ctx, kPortP), burst == fits);
    ut_txv_destroy(ctx);
  }
}

/* (128 trs + credit) / drain + 1 pads must fit half the 512-descriptor ring, 256: the
 * largest bucket ice_cfg_rl_burst_size() programs within it, rounded to 1 KB, at
 * 2160p59.94 */
TEST_F(St20TxRlPlanTest, PlanFitsHalfTheTxRing) {
  ut_txv_set_rl_format(ctx_, 3840, 2160, false, ST_FPS_P59_94);
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);
  const long double drain = ut_txv_rl_drain(ctx_, kPortP);
  const long double room_ns = 255 * drain - 128 * ut_txv_pacing_trs(ctx_) - Aim(drain);
  const uint32_t fits = (uint32_t)(room_ns * kUhdRlBps / 1e9L) / 1024 * 1024;
  ASSERT_GT(fits, 2047u * 64);

  for (uint32_t burst : {fits, fits + 1024}) {
    SCOPED_TRACE(burst);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, 3840, 2160, false, ST_FPS_P59_94);
    ut_txv_set_ice_pf(ctx, kPortP, burst);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);

    EXPECT_EQ(ut_txv_rl_armed(ctx, kPortP), burst == fits);
    ut_txv_destroy(ctx);
  }
}

/* tv_init_hw() builds no trim pads where a 64-byte cut would leave no frame */
TEST_F(St20TxRlPlanTest, PortWithoutTrimPadsHolds) {
  ut_txv_set_no_trim_pads(ctx_);
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);

  ExpectHold(kPortP);
}

/* only an RL port uses them, and only where the NORMAL pkt less a 64-byte cut is still
 * a frame of 64 bytes: an RTP-level session may set an rtp_pkt_size of 1 */
TEST_F(St20TxRlPlanTest, TrimPadsOnlyForRlPortsWithAFrameAfterTheCut) {
  const struct {
    enum st21_tx_pacing_way way;
    uint32_t normal, coarse, fine;
  } cases[] = {
      {ST21_TX_PACING_WAY_RL, 1262, 1198, 1254}, {ST21_TX_PACING_WAY_RL, 128, 64, 120},
      {ST21_TX_PACING_WAY_RL, 127, 0, 0},        {ST21_TX_PACING_WAY_RL, 1 + 42, 0, 0},
      {ST21_TX_PACING_WAY_TSC, 1262, 0, 0},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.normal);
    uint32_t lens[2];
    ASSERT_EQ(ut_txv_init_trim_pads(ctx_, c.way, c.normal, lens), 0);
    EXPECT_EQ(lens[0], c.coarse);
    EXPECT_EQ(lens[1], c.fine);
  }
}

/* At most 576 lines warm up 8 pkts, fewer pads than the last burst keeps to cut up to a
 * pad: the plan could not reach its aim. 119.88 fps keeps the timeslot short. */
TEST_F(St20TxRlPlanTest, WindowShortOfTheTrimPadsHolds) {
  for (uint32_t height : {480u, 576u}) {
    SCOPED_TRACE(height);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, 1920, height, false, ST_FPS_P119_88);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);

    EXPECT_EQ(ut_txv_pacing_warm_pkts(ctx), 8u);
    EXPECT_FALSE(ut_txv_rl_armed(ctx, kPortP));
    ut_txv_destroy(ctx);
  }
}

/* armed while the window holds more than trim_pads - 1 pads, so the plan's first burst
 * takes trim_pads of them. A queue rate a tenth of the trained one, far from any NIC, is
 * only a knob that moves the credit across that edge. */
TEST_F(St20TxRlPlanTest, PlanNeedsTheTrimPadsInItsWindow) {
  constexpr int64_t kTrimPads = 1262 / 64 + 64 / 8;
  ut_txv_set_rl_format(ctx_, 1920, 576, false, ST_FPS_P119_88);
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);
  const long double drain = Drain(164, 1920, 576);
  const long double edge_ns =
      (kTrimPads - 1) * drain - 8 * ut_txv_pacing_trs(ctx_) - Aim(drain);
  const uint64_t edge_bps = (uint64_t)(kVfBurstSize * 1e9L / edge_ns);

  for (uint64_t bps : {edge_bps - 1, edge_bps + 1}) {
    SCOPED_TRACE(bps);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, 1920, 576, false, ST_FPS_P119_88);
    ut_txv_set_rl_queue_bps(ctx, kPortP, bps);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);

    EXPECT_EQ(ut_txv_rl_armed(ctx, kPortP), bps < edge_bps);
    ut_txv_destroy(ctx);
  }
}

/* A plan lands packet 0 up to 2 * aim later than the hold path, so it is kept only up to
 * an aim of 2 us, 257 B of NORMAL pad drain: a pad of at most 9.8 us, a drain of at least
 * ~128.5 MB/s. 720p50 is just over (aim ~2.02 us here); SD holds on its window first. */
TEST_F(St20TxRlPlanTest, TimeslotPastTwoMicrosecondsHolds) {
  const struct {
    uint32_t width, height;
    bool interlaced;
    enum st_fps fps;
    bool armed;
  } cases[] = {
      {3840, 2160, false, ST_FPS_P59_94, true}, {1920, 1080, false, ST_FPS_P59_94, true},
      {1920, 1080, false, ST_FPS_P50, true},    {1920, 1080, false, ST_FPS_P29_97, true},
      {1920, 1080, false, ST_FPS_P25, true},    {1920, 1080, true, ST_FPS_P59_94, true},
      {1280, 720, false, ST_FPS_P59_94, true},  {1280, 720, false, ST_FPS_P50, false},
      {1280, 720, false, ST_FPS_P25, false},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(std::to_string(c.width) + "x" + std::to_string(c.height) +
                 (c.interlaced ? "i" : "p") + " fps " + std::to_string(c.fps));
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, c.width, c.height, c.interlaced, c.fps);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, kTrained, false), 0);

    EXPECT_EQ(ut_txv_rl_armed(ctx, kPortP), c.armed);
    ut_txv_destroy(ctx);
  }
}

/* armed up to a NORMAL pad drain of 2000 * 1262 / 257 = 9821.0 ns. 972 lines at 25 fps
 * have a 9876 ns trs, the pad interval sets the drain around it. */
TEST_F(St20TxRlPlanTest, TwoMicrosecondsBoundTheTimeslot) {
  constexpr uint32_t kHeight = 972;
  constexpr long double kEdgeDrain = kTimeslotMaxNs * kNormalPadSize / kQuantumBytes;
  ut_txv_set_rl_format(ctx_, 1920, kHeight, false, ST_FPS_P25);
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, false), 0);
  /* the drain as pad_interval grows */
  const long double full = Drain(1e30L, 1920, kHeight);

  for (long double drain : {kEdgeDrain - 0.05L, kEdgeDrain + 0.05L}) {
    SCOPED_TRACE((double)drain);
    const float pad_interval[1] = {(float)(drain / (full - drain))};
    ASSERT_GT(pad_interval[0], 32);
    ut_txv_ctx* ctx = ut_txv_create();
    ASSERT_NE(ctx, nullptr);
    ut_txv_set_rl_format(ctx, 1920, kHeight, false, ST_FPS_P25);

    ASSERT_EQ(ut_txv_init_rl_pacing(ctx, 1, 0, false, pad_interval, false), 0);

    EXPECT_EQ(ut_txv_rl_armed(ctx, kPortP), drain < kEdgeDrain);
    ut_txv_destroy(ctx);
  }
}

TEST_F(St20TxRlPlanTest, WidePacingHolds) {
  ASSERT_EQ(ut_txv_init_rl_pacing(ctx_, 1, 0, false, kTrained, true), 0);

  ASSERT_EQ(ut_txv_pacing_warm_pkts(ctx_), 0u);
  ExpectHold(kPortP);
}
