/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * RX burst gate of the ST 2110-21 RX timing parser: packets of a large RX burst
 * share one SW time reading and are skipped, but a valid HW RX timestamp is per
 * packet and must be measured.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTpBurstTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
/* Must match the harness session geometry. */
constexpr uint64_t kFramePeriodNs = 33333333;
constexpr uint32_t kTicksPerFrame = 3003;
constexpr uint64_t kEpoch = 1000;
constexpr uint64_t kArrivalNs = kEpoch * kFramePeriodNs + 1000;
constexpr uint32_t kRtpTimestamp = kEpoch * kTicksPerFrame;
constexpr uint16_t kLargeBurst = 64;
}  // namespace

class St20RxTpBurstTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/false), 0);
    ut20_ctx_set_rx_burst(ctx_, kLargeBurst, /*continuous=*/true);
  }
};

TEST_F(St20RxTpBurstTest, HwTimestampedBurstPacketsAreMeasured) {
  for (int i = 0; i < pkts_per_frame(); i++) {
    ut20_feed_frame_pkt_hw_ts(ctx_, i, kRtpTimestamp, MTL_SESSION_PORT_P, kArrivalNs);
  }
  ASSERT_EQ(frames_received(), 1);

  EXPECT_EQ(ut20_last_tp_pkts_cnt(ctx_), (uint32_t)pkts_per_frame());
  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_NARROW)
      << "cause: " << ut20_last_tp_failed_cause(ctx_);
}

TEST_F(St20RxTpBurstTest, SwTimedBurstPacketsAreUntrusted) {
  for (int i = 0; i < pkts_per_frame(); i++) {
    ut20_feed_frame_pkt_stale_hw_ts(ctx_, i, kRtpTimestamp, MTL_SESSION_PORT_P,
                                    kArrivalNs);
  }
  ASSERT_EQ(frames_received(), 1);

  EXPECT_EQ(ut20_last_tp_pkts_cnt(ctx_), 0u);
  /* A frame with nothing measured must not pass; its cause string is arbitrary. */
  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED);
}
