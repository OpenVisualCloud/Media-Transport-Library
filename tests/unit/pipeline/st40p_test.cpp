/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p (ancillary) pipeline-layer zero-copy dispatch + release-wiring tests.
 *
 *   stat_frames_received / stat_frames_corrupted -> bumped in
 *     rx_st40p_frame_ready() (transport delivery), not in get_frame().
 *   stat_frames_dropped / stat_busy -> bump 1:1 in rx_st40p_frame_ready()
 *     when no free framebuf is available (back-pressure).
 *   put_frame / put_frame_abort -> release the transport-owned UDW slot via
 *     st40_rx_put_framebuff() and clear frame_info->udw_buff_addr.
 */

#include <gtest/gtest.h>

#include "pipeline/st40p_harness.h"

class St40PipelineRxTest : public ::testing::Test {
 protected:
  ut40p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    ctx_ = ut40p_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int inject(void* addr, uint32_t ts) {
    return ut40p_inject_frame(ctx_, addr, ST_FRAME_STATUS_COMPLETE, false, ts);
  }
  int inject_discont(void* addr, uint32_t ts) {
    return ut40p_inject_frame(ctx_, addr, ST_FRAME_STATUS_CORRUPTED, true, ts);
  }
  struct st40_frame_info* get_frame() {
    return ut40p_get_frame(ctx_);
  }
  int put_frame(struct st40_frame_info* f) {
    return ut40p_put_frame(ctx_, f);
  }
  int put_frame_abort(struct st40_frame_info* f) {
    return ut40p_put_frame_abort(ctx_, f);
  }
  uint64_t frames_received() {
    return ut40p_stat_frames_received(ctx_);
  }
  uint64_t frames_dropped() {
    return ut40p_stat_frames_dropped(ctx_);
  }
  uint64_t frames_corrupted() {
    return ut40p_stat_frames_corrupted(ctx_);
  }
  uint32_t stat_busy() {
    return ut40p_stat_busy(ctx_);
  }
};

/* Unlike ST30p, ST40p counts a frame as "received" at transport-delivery
 * time (rx_st40p_frame_ready), not when the app calls get_frame. */
TEST_F(St40PipelineRxTest, FramesReceivedOnFrameReadyNotGetFrame) {
  static uint8_t slot0, slot1;

  ASSERT_EQ(inject(&slot0, 1000), 0);
  EXPECT_EQ(frames_received(), 1u)
      << "ST40p bumps stat_frames_received at frame_ready, not get_frame";

  ASSERT_EQ(inject(&slot1, 2000), 0);
  EXPECT_EQ(frames_received(), 2u);

  struct st40_frame_info* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(frames_received(), 2u) << "get_frame must not bump stat_frames_received";
  EXPECT_EQ(put_frame(f), 0);
}

/* seq_discont on the delivered meta bumps stat_frames_corrupted at
 * frame_ready time; the frame is still delivered (best-effort). */
TEST_F(St40PipelineRxTest, SeqDiscontFrameBumpsCorruptedStat) {
  static uint8_t slot0;

  ASSERT_EQ(inject_discont(&slot0, 1000), 0);
  EXPECT_EQ(frames_received(), 1u) << "corrupted frame is still delivered";
  EXPECT_EQ(frames_corrupted(), 1u);

  struct st40_frame_info* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->status, ST_FRAME_STATUS_CORRUPTED);
  EXPECT_EQ(put_frame(f), 0);
}

/* When all framebufs are full, every further frame_ready bumps
 * stat_frames_dropped and stat_busy 1:1, and returns -EBUSY so the
 * transport reclaims its slot. */
TEST_F(St40PipelineRxTest, FramesDroppedWhenFramebufsFull) {
  static uint8_t slot0, slot1, slot2, slot3, slot4;

  ASSERT_EQ(inject(&slot0, 1000), 0);
  ASSERT_EQ(inject(&slot1, 2000), 0);
  ASSERT_EQ(inject(&slot2, 3000), 0);
  EXPECT_EQ(frames_dropped(), 0u);
  EXPECT_EQ(stat_busy(), 0u);

  EXPECT_EQ(inject(&slot3, 4000), -EBUSY);
  EXPECT_EQ(inject(&slot4, 5000), -EBUSY);

  EXPECT_EQ(frames_dropped(), 2u);
  EXPECT_EQ(stat_busy(), 2u) << "stat_busy must bump 1:1 with stat_frames_dropped";
  EXPECT_EQ(frames_received(), 3u) << "dropped frames must not count as received";
}

/* put_frame must call st40_rx_put_framebuff() with the frame's transport
 * addr exactly once, and clear udw_buff_addr afterward. */
TEST_F(St40PipelineRxTest, PutFrameReleasesTransportSlotAndClearsAddr) {
  static uint8_t slot0;
  ut40p_put_framebuff_reset_spy();

  ASSERT_EQ(inject(&slot0, 1000), 0);
  struct st40_frame_info* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->udw_buff_addr, &slot0);

  EXPECT_EQ(put_frame(f), 0);
  EXPECT_EQ(ut40p_put_framebuff_call_count(), 1);
  EXPECT_EQ(ut40p_put_framebuff_last_addr(), &slot0);
  EXPECT_EQ(f->udw_buff_addr, nullptr)
      << "put_frame must clear udw_buff_addr after releasing the slot";
}

/* put_frame_abort releases the same way but skips the meta_num/stats reset
 * that put_frame performs (frame is discarded, not processed). */
TEST_F(St40PipelineRxTest, PutFrameAbortReleasesTransportSlotAndClearsAddr) {
  static uint8_t slot0;
  ut40p_put_framebuff_reset_spy();

  ASSERT_EQ(inject(&slot0, 1000), 0);
  struct st40_frame_info* f = get_frame();
  ASSERT_NE(f, nullptr);

  EXPECT_EQ(put_frame_abort(f), 0);
  EXPECT_EQ(ut40p_put_framebuff_call_count(), 1);
  EXPECT_EQ(ut40p_put_framebuff_last_addr(), &slot0);
  EXPECT_EQ(f->udw_buff_addr, nullptr);
}

/* reset_session_stats clears every cumulative pipeline counter. */
TEST_F(St40PipelineRxTest, ResetClearsAllPipelineCounters) {
  static uint8_t slot0, slot1, slot2, slot3, slot4;

  ASSERT_EQ(inject(&slot0, 1000), 0);
  struct st40_frame_info* f = get_frame();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(put_frame(f), 0);

  ASSERT_EQ(inject(&slot1, 2000), 0);
  ASSERT_EQ(inject(&slot2, 3000), 0);
  ASSERT_EQ(inject(&slot3, 4000), 0);
  EXPECT_EQ(inject(&slot4, 5000), -EBUSY);

  ASSERT_GT(frames_received(), 0u);
  ASSERT_GT(frames_dropped(), 0u);

  EXPECT_EQ(ut40p_reset_session_stats(ctx_), 0);

  EXPECT_EQ(frames_received(), 0u);
  EXPECT_EQ(frames_dropped(), 0u);
}
