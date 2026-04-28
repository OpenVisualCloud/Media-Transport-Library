/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST30p (audio) pipeline-layer frame-count contract tests.
 *
 *   stat_frames_received → bumped in st30p_rx_get_frame() (app consumes).
 *   stat_frames_dropped  → bumped in rx_st30p_frame_ready() when no free
 *                          framebuf is available (back-pressure).
 *   stat_busy            → bumps 1:1 with stat_frames_dropped.
 *
 * ST30p has no per-frame corrupted concept; frames_corrupted is not
 * tracked by this layer and not asserted on.
 */

#include <gtest/gtest.h>

#include "pipeline/st30p_harness.h"

class St30PipelineRxTest : public ::testing::Test {
 protected:
  ut30p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut30p_init(), 0) << "EAL init failed";
    ctx_ = ut30p_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut30p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int inject(uint32_t ts) {
    return ut30p_inject_frame(ctx_, ts);
  }
  struct st30_frame* get_frame() {
    return ut30p_get_frame(ctx_);
  }
  int put_frame(struct st30_frame* f) {
    return ut30p_put_frame(ctx_, f);
  }
  uint64_t frames_received() {
    return ut30p_stat_frames_received(ctx_);
  }
  uint64_t frames_dropped() {
    return ut30p_stat_frames_dropped(ctx_);
  }
  uint32_t stat_busy() {
    return ut30p_stat_busy(ctx_);
  }
};

/* Pipeline counts a frame as "received" only when the application calls
 * get_frame.  Inject N frames but consume only M < N → counter == M. */
TEST_F(St30PipelineRxTest, FramesReceivedOnlyOnGetFrame) {
  ASSERT_EQ(inject(1000), 0);
  ASSERT_EQ(inject(2000), 0);
  ASSERT_EQ(inject(3000), 0);

  EXPECT_EQ(frames_received(), 0u)
      << "frame_ready must not bump stat_frames_received — only get_frame does";

  struct st30_frame* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frames_received(), 1u);
  EXPECT_EQ(put_frame(f), 0);

  f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frames_received(), 2u);
  EXPECT_EQ(put_frame(f), 0);

  EXPECT_EQ(frames_received(), 2u);
}

/* When all framebufs are full, every further frame_ready bumps
 * stat_frames_dropped 1:1 with stat_busy. */
TEST_F(St30PipelineRxTest, FramesDroppedWhenFramebufsFull) {
  ASSERT_EQ(inject(1000), 0);
  ASSERT_EQ(inject(2000), 0);
  ASSERT_EQ(inject(3000), 0);
  EXPECT_EQ(frames_dropped(), 0u);
  EXPECT_EQ(stat_busy(), 0u);

  EXPECT_EQ(inject(4000), -EBUSY);
  EXPECT_EQ(inject(5000), -EBUSY);

  EXPECT_EQ(frames_dropped(), 2u);
  EXPECT_EQ(stat_busy(), 2u) << "stat_busy must bump 1:1 with stat_frames_dropped";
  EXPECT_EQ(frames_received(), 0u) << "dropped frames must not be counted as received";
}

/* reset_session_stats clears every cumulative pipeline counter. */
TEST_F(St30PipelineRxTest, ResetClearsAllPipelineCounters) {
  ASSERT_EQ(inject(1000), 0);
  struct st30_frame* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(put_frame(f), 0);

  /* fill and overflow */
  ASSERT_EQ(inject(2000), 0);
  ASSERT_EQ(inject(3000), 0);
  ASSERT_EQ(inject(4000), 0);
  EXPECT_EQ(inject(5000), -EBUSY);

  ASSERT_GT(frames_received(), 0u);
  ASSERT_GT(frames_dropped(), 0u);

  EXPECT_EQ(ut30p_reset_session_stats(ctx_), 0);

  EXPECT_EQ(frames_received(), 0u);
  EXPECT_EQ(frames_dropped(), 0u);
}

/* st30p_rx_get_session_stats overlays pipeline frame counters on top
 * of the (zeroed) transport stats.  The API result must match the raw
 * counters byte-for-byte. */
TEST_F(St30PipelineRxTest, GetSessionStatsOverlay) {
  ASSERT_EQ(inject(1000), 0);
  ASSERT_EQ(inject(2000), 0);
  for (int i = 0; i < 2; i++) {
    struct st30_frame* f = get_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(put_frame(f), 0);
  }
  ASSERT_EQ(inject(3000), 0);
  ASSERT_EQ(inject(4000), 0);
  ASSERT_EQ(inject(5000), 0);
  EXPECT_EQ(inject(6000), -EBUSY);

  struct st30_rx_user_stats api {};
  ASSERT_EQ(ut30p_get_session_stats(ctx_, &api), 0);

  EXPECT_EQ(api.common.stat_frames_received, frames_received());
  EXPECT_EQ(api.common.stat_frames_dropped, frames_dropped());
}
