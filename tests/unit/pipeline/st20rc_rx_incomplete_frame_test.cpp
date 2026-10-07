/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20 redundant-combined RX: an incomplete frame refused while st20rc_rx_create()
 * is still creating its sessions must go back to the session.
 *
 * st20rc always sets ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME, and the session
 * ignores what notify_frame_ready returns for an incomplete frame, so the
 * not-ready refusal has to put the frame itself. A refused complete frame is put
 * by the session, so st20rc must not put it too.
 *
 * A real single-port session (session/st20_harness) is the P-port transport; each
 * new timestamp evicts the previous, half-received frame as incomplete.
 */

#include <gtest/gtest.h>

#include "pipeline/st20rc_harness.h"
#include "session/st20_harness.h"

namespace {

int notify_rc(void* priv, void* frame, struct st20_rx_frame_meta* meta) {
  return ut20rc_frame_ready(static_cast<ut20rc_ctx*>(priv), frame, meta);
}

}  // namespace

class St20rcRxIncompleteFrameTest : public ::testing::Test {
 protected:
  ut20_test_ctx* session_ = nullptr;
  ut20rc_ctx* rc_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    session_ = ut20_ctx_create(1);
    ASSERT_NE(session_, nullptr);
    rc_ = ut20rc_ctx_create(ut20_handle(session_));
    ASSERT_NE(rc_, nullptr);
    ut20_ctx_set_flags(session_, ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME);
    ut20_ctx_set_notify(session_, notify_rc, rc_);
  }

  void TearDown() override {
    ut20rc_ctx_destroy(rc_);
    ut20_ctx_destroy(session_);
  }

  void start_frame(uint32_t ts) {
    ut20_feed_frame_pkt(session_, 0, ts, MTL_SESSION_PORT_P);
  }

  int held_frames() {
    int held = 0;
    for (int i = 0; i < ut20_frame_count(); i++) {
      int refcnt = ut20_frame_refcnt(session_, i);
      EXPECT_GE(refcnt, 0) << "transport frame " << i << " was put twice";
      if (refcnt > 0) held++;
    }
    return held;
  }
};

/* One refusal more than the session has frames: each leak would exhaust it. */
TEST_F(St20rcRxIncompleteFrameTest, NotReadyReturnsIncompleteFrame) {
  const int refusals = ut20_frame_count() + 1;
  for (int i = 0; i <= refusals; i++) start_frame(1000 * (i + 1));

  EXPECT_EQ(ut20_stat_slot_get_frame_fail(session_), 0u)
      << "a refused incomplete frame was never returned to the session";
  EXPECT_EQ(ut20_stat_frames_incomplete(session_), (uint64_t)refusals);
  EXPECT_EQ(held_frames(), 1) << "only the frame still being received may be held";
}

TEST_F(St20rcRxIncompleteFrameTest, NotReadyCompleteFrameIsPutOnlyBySession) {
  ut20_feed_full_frame(session_, 1000, MTL_SESSION_PORT_P);

  ASSERT_EQ(ut20_frames_received(session_), 1);
  EXPECT_EQ(ut20_frame_refcnt(session_, 0), 0) << "-1 means the frame was put twice";
}

/* The session runs before st20_rx_create_with_mask() returns its handle to st20rc. With
 * every session frame refused no further callback comes, so storing the handle must
 * return them. */
TEST_F(St20rcRxIncompleteFrameTest, FramesRefusedBeforeHandleIsKnownAreReturned) {
  ut20rc_set_handle(rc_, nullptr);
  for (int i = 0; i <= ut20_frame_count(); i++) start_frame(1000 * (i + 1));
  ASSERT_EQ(held_frames(), ut20_frame_count());
  const uint64_t get_frame_fail = ut20_stat_slot_get_frame_fail(session_);

  ut20rc_set_handle(rc_, ut20_handle(session_));
  EXPECT_EQ(held_frames(), 0) << "a frame refused before the handle was known leaked";

  start_frame(100000);
  EXPECT_EQ(ut20_stat_slot_get_frame_fail(session_), get_frame_fail);
}
