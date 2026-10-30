/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The session contract for refusals, which in-tree receivers rely on: a refused
 * uframe payload still counts toward its frame, and an incomplete frame handed
 * to notify_frame_ready belongs to the receiver whatever it returns.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxCallbackRefusalTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

TEST_F(St20RxCallbackRefusalTest, RefusedUframePayloadStillCountsTowardFrame) {
  ut20_ctx_enable_uframe(ctx_, /*refuse_row=*/1);
  feed_full(1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(frames_incomplete(), 0u);
}

/* The receiver puts each incomplete frame back itself and returns -1; a second put
 * by the session would leave the harness's two frames unusable. */
TEST_F(St20RxCallbackRefusalTest, IncompleteFrameRefusedByNotifyStaysWithReceiver) {
  ut20_ctx_refuse_incomplete_frames(ctx_);
  for (uint32_t ts = 1000; ts <= 4000; ts += 1000) feed(0, ts, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_incomplete(), 3u);
  EXPECT_EQ(ut20_stat_slot_get_frame_fail(ctx_), 0u);
  EXPECT_EQ(ut20_free_frames(ctx_), 1) << "the frame at 4000 holds the other";
}
