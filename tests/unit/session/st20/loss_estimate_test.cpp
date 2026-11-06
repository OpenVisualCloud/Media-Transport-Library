/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The unrecovered packets of an incomplete frame: counted against the last
 * complete frame, or estimated from the size of the packets it did receive until
 * one completes.
 */

#include <gtest/gtest.h>

#include <cfenv>

#include "session/st20/st20_rx_test_base.h"

class St20RxLossEstimateTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  int pkts_per_frame() const override {
    return 4;
  }

  void feed_complete_frame(uint32_t ts) {
    for (int i = 0; i < pkts_per_frame(); i++) feed(i, ts, MTL_SESSION_PORT_P);
  }

  /* a packet whose row is out of the frame, which the session drops */
  void feed_malformed(uint32_t ts) {
    ut20_feed_pkt(ctx_, 0, ts, /*line_num=*/pkts_per_frame(), 0, 40, MTL_SESSION_PORT_P);
  }
};

/* Before any frame has completed, a frame whose every packet was malformed has
 * nothing to estimate loss from. On x86 the float to int conversion of 0/0 yields
 * INT_MIN, so only the FP exception flags show the division. */
TEST_F(St20RxLossEstimateTest, FrameWithNoValidPacketEstimatesNoLoss) {
  feed_malformed(1000);
  EXPECT_EQ(offset_dropped(), 1u);

  std::feclearexcept(FE_ALL_EXCEPT);
  feed(0, 2000, MTL_SESSION_PORT_P); /* evicts the frame at 1000 */
  EXPECT_FALSE(std::fetestexcept(FE_DIVBYZERO | FE_INVALID));
  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

TEST_F(St20RxLossEstimateTest, MissingPacketsCountedAgainstLastCompleteFrame) {
  feed_complete_frame(1000);
  feed(0, 2000, MTL_SESSION_PORT_P);
  feed(0, 3000, MTL_SESSION_PORT_P); /* evicts the frame at 2000 */
  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), (uint64_t)pkts_per_frame() - 1);
}

/* Once a frame has completed, a frame with no valid packet lost all of them. */
TEST_F(St20RxLossEstimateTest, FrameWithNoValidPacketAfterCompleteFrameLostEveryPacket) {
  feed_complete_frame(1000);
  feed_malformed(2000);
  EXPECT_EQ(offset_dropped(), 1u);

  std::feclearexcept(FE_ALL_EXCEPT);
  feed(0, 3000, MTL_SESSION_PORT_P); /* evicts the frame at 2000 */
  EXPECT_FALSE(std::fetestexcept(FE_DIVBYZERO | FE_INVALID));
  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), (uint64_t)pkts_per_frame());
}

/* Received packets of different sizes, like a frame's shorter last packet, skew a
 * byte estimate: 160 bytes in 4 rows, 60 received in 2 packets estimates 3 missing. */
TEST_F(St20RxLossEstimateTest, PacketSizesDoNotSkewTheCount) {
  feed_complete_frame(1000);
  feed(0, 2000, MTL_SESSION_PORT_P);
  /* half of row 1, in sequence after packet 0 */
  ut20_feed_pkt(ctx_, 2000u * pkts_per_frame() + 1, 2000, /*line_num=*/1, 0, 20,
                MTL_SESSION_PORT_P);
  EXPECT_EQ(received(), (uint64_t)pkts_per_frame() + 2);
  feed(0, 3000, MTL_SESSION_PORT_P); /* evicts the frame at 2000 */
  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), (uint64_t)pkts_per_frame() - 2);
}
