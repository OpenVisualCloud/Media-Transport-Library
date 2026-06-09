/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20 NEW-API (unified session) RX frame-count contract tests.
 *
 * Parallel copy of tests/unit/pipeline/st20p_test.cpp, adapted to the unified
 * session API in lib/src/new_api. Old → new mapping:
 *
 *   pipeline stat_frames_received  ≙ s->stats.buffers_processed
 *     (bumped in buffer_get, NOT at datapath ingress).
 *   pipeline stat_frames_dropped   ≙ s->stats.buffers_dropped
 *     (bumped in notify_frame_ready when the ready_ring is full).
 *   pipeline stat_busy             → no equivalent. The unified API has no
 *     separate busy atomic; back-pressure is counted solely by buffers_dropped,
 *     which bumps 1:1 with each ready_ring overflow.
 *   pipeline stat_frames_corrupted → no equivalent. A non-COMPLETE frame is
 *     still delivered by buffer_get with status MTL_FRAME_STATUS_INCOMPLETE and
 *     the MTL_BUF_FLAG_INCOMPLETE flag, and still counts as processed.
 *   pipeline get_session_stats overlay → io_stats_get is pure transport
 *     passthrough; the abstract counters live on stats_get instead.
 */

#include <gtest/gtest.h>

#include "new_api/st20_rx_harness.h"

class St20NewApiRxTest : public ::testing::Test {
 protected:
  ut20rx_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20rx_init(), 0) << "EAL init failed";
    ctx_ = ut20rx_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20rx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int inject_complete(uint32_t ts) {
    return ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_COMPLETE, ts);
  }
  int inject_corrupted(uint32_t ts) {
    return ut20rx_inject_frame(ctx_, ST_FRAME_STATUS_CORRUPTED, ts);
  }
  mtl_buffer_t* get_buffer() {
    return ut20rx_buffer_get(ctx_);
  }
  int put_buffer(mtl_buffer_t* b) {
    return ut20rx_buffer_put(ctx_, b);
  }

  uint64_t buffers_processed() {
    return ut20rx_buffers_processed(ctx_);
  }
  uint64_t buffers_dropped() {
    return ut20rx_buffers_dropped(ctx_);
  }
};

/* The unified session counts a buffer as "processed" only when the application
 * calls buffer_get. Inject N frames but consume only M < N → counter == M.
 * (pipeline analog: FramesReceivedOnlyOnGetFrame.) */
TEST_F(St20NewApiRxTest, BuffersProcessedOnlyOnBufferGet) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  EXPECT_EQ(buffers_processed(), 0u)
      << "notify_frame_ready must not bump buffers_processed — only buffer_get does";

  mtl_buffer_t* b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(buffers_processed(), 1u);
  EXPECT_EQ(put_buffer(b), 0);

  /* second consume */
  b = get_buffer();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(buffers_processed(), 2u);
  EXPECT_EQ(put_buffer(b), 0);

  /* third frame still pending in the ready_ring — not yet counted */
  EXPECT_EQ(buffers_processed(), 2u);
}

/* When the ready_ring is full (no buffer_get draining), every further
 * notify_frame_ready must bump buffers_dropped 1:1 with the overflow event.
 * (pipeline analog: FramesDroppedWhenFramebufsFull. The pipeline's
 * stat_busy 1:1 invariant is dropped — the unified API has no busy atomic.) */
TEST_F(St20NewApiRxTest, BuffersDroppedWhenReadyRingFull) {
  /* fill all 3 ready_ring slots */
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_complete(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  EXPECT_EQ(buffers_dropped(), 0u);

  /* further frames have nowhere to go */
  EXPECT_EQ(inject_complete(4000), -ENOSPC);
  EXPECT_EQ(inject_complete(5000), -ENOSPC);
  EXPECT_EQ(inject_complete(6000), -ENOSPC);

  EXPECT_EQ(buffers_dropped(), 3u);
  EXPECT_EQ(buffers_processed(), 0u) << "dropped frames must not be counted as processed";
}

/* A non-COMPLETE (CORRUPTED) frame is still delivered to the application.
 * Unlike the pipeline (which has a dedicated stat_frames_corrupted), the
 * unified API surfaces corruption only through buffer status/flags: the buffer
 * carries MTL_FRAME_STATUS_INCOMPLETE + MTL_BUF_FLAG_INCOMPLETE and still
 * counts toward buffers_processed. COMPLETE frames stay COMPLETE/no-flag.
 * (pipeline analog: CorruptedDeliveredAndCounted.) */
TEST_F(St20NewApiRxTest, IncompleteDeliveredWithStatusFlag) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);

  mtl_buffer_t* b0 = get_buffer();
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->status, MTL_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(b0->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b0), 0);

  mtl_buffer_t* b1 = get_buffer();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(b1->status, MTL_FRAME_STATUS_INCOMPLETE)
      << "a non-COMPLETE frame must still be delivered, marked incomplete";
  EXPECT_NE(b1->flags & MTL_BUF_FLAG_INCOMPLETE, 0u);
  EXPECT_EQ(put_buffer(b1), 0);

  mtl_buffer_t* b2 = get_buffer();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(b2->status, MTL_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(put_buffer(b2), 0);

  EXPECT_EQ(buffers_processed(), 3u)
      << "every delivered frame, complete or not, counts as processed";
}

/* stats_reset clears every cumulative unified-session counter.
 * (pipeline analog: ResetClearsAllPipelineCounters.) */
TEST_F(St20NewApiRxTest, ResetClearsAllCounters) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  mtl_buffer_t* b1 = get_buffer();
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(put_buffer(b1), 0);
  mtl_buffer_t* b2 = get_buffer();
  ASSERT_NE(b2, nullptr);
  EXPECT_EQ(put_buffer(b2), 0);

  /* fill up and overflow to exercise buffers_dropped */
  ASSERT_EQ(inject_complete(3000), 0);
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  EXPECT_EQ(inject_complete(6000), -ENOSPC);

  ASSERT_GT(buffers_processed(), 0u);
  ASSERT_GT(buffers_dropped(), 0u);
  ASSERT_GT(ut20rx_bytes_processed(ctx_), 0u);

  EXPECT_EQ(ut20rx_reset_stats(ctx_), 0);

  EXPECT_EQ(buffers_processed(), 0u);
  EXPECT_EQ(buffers_dropped(), 0u);
  EXPECT_EQ(ut20rx_bytes_processed(ctx_), 0u);
}

/* The unified API exposes two distinct stats surfaces:
 *   - stats_get   → the abstract counters this suite drives.
 *   - io_stats_get → a pure passthrough of the transport's own stats.
 * Unlike the pipeline (which OVERLAYS the frame counters onto the transport
 * stats), the two surfaces are independent here. Assert that stats_get matches
 * what the test drove, while the passthrough io_stats returns the (stubbed)
 * zeroed transport stats. (pipeline analog: GetSessionStatsOverlay.) */
TEST_F(St20NewApiRxTest, StatsGetVsIoStatsPassthrough) {
  ASSERT_EQ(inject_complete(1000), 0);
  ASSERT_EQ(inject_corrupted(2000), 0);
  ASSERT_EQ(inject_complete(3000), 0);
  for (int i = 0; i < 3; i++) {
    mtl_buffer_t* b = get_buffer();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(put_buffer(b), 0);
  }
  /* fill + one overflow */
  ASSERT_EQ(inject_complete(4000), 0);
  ASSERT_EQ(inject_complete(5000), 0);
  ASSERT_EQ(inject_complete(6000), 0);
  EXPECT_EQ(inject_complete(7000), -ENOSPC);

  mtl_session_stats_t abstract{};
  ASSERT_EQ(ut20rx_stats_get(ctx_, &abstract), 0);
  EXPECT_EQ(abstract.buffers_processed, buffers_processed());
  EXPECT_EQ(abstract.buffers_dropped, buffers_dropped());

  struct st20_rx_user_stats io {};
  ASSERT_EQ(ut20rx_io_stats_get(ctx_, &io), 0);
  EXPECT_EQ(io.common.stat_frames_received, 0u)
      << "io_stats_get is transport passthrough, not an overlay of the "
         "abstract counters";
}

/* Invariant: buffers_dropped advances monotonically, exactly once per
 * ready_ring overflow. Replaces the pipeline's BusyEqualsDroppedInvariant —
 * the unified API has no busy atomic, so the 1:1 relationship is between the
 * overflow event and buffers_dropped itself. */
TEST_F(St20NewApiRxTest, DroppedMonotonicWithOverflow) {
  /* fill */
  for (int i = 0; i < 3; i++) ASSERT_EQ(inject_complete(1000 + i), 0);
  /* overflow burst */
  for (uint64_t i = 0; i < 5; i++) {
    EXPECT_EQ(inject_complete(2000 + i), -ENOSPC);
    EXPECT_EQ(buffers_dropped(), i + 1)
        << "after overflow " << (i + 1)
        << ": buffers_dropped must bump exactly once per event";
  }
}
