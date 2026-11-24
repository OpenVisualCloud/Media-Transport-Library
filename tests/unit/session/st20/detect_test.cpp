/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-20 RX format detection (rv_attach + rv_handle_detect_pkt). The timing
 * parser needs the sender's packets per frame, which the detector takes from the
 * RTP sequence numbers of three markers, so a pkt lost in the frames it samples
 * must neither end detection nor change the count, and a sample without a count
 * must not end detection either. An auto-detect session reads no count, so it
 * must not wait for one.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20Rx*DetectTest.*'
 */

#include <gtest/gtest.h>

#include <cstring>

#include "session/st20/st20_rx_test_base.h"

class St20RxDetectTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  virtual uint32_t flags() const {
    return ST20_RX_FLAG_TIMING_PARSER_META;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ASSERT_EQ(ut20_ctx_attach_st20(ctx_, flags()), 0);
  }

  /* 59.94 fps: the rtp ts steps 1501, 1502, ... */
  void feed_frame(int lost_line = -1) {
    ASSERT_EQ(ut20_feed_attached_frame(ctx_, static_cast<uint32_t>(frame_ * 3003 / 2),
                                       lost_line),
              0);
    frame_++;
  }

  /* the third frame the detector counts loses a line; 8 clean frames cover a retry */
  void feed_lossy_detection() {
    feed_frame();
    feed_frame();
    feed_frame(100);
    for (int i = 0; i < 8; i++) feed_frame();
  }

  int lines() {
    return ut20_pkts_per_frame(ctx_);
  }

  int frame_ = 0;
};

class St20RxAutoDetectTest : public St20RxDetectTest {
 protected:
  uint32_t flags() const override {
    return ST20_RX_FLAG_AUTO_DETECT;
  }
};

TEST_F(St20RxDetectTest, CleanStreamDeliversFromTheFifthFrame) {
  ut20_skip_wire_seq(ctx_, 0x10000 - 2 * lines()); /* the sample spans the 16-bit wrap */
  for (int i = 0; i < 6; i++) feed_frame();

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(ut20_detect_pkt_per_frame(ctx_), lines());
}

TEST_F(St20RxDetectTest, LostPktDuringDetectionKeepsDeliveringFrames) {
  feed_lossy_detection();
  int before = frames_received();

  feed_frame();
  feed_frame();

  EXPECT_EQ(frames_received(), before + 2);
  EXPECT_NE(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_MAX);
}

TEST_F(St20RxDetectTest, EqualLossInTheSampledFramesKeepsTheSentCount) {
  feed_frame();
  feed_frame(100);
  feed_frame(200);
  for (int i = 0; i < 8; i++) feed_frame();

  EXPECT_EQ(ut20_detect_pkt_per_frame(ctx_), lines());
}

TEST_F(St20RxDetectTest, SeqJumpDuringDetectionKeepsDeliveringFrames) {
  feed_frame();
  ut20_skip_wire_seq(ctx_, 2000);
  feed_frame();
  ut20_skip_wire_seq(ctx_, 1000);
  for (int i = 0; i < 9; i++) feed_frame();
  int before = frames_received();

  feed_frame();
  feed_frame();

  EXPECT_EQ(frames_received(), before + 2);
  EXPECT_EQ(ut20_detect_pkt_per_frame(ctx_), lines());
}

static int g_count_mismatch_logs;

static void ut20_count_mismatch_printer(enum mtl_log_level, const char* format, ...) {
  if (strstr(format, "err total0")) g_count_mismatch_logs++;
}

/* Counts that never agree keep a session detecting; each session says so once. */
TEST_F(St20RxDetectTest, CountsThatNeverAgreeAreLoggedOncePerSession) {
  ut20_test_ctx* other = ut20_ctx_create_geom(num_port(), pkts_per_frame());
  ASSERT_NE(other, nullptr);
  ASSERT_EQ(ut20_ctx_attach_st20(other, flags()), 0);

  g_count_mismatch_logs = 0;
  mtl_set_log_printer(ut20_count_mismatch_printer);
  for (uint32_t f = 0; f < 12; f++) {
    for (ut20_test_ctx* ctx : {ctx_, other}) {
      ut20_skip_wire_seq(ctx, f);
      ut20_feed_attached_frame(ctx, f * 3003 / 2, -1);
    }
  }
  mtl_set_log_printer(nullptr);
  EXPECT_EQ(ut20_frames_received(other), 0);
  ut20_ctx_destroy(other);

  EXPECT_EQ(g_count_mismatch_logs, 2);
  EXPECT_EQ(frames_received(), 0);
}

TEST_F(St20RxAutoDetectTest, UnequalCountsDetectOnTheFourthMarker) {
  feed_frame();
  feed_frame();
  ut20_skip_wire_seq(ctx_, 1000);
  feed_frame();
  feed_frame();
  EXPECT_EQ(ut20_detected_cnt(ctx_), 1);

  feed_frame();
  feed_frame();
  EXPECT_EQ(frames_received(), 2);
}
