/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Who puts a frame the app refuses from notify_frame_ready (rv_frame_notify).
 *
 * A refused complete frame is put back by the session. An incomplete frame,
 * delivered only under ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME, belongs to the app
 * whatever the callback returns, so the app always puts it with
 * st20_rx_put_framebuff() and the session never does.
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "session/st20/st20_rx_test_base.h"

namespace {

struct App {
  st20_rx_handle handle;
  bool put;
  int notified;
};

int refuse(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  (void)meta;
  App* app = static_cast<App*>(priv);
  app->notified++;
  if (app->put) st20_rx_put_framebuff(app->handle, frame);
  return -EBUSY;
}

}  // namespace

class St20RxIncompleteFrameOwnershipTest : public St20RxBaseTest {
 protected:
  App app_ = {};

  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    app_.handle = ut20_handle(ctx_);
    ut20_ctx_set_flags(ctx_, ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME);
    ut20_ctx_set_notify(ctx_, refuse, &app_);
  }

  /* The second timestamp evicts the first, half-received frame as incomplete. */
  void deliver_incomplete_frame() {
    ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
    ut20_feed_frame_pkt(ctx_, 0, 2000, MTL_SESSION_PORT_P);
    ASSERT_EQ(app_.notified, 1);
    ASSERT_EQ(frames_incomplete(), 1u);
  }

  int refcnt_sum() {
    int sum = 0;
    for (int i = 0; i < ut20_frame_count(); i++) sum += ut20_frame_refcnt(ctx_, i);
    return sum;
  }
};

TEST_F(St20RxIncompleteFrameOwnershipTest, RefusedIncompleteFrameIsNotPutBySession) {
  deliver_incomplete_frame();
  EXPECT_EQ(ut20_frame_refcnt(ctx_, 0), 1) << "the refused frame is still the app's";
  EXPECT_EQ(refcnt_sum(), 2) << "the app's frame plus the one the slot receives into";
}

TEST_F(St20RxIncompleteFrameOwnershipTest, IncompleteFramePutByAppIsNotPutAgain) {
  app_.put = true;
  deliver_incomplete_frame();
  for (int i = 0; i < ut20_frame_count(); i++)
    EXPECT_GE(ut20_frame_refcnt(ctx_, i), 0) << "frame " << i << " was put twice";
  EXPECT_EQ(refcnt_sum(), 1) << "only the frame the slot receives into";
}

TEST_F(St20RxIncompleteFrameOwnershipTest, RefusedCompleteFrameIsPutBySession) {
  ut20_feed_full_frame(ctx_, 1000, MTL_SESSION_PORT_P);
  ASSERT_EQ(app_.notified, 1);
  EXPECT_EQ(ut20_frame_refcnt(ctx_, 0), 0);
}
