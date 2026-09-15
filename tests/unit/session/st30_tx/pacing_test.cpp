/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins `tx_audio_pacing_update_rtp_time_stamp()`'s contract in
 * st_tx_audio_session.c: the TAI instant it returns (reported to the app as
 * frame->ta_meta.timestamp) must always reconstruct frame->ta_meta.rtp_timestamp
 * via st10_tai_to_media_clk(), across ST30_TX_FLAG_USER_TIMESTAMP,
 * ST30_TX_FLAG_USER_PACING, rtp_timestamp_delta_us, and the
 * ST10_TIMESTAMP_FMT_MEDIA_CLK input format -- mirroring the st20 (video) fix
 * in tv_update_rtp_time_stamp() / st_tx_video_session.c.
 *
 * All PTP anchors below use a large, real-epoch-scale TAI value (~2025), not a
 * small round number near zero: a bug that only shows up far from the 32-bit
 * media-clock wrap origin, or that only cancels out in a self-referential
 * round-trip check, would otherwise hide behind a passing test.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxPacingTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30_tx_harness.h"

namespace {
constexpr uint64_t kNsPerMs = 1000 * 1000;
constexpr uint32_t kSamplingRate = 48000; /* ST30_SAMPLING_48K */
constexpr uint32_t kSamplesPerPkt = 48;   /* 48kHz @ 1ms ptime */
constexpr uint64_t kPktTimeNs = kNsPerMs;
constexpr uint64_t kRealisticTaiMs = 1755000000000ULL; /* ~2025, in ms */
constexpr uint64_t kRealisticTai = kRealisticTaiMs * kNsPerMs;
}  // namespace

class St30TxPacingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txa30_destroy(ctx_);
  }
  ut_txa30_ctx* ctx_ = nullptr;
};

/* Bug #1 (MEDIA_CLK + USER_TIMESTAMP): the raw 32-bit tick count must be
 * unwrapped against the session's actual pacing cursor, not assigned to
 * pacing->rtp_time_stamp verbatim -- and frame->ta_meta.timestamp must be the
 * TAI instant that unwrap actually produced, not tx_audio_session_sync_pacing()'s
 * unrelated natural-cadence cursor.
 *
 * kRealisticTaiMs is millisecond-aligned, so natural (non-user) pacing lands
 * pacing->ptp_time_cursor exactly on kRealisticTai for packet 0. kMediaClockTimestamp
 * is anchor_ticks (kRealisticTai's own tick count) + one packet's worth of ticks,
 * i.e. 1ms ahead of that anchor.
 *
 * Old (buggy) code: frame->ta_meta.timestamp = pacing->ptp_time_cursor = kRealisticTai
 * (the natural-cadence cursor, unrelated to the app's timestamp);
 * frame->ta_meta.rtp_timestamp = (uint32_t)kMediaClockTimestamp verbatim =
 * anchor_ticks + 48. media_clk(kRealisticTai) == anchor_ticks != anchor_ticks + 48:
 * the two fields disagree by exactly one packet time.
 * Fixed code: frame->ta_meta.timestamp = kRealisticTai + kPktTimeNs, which
 * reconstructs anchor_ticks + 48 exactly. */
TEST_F(St30TxPacingTest, FrameTaskletMediaClkUserTimestampReportsConsistentTai) {
  const uint32_t anchor_ticks = st10_tai_to_media_clk(kRealisticTai, kSamplingRate);
  const uint32_t media_clock_timestamp = anchor_ticks + kSamplesPerPkt;
  ut_txa30_set_user_timestamp(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       media_clock_timestamp, 1),
            0);

  const uint64_t expected_timestamp = kRealisticTai + kPktTimeNs;
  EXPECT_EQ(ut_txa30_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_), media_clock_timestamp);
  EXPECT_EQ(
      ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
      st10_tai_to_media_clk(ut_txa30_notify_frame_done_timestamp(ctx_), kSamplingRate));
  ASSERT_EQ(ut_txa30_wire_rtp_timestamp_count(ctx_), 1);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 0), media_clock_timestamp);
  EXPECT_NEAR((double)ut_txa30_notify_frame_done_timestamp(ctx_), (double)kRealisticTai,
              2.0 * (double)kPktTimeNs);
}

/* Same unwrap, app timestamp slightly *behind* the anchor tick rather than
 * ahead, so st10_media_clk_to_tai()'s negative-cycle path runs. A wrong wrap
 * direction would land roughly half a 32-bit tick cycle away (many hours),
 * not one packet time behind kRealisticTai. */
TEST_F(St30TxPacingTest, FrameTaskletMediaClkUserTimestampUnwrapsBehindAnchor) {
  const uint32_t anchor_ticks = st10_tai_to_media_clk(kRealisticTai, kSamplingRate);
  const uint32_t media_clock_timestamp = anchor_ticks - kSamplesPerPkt;
  ut_txa30_set_user_timestamp(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                       media_clock_timestamp, 1),
            0);

  const uint64_t expected_timestamp = kRealisticTai - kPktTimeNs;
  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_), media_clock_timestamp);
  EXPECT_EQ(
      ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
      st10_tai_to_media_clk(ut_txa30_notify_frame_done_timestamp(ctx_), kSamplingRate));
}

/* Bug #2 (TAI + USER_PACING off-by-one): tx_audio_session_sync_pacing() advances
 * pacing->ptp_time_cursor to required_tai + trs to seed the next packet's
 * schedule ("prepare next packet"), so the required_tai actually used to derive
 * frame->ta_meta.rtp_timestamp is one packet time behind the post-sync cursor.
 *
 * Old (buggy) code: frame->ta_meta.timestamp = pacing->ptp_time_cursor =
 * kRealisticTai + kPktTimeNs, one packet ahead of what rtp_timestamp reflects.
 * Fixed code: frame->ta_meta.timestamp = kRealisticTai, matching rtp_timestamp
 * exactly. */
TEST_F(St30TxPacingTest, FrameTaskletTaiUserPacingReportsPreAdvanceInstant) {
  ut_txa30_set_user_pacing(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai, 1),
            0);

  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), kRealisticTai);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(kRealisticTai, kSamplingRate));
  EXPECT_EQ(
      ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
      st10_tai_to_media_clk(ut_txa30_notify_frame_done_timestamp(ctx_), kSamplingRate));
  ASSERT_EQ(ut_txa30_wire_rtp_timestamp_count(ctx_), 1);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 0),
            st10_tai_to_media_clk(kRealisticTai, kSamplingRate));
  EXPECT_EQ(ut_txa30_stat_error_user_timestamp(ctx_), 0u);
}

/* Regression guard for the fix's seed-forwarding: consecutive packets within
 * one USER_PACING frame must still advance by exactly one packet time each
 * (via pacing->ptp_time_cursor), even though frame->ta_meta.timestamp itself
 * no longer carries that pre-advanced value forward. */
TEST_F(St30TxPacingTest, FrameTaskletTaiUserPacingChainsConsecutivePacketsWithoutDrift) {
  const uint32_t anchor_ticks = st10_tai_to_media_clk(kRealisticTai, kSamplingRate);
  ut_txa30_set_user_pacing(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai, 3),
            0);

  EXPECT_EQ(ut_txa30_notify_frame_done_calls(ctx_), 1);
  ASSERT_EQ(ut_txa30_wire_rtp_timestamp_count(ctx_), 3);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 0), anchor_ticks);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 1), anchor_ticks + kSamplesPerPkt);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 2), anchor_ticks + 2 * kSamplesPerPkt);
  const uint64_t expected_last_timestamp = kRealisticTai + 2 * kPktTimeNs;
  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), expected_last_timestamp);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
            anchor_ticks + 2 * kSamplesPerPkt);
}

/* USER_TIMESTAMP with TAI format must honor the app's raw value for RTP
 * reporting purposes even when USER_PACING is not set -- mirroring st20's
 * tv_update_rtp_time_stamp(), whose USER_TIMESTAMP branch never depended on
 * pacing having followed it (see st20_tx/pacing_test.cpp,
 * FrameTaskletUserTimestampMediaClkReportsRtpConsistentTai). The actual send
 * time still follows default (natural-cadence) pacing -- USER_PACING, not
 * USER_TIMESTAMP, controls that; this is deliberately out of scope here. */
TEST_F(St30TxPacingTest, FrameTaskletUserTimestampTaiHonorsRawValueWithoutUserPacing) {
  const uint64_t natural_tai = kRealisticTai + 5 * kPktTimeNs;
  ut_txa30_set_user_timestamp(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, natural_tai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai, 1),
            0);

  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), kRealisticTai);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(kRealisticTai, kSamplingRate));
}

/* Multi-packet counterpart of the test above: USER_TIMESTAMP honors the app's
 * raw value for packet 0 only. tx_audio_pacing_required_tai() (st_tx_audio_session.c)
 * returns 0 unconditionally without ST30_TX_FLAG_USER_PACING, regardless of
 * what the pkt_idx!=0 seed override passes it, so packet 1+ falls into
 * tx_audio_session_sync_pacing()'s natural-cadence branch instead of
 * continuing to advance from the app's timestamp by one packet time each.
 *
 * mock_ptp_ns is deliberately set 10 packet times ahead of the app's supplied
 * timestamp so "natural cadence" and "chained by one packet time from the app
 * value" land on clearly different instants -- proving this is a real jump to
 * an unrelated wall-clock instant, not a coincidental match. */
TEST_F(St30TxPacingTest,
       FrameTaskletUserTimestampTaiWithoutUserPacingFallsBackAfterFirstPacket) {
  const uint64_t natural_tai = kRealisticTai + 10 * kPktTimeNs;
  ut_txa30_set_user_timestamp(ctx_, true);
  ut_txa30_set_mock_ptp_time(ctx_, natural_tai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai, 3),
            0);

  ASSERT_EQ(ut_txa30_wire_rtp_timestamp_count(ctx_), 3);
  /* Packet 0: honors the app's raw timestamp, as above. */
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 0),
            st10_tai_to_media_clk(kRealisticTai, kSamplingRate));
  /* Packet 1: NOT kRealisticTai + kPktTimeNs (what continuing to chain from
   * the app's value would give) -- it jumps to the natural-cadence instant
   * tx_audio_session_sync_pacing() lands on once required_tai is 0. */
  const uint32_t chained_from_app_value =
      st10_tai_to_media_clk(kRealisticTai + kPktTimeNs, kSamplingRate);
  const uint32_t natural_cadence_pkt1 = st10_tai_to_media_clk(natural_tai, kSamplingRate);
  EXPECT_NE(ut_txa30_wire_rtp_timestamp(ctx_, 1), chained_from_app_value);
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 1), natural_cadence_pkt1);
  /* Packet 2: having fallen back, it now continues the natural cadence by one
   * packet time -- not by continuing to chain from the app's original value. */
  EXPECT_EQ(ut_txa30_wire_rtp_timestamp(ctx_, 2),
            st10_tai_to_media_clk(natural_tai + kPktTimeNs, kSamplingRate));
  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), natural_tai + kPktTimeNs);
}

/* Latent bug, present even with no USER_* flags at all: rtp_timestamp_delta_us
 * was only ever folded into pacing->rtp_time_stamp (in the raw-tick domain),
 * never into frame->ta_meta.timestamp, so the two fields disagreed by the
 * delta whenever it was nonzero. Fixed code derives both from the same
 * TAI-domain tai_for_rtp_ts. */
TEST_F(St30TxPacingTest, FrameTaskletDefaultPacingAppliesDeltaToBothFields) {
  constexpr int32_t kDeltaUs = 500;
  ut_txa30_set_rtp_timestamp_delta_us(ctx_, kDeltaUs);
  ut_txa30_set_mock_ptp_time(ctx_, kRealisticTai);
  ut_txa30_set_mock_tsc_time(ctx_, 0);

  ASSERT_EQ(ut_txa30_run_frame_tasklet(ctx_, ST10_TIMESTAMP_FMT_TAI, 0, 1), 0);

  const uint64_t expected_timestamp = kRealisticTai + (uint64_t)kDeltaUs * 1000;
  EXPECT_EQ(ut_txa30_notify_frame_done_timestamp(ctx_), expected_timestamp);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
            st10_tai_to_media_clk(expected_timestamp, kSamplingRate));
  EXPECT_EQ(
      ut_txa30_notify_frame_done_rtp_timestamp(ctx_),
      st10_tai_to_media_clk(ut_txa30_notify_frame_done_timestamp(ctx_), kSamplingRate));
}
