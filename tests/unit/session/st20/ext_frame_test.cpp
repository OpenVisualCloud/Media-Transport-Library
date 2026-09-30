/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Validation of application-supplied RX frame buffers: the dedicated
 * st20_rx_ops.ext_frames[] array at create time and each buffer handed out by
 * query_ext_frame at the start of a frame.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxExtFrameTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  static constexpr uint64_t kIova = 0x100000;
};

TEST_F(St20RxExtFrameTest, DedicatedExtFrameSmallerThanFrameRejected) {
  EXPECT_LT(ut20_alloc_ext_frames(ctx_, ut20_frame_size(ctx_) - 1), 0);
}

TEST_F(St20RxExtFrameTest, DedicatedExtFrameOfFrameSizeAccepted) {
  EXPECT_EQ(ut20_alloc_ext_frames(ctx_, ut20_frame_size(ctx_)), 0);
}

/* Unfixed, the payload copy writes through the NULL address and the test crashes. */
TEST_F(St20RxExtFrameTest, QueriedExtFrameWithoutAddressRejected) {
  ut20_ctx_enable_query_ext_frame(ctx_, false, kIova);
  EXPECT_LT(ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(ut20_stat_slot_query_ext_fail(ctx_), 1u);
  EXPECT_EQ(received(), 0u);
}

/* With DMA offload the frame iova is the DMA write target; 0 would aim it at IOVA 0. */
TEST_F(St20RxExtFrameTest, QueriedExtFrameWithoutIovaRejectedWithDma) {
  ut20_ctx_enable_query_ext_frame(ctx_, true, 0);
  ut20_ctx_attach_idle_dma(ctx_);
  EXPECT_LT(ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(ut20_stat_slot_query_ext_fail(ctx_), 1u);
  EXPECT_EQ(received(), 0u);
}

/* The CPU copy path never reads the iova, so an unmapped buffer stays usable. */
TEST_F(St20RxExtFrameTest, QueriedExtFrameWithoutIovaAcceptedWithoutDma) {
  ut20_ctx_enable_query_ext_frame(ctx_, true, 0);
  EXPECT_EQ(ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(ut20_stat_slot_query_ext_fail(ctx_), 0u);
  EXPECT_EQ(received(), 1u);
}
