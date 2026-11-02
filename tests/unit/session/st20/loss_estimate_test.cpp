/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The unrecovered-packet estimate of an incomplete frame, taken from the size of
 * the packets it did receive.
 */

#include <gtest/gtest.h>

#include <cfenv>

#include "session/st20/st20_rx_test_base.h"

class St20RxLossEstimateTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

/* A frame whose every packet was malformed has nothing to estimate loss from. On
 * x86 the float to int conversion of 0/0 yields INT_MIN, so only the FP exception
 * flags show the division. */
TEST_F(St20RxLossEstimateTest, FrameWithNoValidPacketEstimatesNoLoss) {
  ut20_feed_pkt(ctx_, 0, 1000, /*line_num=*/pkts_per_frame(), 0, 40, MTL_SESSION_PORT_P);
  EXPECT_EQ(offset_dropped(), 1u);

  std::feclearexcept(FE_ALL_EXCEPT);
  feed(0, 2000, MTL_SESSION_PORT_P); /* evicts the frame at 1000 */
  EXPECT_FALSE(std::fetestexcept(FE_DIVBYZERO | FE_INVALID));
  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(pkts_unrecovered(), 0u);
}
