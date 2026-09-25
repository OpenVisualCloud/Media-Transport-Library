/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * rtp_ts_delta gate of the ST 2110-21 RX timing parser: a port's first frame
 * has no previous frame to measure against, so the gate must not fail it, while
 * a later frame with a short delta still fails it.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTpRtpTsDeltaTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
/* Must match the harness session geometry. */
constexpr uint64_t kFramePeriodNs = 33333333;
constexpr uint32_t kTicksPerFrame = 3003;
constexpr uint64_t kFirstPktOffsetNs = 1000;
constexpr uint64_t kEpoch = 1000;
}  // namespace

class St20RxTpRtpTsDeltaTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/false), 0);
  }

  void FeedFrameInEpoch(uint64_t epoch, uint32_t rtp_timestamp) {
    const uint64_t arrival_ns = epoch * kFramePeriodNs + kFirstPktOffsetNs;
    for (int i = 0; i < pkts_per_frame(); i++) {
      ut20_feed_frame_pkt_hw_ts(ctx_, i, rtp_timestamp, MTL_SESSION_PORT_P, arrival_ns);
    }
  }
};

TEST_F(St20RxTpRtpTsDeltaTest, FirstFrameIsCompliant) {
  FeedFrameInEpoch(kEpoch, (uint32_t)(kEpoch * kTicksPerFrame));
  ASSERT_EQ(frames_received(), 1);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_NARROW)
      << "cause: " << ut20_last_tp_failed_cause(ctx_);
}

TEST_F(St20RxTpRtpTsDeltaTest, SecondFrameShortDeltaFails) {
  const uint32_t first_rtp = (uint32_t)(kEpoch * kTicksPerFrame);
  FeedFrameInEpoch(kEpoch, first_rtp);
  FeedFrameInEpoch(kEpoch + 1, first_rtp + kTicksPerFrame - 1);
  ASSERT_EQ(frames_received(), 2);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut20_last_tp_failed_cause(ctx_), "rtp_ts_delta exceed min");
}
