/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Cross-essence proof that ST20 (video), ST30 (audio), and ST40 (ancillary) TX
 * sessions resolve the same USER_TIMESTAMP input to the same real TAI instant --
 * the actual sync guarantee USER_TIMESTAMP exists to deliver. st20_tx/pacing_test.cpp,
 * st30_tx/pacing_test.cpp, and st40_tx/pacing_test.cpp each already pin one essence's
 * internal timestamp/rtp_timestamp consistency in isolation; nothing until now proved
 * the three essences agree with each other.
 *
 * Each session's own natural pacing cursor (mock PTP time) is deliberately offset 5ms
 * from the app's intended instant, so a session that reported its pacing cursor
 * instead of the instant its rtp_timestamp actually derived from would disagree by
 * that 5ms -- not coincidentally match by using the same value for both.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest
 * --gtest_filter='MultiEssenceUserTimestampSyncTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"
#include "session/st30_tx_harness.h"
#include "session/st40_tx_harness.h"

namespace {
constexpr uint64_t kNsPerMs = 1000 * 1000;
constexpr uint64_t kRealisticTaiMs = 1755000000000ULL; /* ~2025, in ms */
constexpr uint64_t kRealisticTai = kRealisticTaiMs * kNsPerMs;
constexpr uint64_t kNaturalOffsetMs = 5;
constexpr uint64_t kNaturalTaiMs = kRealisticTaiMs + kNaturalOffsetMs;
constexpr uint64_t kNaturalTai = kNaturalTaiMs * kNsPerMs;
constexpr uint32_t kVideoSamplingRate = ST10_VIDEO_SAMPLING_RATE_90K;
constexpr uint32_t kAncillarySamplingRate = ST10_VIDEO_SAMPLING_RATE_90K;
constexpr uint32_t kAudioSamplingRate = 48000; /* ST30_SAMPLING_48K */
}  // namespace

class MultiEssenceUserTimestampSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ASSERT_EQ(ut_txa30_init(), 0);
    ASSERT_EQ(ut_txa_init(), 0);
    video_ = ut_txv_create();
    audio_ = ut_txa30_create();
    ancillary_ = ut_txa_create();
    ASSERT_NE(video_, nullptr);
    ASSERT_NE(audio_, nullptr);
    ASSERT_NE(ancillary_, nullptr);

    /* All three default to a 1ms frame/packet period, so the same epoch and
     * mock-time offsets in milliseconds apply uniformly across essences. */
    ut_txv_set_user_timestamp(video_, true);
    ut_txv_set_cur_epochs(video_, kNaturalTaiMs - 1);
    ut_txv_set_mock_ptp_time(video_, kNaturalTai);
    ut_txv_set_mock_tsc_time(video_, 0);

    ut_txa30_set_user_timestamp(audio_, true);
    ut_txa30_set_mock_ptp_time(audio_, kNaturalTai);
    ut_txa30_set_mock_tsc_time(audio_, 0);

    ut_txa_set_user_timestamp(ancillary_, true);
    ut_txa_set_cur_epochs(ancillary_, kNaturalTaiMs - 1);
    ut_txa_set_mock_ptp_time(ancillary_, kNaturalTai);
    ut_txa_set_mock_tsc_time(ancillary_, 0);
  }

  void TearDown() override {
    ut_txv_destroy(video_);
    ut_txa30_destroy(audio_);
    ut_txa_destroy(ancillary_);
  }

  ut_txv_ctx* video_ = nullptr;
  ut_txa30_ctx* audio_ = nullptr;
  ut_txa_ctx* ancillary_ = nullptr;
};

TEST_F(MultiEssenceUserTimestampSyncTest, TaiInputResolvesToSameInstantAcrossEssences) {
  uint64_t video_tsc = 0, video_ptp = 0, ancillary_tsc = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(video_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai,
                                     &video_tsc, &video_ptp),
            0);
  ASSERT_EQ(ut_txa30_run_frame_tasklet(audio_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai, 1),
            0);
  ASSERT_EQ(ut_txa_run_frame_tasklet(ancillary_, ST10_TIMESTAMP_FMT_TAI, kRealisticTai,
                                     &ancillary_tsc),
            0);

  const uint64_t video_timestamp = ut_txv_notify_frame_done_timestamp(video_);
  const uint64_t audio_timestamp = ut_txa30_notify_frame_done_timestamp(audio_);
  const uint64_t ancillary_timestamp = ut_txa_notify_frame_done_timestamp(ancillary_);

  /* The cross-essence invariant: the same app-supplied TAI instant, resolved
   * independently by three unrelated session types, must be the exact same real-world
   * instant, not merely close -- USER_TIMESTAMP with ST10_TIMESTAMP_FMT_TAI is assigned
   * verbatim, with no format-dependent rounding, so zero tolerance is the only value
   * that actually tests the contract. */
  EXPECT_EQ(video_timestamp, kRealisticTai);
  EXPECT_EQ(audio_timestamp, kRealisticTai);
  EXPECT_EQ(ancillary_timestamp, kRealisticTai);
  EXPECT_EQ(video_timestamp, audio_timestamp);
  EXPECT_EQ(video_timestamp, ancillary_timestamp);

  /* Sanity check that this cross-essence test is wired to the same per-essence
   * contract st20_tx/st30_tx/st40_tx pacing_test.cpp already pin individually. */
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(video_),
            st10_tai_to_media_clk(video_timestamp, kVideoSamplingRate));
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(audio_),
            st10_tai_to_media_clk(audio_timestamp, kAudioSamplingRate));
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ancillary_),
            st10_tai_to_media_clk(ancillary_timestamp, kAncillarySamplingRate));
}

TEST_F(MultiEssenceUserTimestampSyncTest,
       MediaClkInputAtEachEssenceRateResolvesToSameInstantAcrossEssences) {
  const uint32_t video_ticks = st10_tai_to_media_clk(kRealisticTai, kVideoSamplingRate);
  const uint32_t audio_ticks = st10_tai_to_media_clk(kRealisticTai, kAudioSamplingRate);
  const uint32_t ancillary_ticks =
      st10_tai_to_media_clk(kRealisticTai, kAncillarySamplingRate);
  uint64_t video_tsc = 0, video_ptp = 0, ancillary_tsc = 0;

  ASSERT_EQ(ut_txv_run_frame_tasklet(video_, ST10_TIMESTAMP_FMT_MEDIA_CLK, video_ticks,
                                     &video_tsc, &video_ptp),
            0);
  ASSERT_EQ(
      ut_txa30_run_frame_tasklet(audio_, ST10_TIMESTAMP_FMT_MEDIA_CLK, audio_ticks, 1),
      0);
  ASSERT_EQ(ut_txa_run_frame_tasklet(ancillary_, ST10_TIMESTAMP_FMT_MEDIA_CLK,
                                     ancillary_ticks, &ancillary_tsc),
            0);

  const uint64_t video_timestamp = ut_txv_notify_frame_done_timestamp(video_);
  const uint64_t audio_timestamp = ut_txa30_notify_frame_done_timestamp(audio_);
  const uint64_t ancillary_timestamp = ut_txa_notify_frame_done_timestamp(ancillary_);

  /* Same instant, now supplied per essence in that session's own MEDIA_CLK sampling
   * rate (audio at 48kHz, video/ancillary at 90kHz) instead of TAI -- the unwrap
   * against each session's own pacing cursor must still land on the exact same TAI
   * instant, not one skewed by whichever sampling rate happened to be in play. */
  EXPECT_EQ(video_timestamp, kRealisticTai);
  EXPECT_EQ(audio_timestamp, kRealisticTai);
  EXPECT_EQ(ancillary_timestamp, kRealisticTai);
  EXPECT_EQ(video_timestamp, audio_timestamp);
  EXPECT_EQ(video_timestamp, ancillary_timestamp);

  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(video_), video_ticks);
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(audio_), audio_ticks);
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ancillary_), ancillary_ticks);
  EXPECT_EQ(ut_txv_notify_frame_done_rtp_timestamp(video_),
            st10_tai_to_media_clk(video_timestamp, kVideoSamplingRate));
  EXPECT_EQ(ut_txa30_notify_frame_done_rtp_timestamp(audio_),
            st10_tai_to_media_clk(audio_timestamp, kAudioSamplingRate));
  EXPECT_EQ(ut_txa_notify_frame_done_rtp_timestamp(ancillary_),
            st10_tai_to_media_clk(ancillary_timestamp, kAncillarySamplingRate));
}
