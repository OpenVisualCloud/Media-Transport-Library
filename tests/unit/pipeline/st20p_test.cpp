/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) pipeline-layer frame-count contract tests.
 *
 *   stat_frames_received  → bumped in st20p_rx_get_frame() (app consumes).
 *   stat_frames_dropped   → bumped in rx_st20p_frame_ready() when no free
 *                           framebuf is available (back-pressure).
 *   stat_frames_corrupted → bumped in st20p_rx_get_frame() iff the frame's
 *                           status is ST_FRAME_STATUS_CORRUPTED.
 *   stat_busy             → atomic; bumps 1:1 with stat_frames_dropped.
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_harness.h"

class St20PipelineRxTest : public ::testing::Test {
 protected:
  ut20p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";
    ctx_ = ut20p_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int inject_complete(uint32_t ts) {
    return ut20p_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, ts);
  }
  int inject_corrupted(uint32_t ts) {
    return ut20p_inject_frame(ctx_, ST_FRAME_STATUS_CORRUPTED, ts);
  }
  struct st_frame* get_frame() {
    return ut20p_get_frame(ctx_);
  }
  int put_frame(struct st_frame* f) {
    return ut20p_put_frame(ctx_, f);
  }

  uint64_t frames_received() {
    return ut20p_stat_frames_received(ctx_);
  }
  uint64_t frames_dropped() {
    return ut20p_stat_frames_dropped(ctx_);
  }
  uint64_t frames_corrupted() {
    return ut20p_stat_frames_corrupted(ctx_);
  }
  uint32_t stat_busy() {
    return ut20p_stat_busy(ctx_);
  }
};

/* Pipeline counts a frame as "received" only when the application calls
 * get_frame.  Inject N frames but consume only M < N → counter == M. */
TEST_F(St20PipelineRxTest, FramesReceivedOnlyOnGetFrame) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  EXPECT_EQ(frames_received(), 0u)
      << "frame_ready must not bump stat_frames_received — only get_frame does";

  struct st_frame* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frames_received(), 1u);
  EXPECT_EQ(put_frame(f), 0);

  /* second consume */
  f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frames_received(), 2u);
  EXPECT_EQ(put_frame(f), 0);

  /* third frame still pending in framebuf — not yet counted */
  EXPECT_EQ(frames_received(), 2u);
}

/* When all framebufs are full (no get_frame draining), every further
 * frame_ready must bump stat_frames_dropped 1:1 with stat_busy. */
TEST_F(St20PipelineRxTest, FramesDroppedWhenFramebufsFull) {
  /* fill all 3 slots */
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  EXPECT_EQ(frames_dropped(), 0u);
  EXPECT_EQ(stat_busy(), 0u);

  /* further frames have nowhere to go */
  EXPECT_EQ(inject_complete(4000), -EBUSY);
  EXPECT_EQ(inject_complete(5000), -EBUSY);
  EXPECT_EQ(inject_complete(6000), -EBUSY);

  EXPECT_EQ(frames_dropped(), 3u);
  EXPECT_EQ(stat_busy(), 3u)
      << "stat_busy must bump 1:1 with stat_frames_dropped (same back-pressure event)";
  EXPECT_EQ(frames_received(), 0u) << "dropped frames must not be counted as received";
}

/* CORRUPTED frames are still delivered to the application.  They bump
 * BOTH stat_frames_received and stat_frames_corrupted; COMPLETE frames
 * bump only the former. */
TEST_F(St20PipelineRxTest, CorruptedDeliveredAndCounted) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  for (int i = 0; i < 3; i++) {
    struct st_frame* f = get_frame();
    ASSERT_NE(f, nullptr) << "frame " << i;
    EXPECT_EQ(put_frame(f), 0);
  }

  EXPECT_EQ(frames_received(), 3u);
  EXPECT_EQ(frames_corrupted(), 1u)
      << "only the CORRUPTED frame must bump stat_frames_corrupted";
}

/* reset_session_stats clears every cumulative pipeline counter. */
TEST_F(St20PipelineRxTest, ResetClearsAllPipelineCounters) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  struct st_frame* f1 = get_frame();
  ASSERT_NE(f1, nullptr);
  EXPECT_EQ(put_frame(f1), 0);
  struct st_frame* f2 = get_frame();
  ASSERT_NE(f2, nullptr);
  EXPECT_EQ(put_frame(f2), 0);

  /* fill up and overflow to exercise frames_dropped */
  ASSERT_EQ(inject_complete(3000), 0);
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  EXPECT_EQ(inject_complete(6000), -EBUSY);

  ASSERT_GT(frames_received(), 0u);
  ASSERT_GT(frames_corrupted(), 0u);
  ASSERT_GT(frames_dropped(), 0u);

  EXPECT_EQ(ut20p_reset_session_stats(ctx_), 0);

  EXPECT_EQ(frames_received(), 0u);
  EXPECT_EQ(frames_corrupted(), 0u);
  EXPECT_EQ(frames_dropped(), 0u);
}

/* st20p_rx_get_session_stats overlays the pipeline frame counters on top
 * of the (zeroed) transport stats.  The API result must match the
 * raw counters byte-for-byte. */
TEST_F(St20PipelineRxTest, GetSessionStatsOverlay) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  for (int i = 0; i < 3; i++) {
    struct st_frame* f = get_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(put_frame(f), 0);
  }
  /* trigger one drop */
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  ASSERT_EQ(inject_complete(6000), 0);
  EXPECT_EQ(inject_complete(7000), -EBUSY);

  struct st20_rx_user_stats api {};
  ASSERT_EQ(ut20p_get_session_stats(ctx_, &api), 0);

  EXPECT_EQ(api.common.stat_frames_received, frames_received());
  EXPECT_EQ(api.common.stat_frames_corrupted, frames_corrupted());
  EXPECT_EQ(api.common.stat_frames_dropped, frames_dropped());
}

/* Invariant: stat_busy and stat_frames_dropped move in lockstep — every
 * back-pressure event bumps both, never one without the other. */
TEST_F(St20PipelineRxTest, BusyEqualsDroppedInvariant) {
  /* fill */
  for (int i = 0; i < 3; i++) ASSERT_EQ(inject_complete(1000 + i), 0);
  /* overflow burst */
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(inject_complete(2000 + i), -EBUSY);
    EXPECT_EQ(stat_busy(), frames_dropped())
        << "after drop " << (i + 1) << ": stat_busy and frames_dropped diverged";
  }
}
