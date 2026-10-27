/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX CMAX / VRX result of the st20 RX timing parser, fed through the RX session
 * with HW RX timestamps. 32 packets a 29.97 frame: CMAX 16, VRXFULL 32.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxRxTp*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "ipmx/ipmx_api.h"
#include "session/st20/st20_rx_test_base.h"

namespace {

constexpr uint32_t kTicksPerFrame = 3003;
constexpr uint64_t kEpoch = 1000;
constexpr uint64_t kFrameNs = 33366667;
constexpr uint64_t kArrivalNs = kEpoch * kFrameNs + 1000;
constexpr uint32_t kRtpTimestamp = kEpoch * kTicksPerFrame;
/* about the VRX drain period, 0.96 x TFRAME / 32 */
constexpr uint64_t kGapNs = 1000000;

class IpmxRxTpTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  int pkts_per_frame() const override {
    return 32;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
  }

  void EnableTimingParser(bool ipmx) {
    if (ipmx) ut20_ctx_add_flags(ctx_, ST20_RX_FLAG_IPMX);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/false), 0);
  }

  /* packets [0, pkts) of the frame-th frame, one every gap_ns */
  void FeedPkts(int frame, int pkts, uint64_t gap_ns) {
    for (int i = 0; i < pkts; i++)
      ut20_feed_frame_pkt_hw_ts(ctx_, i, kRtpTimestamp + frame * kTicksPerFrame,
                                MTL_SESSION_PORT_P,
                                kArrivalNs + frame * kFrameNs + i * gap_ns);
  }

  void FeedFrame(uint64_t gap_ns) {
    FeedPkts(0, pkts_per_frame(), gap_ns);
    ASSERT_EQ(frames_received(), 1);
  }

  struct ipmx_rx_tp_stat Stat() {
    struct ipmx_rx_tp_stat stat;
    EXPECT_EQ(ut20_ipmx_tp_stat(ctx_, MTL_SESSION_PORT_P, &stat), 0);
    return stat;
  }
};

TEST_F(IpmxRxTpTest, GappedFrameIsCompliant) {
  EnableTimingParser(true);
  FeedFrame(kGapNs);
  struct ipmx_rx_tp_stat stat = Stat();
  EXPECT_EQ(stat.cmax, 16);
  EXPECT_EQ(stat.vrx_full, 32);
  EXPECT_EQ(stat.frames_compliant, 1u);
  EXPECT_EQ(stat.frames_failed, 0u) << stat.failed_cause;
  EXPECT_EQ(stat.frames_unmeasured, 0u);
  EXPECT_LE(stat.cinst_max, 2);
}

TEST_F(IpmxRxTpTest, BurstFrameFailsOnCinst) {
  EnableTimingParser(true);
  FeedFrame(0);
  struct ipmx_rx_tp_stat stat = Stat();
  EXPECT_EQ(stat.frames_compliant, 0u);
  EXPECT_EQ(stat.frames_failed, 1u);
  EXPECT_STREQ(stat.failed_cause, "cinst exceed cmax");
  EXPECT_EQ(stat.cinst_max, 32);
}

/* a burst that would fail CINST, but the frame misses its last packet */
TEST_F(IpmxRxTpTest, IncompleteFrameIsUnmeasured) {
  EnableTimingParser(true);
  FeedPkts(0, pkts_per_frame() - 1, 0);
  FeedPkts(1, pkts_per_frame(), kGapNs);
  struct ipmx_rx_tp_stat stat = Stat();
  EXPECT_EQ(stat.frames_unmeasured, 1u);
  EXPECT_EQ(stat.frames_failed, 0u) << stat.failed_cause;
  EXPECT_EQ(stat.frames_compliant, 1u);
}

TEST_F(IpmxRxTpTest, FlagOffHasNoIpmxResult) {
  EnableTimingParser(false);
  FeedFrame(0);
  struct ipmx_rx_tp_stat stat;
  EXPECT_EQ(ut20_ipmx_tp_stat(ctx_, MTL_SESSION_PORT_P, &stat), -EIO);
}

TEST_F(IpmxRxTpTest, UnusedPortRejected) {
  EnableTimingParser(true);
  struct ipmx_rx_tp_stat stat;
  EXPECT_EQ(ut20_ipmx_tp_stat(ctx_, MTL_SESSION_PORT_R, &stat), -EINVAL);
}

TEST_F(IpmxRxTpTest, TimingParserOffHasNoIpmxResult) {
  ut20_ctx_add_flags(ctx_, ST20_RX_FLAG_IPMX);
  struct ipmx_rx_tp_stat stat;
  EXPECT_EQ(ut20_ipmx_tp_stat(ctx_, MTL_SESSION_PORT_P, &stat), -EIO);
}

}  // namespace
