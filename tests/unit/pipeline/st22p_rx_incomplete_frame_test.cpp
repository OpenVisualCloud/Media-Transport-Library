/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p RX: an incomplete frame the pipeline refuses must go back to the session.
 *
 * With ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME the session ignores what
 * notify_frame_ready returns for an incomplete frame, so a refusing pipeline has
 * to put the frame itself. A refused complete frame is put by the session, so the
 * pipeline must not put it too.
 *
 * A real single-port ST 2110-22 session (session/st20_harness) delivers its frames
 * to the real rx_st22p_frame_ready() (pipeline/st22p_harness), which puts them back
 * through the session's own st22_rx_put_framebuff(). Each new timestamp evicts the
 * previous, unfinished codestream as incomplete.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>

#include "pipeline/st22p_harness.h"
#include "session/st20_harness.h"

namespace {

constexpr int kPipelineFrames = 2;
constexpr uint32_t kJpvsLen = 42;
constexpr uint32_t kColrLen = 18;
constexpr uint16_t kCodestreamLen = 40;

struct Bridge {
  ut22p_ctx* pipeline;
  int complete_refused;
};

int notify_pipeline(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
  Bridge* bridge = static_cast<Bridge*>(priv);
  int ret = ut22p_frame_ready(bridge->pipeline, frame, meta);
  if (ret < 0 && meta->status == ST_FRAME_STATUS_COMPLETE) bridge->complete_refused++;
  return ret;
}

int query_ext_frame_fail(void* priv, struct st_ext_frame* ext_frame,
                         struct st22_rx_frame_meta* meta) {
  (void)ext_frame;
  (void)meta;
  ++*static_cast<int*>(priv);
  return -ENOMEM;
}

/* Succeeds with an empty frame, which st_frame_sanity_check() rejects. */
int query_ext_frame_empty(void* priv, struct st_ext_frame* ext_frame,
                          struct st22_rx_frame_meta* meta) {
  (void)ext_frame;
  (void)meta;
  ++*static_cast<int*>(priv);
  return 0;
}

}  // namespace

class St22PipelineRxIncompleteFrameTest : public ::testing::Test {
 protected:
  ut20_test_ctx* session_ = nullptr;
  ut22p_ctx* pipeline_ = nullptr;
  Bridge bridge_ = {};
  uint8_t payload_[kJpvsLen + kColrLen + kCodestreamLen];
  uint32_t seq_ = 0;

  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ASSERT_EQ(ut22p_init(), 0);
    session_ = ut20_ctx_create(1);
    ASSERT_NE(session_, nullptr);
    pipeline_ = ut22p_ctx_create(kPipelineFrames);
    ASSERT_NE(pipeline_, nullptr);
    ut20_ctx_enable_st22(session_);
    ut20_ctx_set_flags(session_, ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME);
    bridge_.pipeline = pipeline_;
    ut20_ctx_set_st22_notify(session_, notify_pipeline, &bridge_);
    ut22p_set_transport(pipeline_, ut20_st22_handle(session_));

    memset(payload_, 0xA5, sizeof(payload_));
    ASSERT_EQ(ut20_st22_build_boxes(payload_, kJpvsLen, kColrLen), kJpvsLen + kColrLen);
  }

  void TearDown() override {
    ut22p_ctx_destroy(pipeline_);
    ut20_ctx_destroy(session_);
  }

  void feed_first_pkt(uint32_t ts, bool marker) {
    ut20_feed_st22_pkt(session_, seq_++, ts, 0, marker, payload_, sizeof(payload_),
                       MTL_SESSION_PORT_P);
  }

  void start_frame(uint32_t ts) {
    feed_first_pkt(ts, false);
  }

  void fill_pipeline() {
    for (int i = 0; i < kPipelineFrames; i++)
      ASSERT_EQ(ut22p_inject_frame(pipeline_, ST_FRAME_STATUS_COMPLETE, i), 0);
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

  /* One refusal more than the session has frames: each leak would exhaust it. */
  void expect_refused_incomplete_frames_returned() {
    const int refusals = ut20_frame_count() + 1;
    for (int i = 0; i <= refusals; i++) start_frame(1000 * (i + 1));

    EXPECT_EQ(ut20_stat_slot_get_frame_fail(session_), 0u)
        << "a refused incomplete frame was never returned to the session";
    EXPECT_EQ(ut20_stat_frames_incomplete(session_), (uint64_t)refusals);
    EXPECT_EQ(held_frames(), 1) << "only the frame still being received may be held";
  }
};

TEST_F(St22PipelineRxIncompleteFrameTest, NotReadyReturnsIncompleteFrame) {
  ut22p_set_ready(pipeline_, false);
  expect_refused_incomplete_frames_returned();
}

TEST_F(St22PipelineRxIncompleteFrameTest, NoFreeFramebuffReturnsIncompleteFrame) {
  fill_pipeline();
  expect_refused_incomplete_frames_returned();
}

TEST_F(St22PipelineRxIncompleteFrameTest, ExtFrameQueryFailureReturnsIncompleteFrame) {
  int queries = 0;
  ut22p_enable_ext_frame(pipeline_, query_ext_frame_fail, &queries);
  expect_refused_incomplete_frames_returned();
  EXPECT_EQ(queries, ut20_frame_count() + 1);
}

TEST_F(St22PipelineRxIncompleteFrameTest, ExtFrameSanityFailureReturnsIncompleteFrame) {
  int queries = 0;
  ut22p_enable_ext_frame(pipeline_, query_ext_frame_empty, &queries);
  expect_refused_incomplete_frames_returned();
  EXPECT_EQ(queries, ut20_frame_count() + 1);
}

TEST_F(St22PipelineRxIncompleteFrameTest, RefusedCompleteFrameIsPutOnlyBySession) {
  fill_pipeline();
  feed_first_pkt(1000, true);

  ASSERT_EQ(bridge_.complete_refused, 1);
  EXPECT_EQ(ut20_frame_refcnt(session_, 0), 0) << "-1 means the frame was put twice";
}

/* The session runs before st22_rx_create() returns the handle to the pipeline. With
 * every session frame refused no further callback comes, so storing the handle must
 * return them. */
TEST_F(St22PipelineRxIncompleteFrameTest,
       FramesRefusedBeforeTransportIsKnownAreReturned) {
  ut22p_set_ready(pipeline_, false);
  ut22p_set_transport(pipeline_, nullptr);
  for (int i = 0; i <= ut20_frame_count(); i++) start_frame(1000 * (i + 1));
  ASSERT_EQ(held_frames(), ut20_frame_count());
  const uint64_t get_frame_fail = ut20_stat_slot_get_frame_fail(session_);

  ut22p_set_transport(pipeline_, ut20_st22_handle(session_));
  EXPECT_EQ(held_frames(), 0) << "a frame refused before the handle was known leaked";

  start_frame(100000);
  EXPECT_EQ(ut20_stat_slot_get_frame_fail(session_), get_frame_fail);
}
