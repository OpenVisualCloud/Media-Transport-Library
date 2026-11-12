/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Latency gate of the ST 2110-21 RX timing parser at 59.94 fps, where a frame is
 * 1501.5 ticks of the 90 kHz clock, so every other epoch starts half a tick into a
 * tick: a frame whose RTP time precedes its first packet must pass on either kind
 * of epoch, and one whose first packet precedes its RTP time must fail.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTpLatencyTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
constexpr double kFrameNs = 1e9 * 1001 / 60000; /* 59.94 fps */
constexpr double kTickNs = 1e9 / 90000;
constexpr double kTicksPerFrame = 1501.5;
constexpr uint64_t kEvenEpoch = 100000;        /* starts on a tick */
constexpr uint64_t kOddEpoch = kEvenEpoch + 1; /* starts half a tick into one */
}  // namespace

class St20RxTpLatencyTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
    ASSERT_EQ(ut20_ctx_set_fps(ctx_, ST_FPS_P59_94), 0);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/false), 0);
  }

  /* The frame's RTP timestamp is the tick after the one its epoch starts in, and
   * its first packet arrives first_pkt_ns after the epoch. */
  void FeedFrame(uint64_t epoch, double first_pkt_ns) {
    double epoch_ticks = (double)epoch * kTicksPerFrame;
    uint32_t rtp = (uint32_t)((uint64_t)epoch_ticks + 1);
    uint64_t arrival_ns = (uint64_t)((double)epoch * kFrameNs + first_pkt_ns);
    for (int i = 0; i < pkts_per_frame(); i++)
      ut20_feed_frame_pkt_hw_ts(ctx_, i, rtp, MTL_SESSION_PORT_P, arrival_ns);
  }
};

/* RTP time = epoch + 5.6 us, first packet at epoch + 8 us: 2.4 us of latency. */
TEST_F(St20RxTpLatencyTest, RtpTimeBeforeFirstPacketPassesOnAHalfTickEpoch) {
  FeedFrame(kOddEpoch, 8000);
  ASSERT_EQ(frames_received(), 1);

  EXPECT_NE(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED)
      << "cause: " << ut20_last_tp_failed_cause(ctx_);
}

/* RTP time = epoch + 11.1 us, first packet at epoch + 8 us: 3.1 us early. */
TEST_F(St20RxTpLatencyTest, FirstPacketBeforeRtpTimeFailsOnAWholeTickEpoch) {
  FeedFrame(kEvenEpoch, 8000);
  ASSERT_EQ(frames_received(), 1);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut20_last_tp_failed_cause(ctx_), "latency exceed min");
}

/* The same frame half a tick into its epoch: RTP time = epoch + 5.6 us, first packet
 * at epoch + 2 us. */
TEST_F(St20RxTpLatencyTest, FirstPacketBeforeRtpTimeFailsOnAHalfTickEpoch) {
  FeedFrame(kOddEpoch, 2000);
  ASSERT_EQ(frames_received(), 1);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut20_last_tp_failed_cause(ctx_), "latency exceed min");
}
