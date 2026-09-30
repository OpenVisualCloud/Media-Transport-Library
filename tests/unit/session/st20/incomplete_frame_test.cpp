/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Framebuffer ownership when an incomplete frame is delivered with
 * ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME: a frame the app refuses from
 * notify_frame_ready must go back to the session, as a refused complete one does.
 *
 * Single port, so slot_max is 1 and every new RTP timestamp closes the open
 * frame; feeding only pkt 0 of each frame leaves each one incomplete.
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "session/st20/st20_rx_test_base.h"

class St20RxIncompleteFrameTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_incomplete_frames(ctx_);
  }
};

class St22RxIncompleteFrameTest : public St20RxIncompleteFrameTest {
 protected:
  void SetUp() override {
    St20RxIncompleteFrameTest::SetUp();
    ut20_ctx_enable_st22(ctx_);
  }
  int feed_first_pkt(uint32_t seq, uint32_t ts) {
    uint8_t payload[64] = {0};
    uint16_t len = ut20_st22_build_boxes(payload, 42, 18);
    return ut20_feed_st22_pkt(ctx_, seq, ts, 0, false, payload, len, MTL_SESSION_PORT_P);
  }
};

TEST_F(St20RxIncompleteFrameTest, RefusedIncompleteFrameReturnsBuffer) {
  ut20_set_notify_frame_ready_ret(ctx_, -EBUSY);
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 2000, MTL_SESSION_PORT_P);
  feed(0, 3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_incomplete(), 2u);
  EXPECT_EQ(ut20_stat_slot_get_frame_fail(ctx_), 0u);
  EXPECT_EQ(ut20_free_frames(ctx_), 1); /* the other one holds ts 3000 */
}

TEST_F(St20RxIncompleteFrameTest, AcceptedIncompleteFrameStaysWithApp) {
  ut20_set_hold_frames(ctx_, true);
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_incomplete(), 1u);
  EXPECT_EQ(ut20_free_frames(ctx_), 0);
}

TEST_F(St22RxIncompleteFrameTest, RefusedIncompleteFrameReturnsBuffer) {
  ut20_set_notify_frame_ready_ret(ctx_, -EBUSY);
  ASSERT_EQ(feed_first_pkt(100, 1000), 0);
  ASSERT_EQ(feed_first_pkt(200, 2000), 0);
  ASSERT_EQ(feed_first_pkt(300, 3000), 0);

  EXPECT_EQ(ut20_st22_frames_ready(ctx_), 2u);
  EXPECT_EQ(ut20_stat_slot_get_frame_fail(ctx_), 0u);
  EXPECT_EQ(ut20_free_frames(ctx_), 1);
}
