/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-20 RX format detection: when the packet counts of the sampled frames
 * disagree, detection must resample from a clean state instead of failing or
 * reporting a count of 0. Loss identical in both sampled frames still agrees
 * on a wrong count; that is not covered here.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxFormatDetectTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
/* 640x480p29.97 YUV422 10-bit, one 1600-byte row per packet. */
constexpr uint16_t kRows = 480;
constexpr uint16_t kRowBytes = 1600;
constexpr uint32_t kTicksPerFrame = 3003;
constexpr int kGoodFrames = 10;
constexpr uint8_t kPt = 112;
constexpr uint32_t kSsrc = 0x5a5a;
}  // namespace

class St20RxFormatDetectTest : public St20RxBaseTest {
 protected:
  uint32_t seq_ = 0;
  uint32_t ts_ = 0;

  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_set_pt(ctx_, kPt);
    ut20_ctx_set_ssrc(ctx_, kSsrc);
  }

  void FeedPkt(uint16_t row, bool marker) {
    ut20_feed_pkt_via_wrapper(ctx_, seq_++, ts_, row, 0, kRowBytes, MTL_SESSION_PORT_P,
                              kPt, kSsrc, marker);
  }

  /* Rows in [lost_begin, lost_end) never arrive but still consume a sequence number. */
  void FeedFrame(uint16_t lost_begin = 0, uint16_t lost_end = 0) {
    for (uint16_t row = 0; row < kRows; row++) {
      if (row >= lost_begin && row < lost_end) {
        seq_++;
        continue;
      }
      FeedPkt(row, row == kRows - 1);
    }
    ts_ += kTicksPerFrame;
  }
};

TEST_F(St20RxFormatDetectTest, MidFrameStartDetectsPacketsPerFrame) {
  ut20_ctx_enable_format_detect(ctx_, /*auto_detect=*/false, /*timing_parser=*/true);
  FeedFrame(0, 300);
  for (int i = 0; i < kGoodFrames; i++) FeedFrame();

  EXPECT_TRUE(ut20_format_detected(ctx_));
  EXPECT_EQ(ut20_detected_pkts_per_frame(ctx_), kRows);
  EXPECT_GT(frames_received(), 0);
}

TEST_F(St20RxFormatDetectTest, StartupLossInSampledFrameRetriesDetection) {
  ut20_ctx_enable_format_detect(ctx_, /*auto_detect=*/false, /*timing_parser=*/true);
  FeedFrame(0, 300);
  FeedFrame(100, 200);
  for (int i = 0; i < kGoodFrames; i++) FeedFrame();

  EXPECT_TRUE(ut20_format_detected(ctx_));
  EXPECT_EQ(ut20_detected_pkts_per_frame(ctx_), kRows);
  EXPECT_GT(frames_received(), 0);
}

TEST_F(St20RxFormatDetectTest, AutoDetectAfterStartupLossReportsCleanRoundFormat) {
  ut20_ctx_enable_format_detect(ctx_, /*auto_detect=*/true, /*timing_parser=*/false);
  FeedFrame(0, 300);
  /* A corrupt packet flags a second field on this progressive stream. */
  FeedPkt(static_cast<uint16_t>(ST20_SECOND_FIELD), false);
  FeedFrame(100, 200);
  for (int i = 0; i < kGoodFrames; i++) FeedFrame();

  ASSERT_EQ(ut20_detect_notify_cnt(ctx_), 1);
  const struct st20_detect_meta* meta = ut20_detect_notified_meta(ctx_);
  EXPECT_EQ(meta->width, 640);
  EXPECT_EQ(meta->height, kRows);
  EXPECT_EQ(meta->fps, ST_FPS_P29_97);
  EXPECT_EQ(meta->packing, ST20_PACKING_GPM_SL);
  EXPECT_FALSE(meta->interlaced);
  EXPECT_GT(frames_received(), 0);
}

TEST_F(St20RxFormatDetectTest, RepeatedMismatchKeepsRetrying) {
  ut20_ctx_enable_format_detect(ctx_, /*auto_detect=*/false, /*timing_parser=*/true);
  /* A different loss in every frame: no two sampled frames ever agree. */
  for (uint16_t lost = 1; lost <= 12; lost++) FeedFrame(0, lost);
  EXPECT_FALSE(ut20_format_detected(ctx_));

  for (int i = 0; i < kGoodFrames; i++) FeedFrame();

  EXPECT_TRUE(ut20_format_detected(ctx_));
  EXPECT_EQ(ut20_detected_pkts_per_frame(ctx_), kRows);
  EXPECT_GT(frames_received(), 0);
}
