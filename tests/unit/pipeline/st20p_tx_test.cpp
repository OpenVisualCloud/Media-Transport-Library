/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) pipeline-layer TX contract tests.
 *
 * Old-API mirror of tests/unit/new_api/st20_tx_test.cpp. Drives the pipeline
 * TX state machine FREE -> IN_USER -> CONVERTED -> IN_TRANSMITTING -> FREE
 * through st20p_tx_get_frame / put_frame and the tx_st20p_next_frame /
 * frame_done callbacks. The two suites are parallel: the new-API DropWhenLate*
 * cases pin the replicated late-drop logic in video_tx_get_next_frame; these
 * pin the original tx_st20p_if_frame_late they were modelled on.
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_tx_harness.h"

/* P59_94: one frame period is ~16.683 ms. */
static constexpr uint64_t kFrameTai = 1000000000ULL;
static constexpr uint64_t kPeriodNs = 16683350ULL; /* ~1e9 / 59.94 */

/* enum st20p_tx_frame_status mirror (header is pipeline-internal). */
enum {
  kFree = 0,
  kReady = 1,
  kInConverting = 2,
  kConverted = 3,
  kDropped = 4,
  kInUser = 5,
  kInTransmitting = 6,
};

class St20PipelineTxTest : public ::testing::Test {
 protected:
  ut20ptx_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20ptx_init(), 0) << "EAL init failed";
    ctx_ = ut20ptx_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20ptx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  struct st_frame* get() {
    return ut20ptx_get_frame(ctx_);
  }
  int put(struct st_frame* f) {
    return ut20ptx_put_frame(ctx_, f);
  }
  uint64_t dropped() {
    return ut20ptx_stat_frames_dropped(ctx_);
  }
  uint64_t sent() {
    return ut20ptx_stat_frames_sent(ctx_);
  }
  int stat(uint16_t i) {
    return ut20ptx_frame_stat(ctx_, i);
  }

  void put_stamped_frame(uint64_t tai) {
    struct st_frame* f = get();
    ASSERT_NE(f, nullptr);
    f->tfmt = ST10_TIMESTAMP_FMT_TAI;
    f->timestamp = tai;
    ASSERT_EQ(put(f), 0);
  }
};

/* DROP_WHEN_LATE + USER_PACING: a CONVERTED frame a full period in the past is
 * dropped by tx_st20p_next_frame (recycled, not transmitted). stat_frames_dropped
 * bumps once and notify_frame_late fires. */
TEST_F(St20PipelineTxTest, DropWhenLateDropsLateFrame) {
  ut20ptx_set_drop_when_late(ctx_, true);
  ut20ptx_set_user_pacing(ctx_, true);
  ut20ptx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(stat(0), kConverted);

  ut20ptx_set_ptp_now(ctx_, kFrameTai + 2 * kPeriodNs);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), -EBUSY)
      << "a late frame must not be selected for transmission";

  EXPECT_EQ(stat(0), kFree) << "dropped frame must be recycled to FREE";
  EXPECT_EQ(dropped(), 1u);
  EXPECT_EQ(ut20ptx_notify_late_cnt(ctx_), 1u) << "notify_frame_late must fire";
}

/* On-time CONVERTED frame is selected and transmitted normally. */
TEST_F(St20PipelineTxTest, DropWhenLateKeepsOnTimeFrame) {
  ut20ptx_set_drop_when_late(ctx_, true);
  ut20ptx_set_user_pacing(ctx_, true);
  ut20ptx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(stat(0), kConverted);

  ut20ptx_set_ptp_now(ctx_, kFrameTai + kPeriodNs / 2);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), 0);
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(stat(0), kInTransmitting);
  EXPECT_EQ(dropped(), 0u);
  EXPECT_EQ(ut20ptx_notify_late_cnt(ctx_), 0u);
}

/* DROP_WHEN_LATE set but USER_PACING absent: late check is skipped (both flags
 * required), so the late frame is still transmitted. */
TEST_F(St20PipelineTxTest, DropWhenLateIgnoredWithoutUserPacing) {
  ut20ptx_set_drop_when_late(ctx_, true);
  ut20ptx_set_user_pacing(ctx_, false);
  ut20ptx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(stat(0), kConverted);

  ut20ptx_set_ptp_now(ctx_, kFrameTai + 100 * kPeriodNs);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), 0)
      << "without USER_PACING the late frame must still be transmitted";
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(stat(0), kInTransmitting);
  EXPECT_EQ(dropped(), 0u);
}

/* Full FREE -> IN_USER -> CONVERTED -> IN_TRANSMITTING -> FREE walk.
 * stat_frames_sent bumps on frame_done (transmission complete). */
TEST_F(St20PipelineTxTest, FrameLifecycleGetPutTransmit) {
  struct st_frame* f = get();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(stat(0), kInUser);
  EXPECT_EQ(sent(), 0u);

  EXPECT_EQ(put(f), 0);
  EXPECT_EQ(stat(0), kConverted);

  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), 0);
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(stat(0), kInTransmitting);
  EXPECT_EQ(sent(), 0u) << "not yet done — still on the wire";

  ASSERT_EQ(ut20ptx_frame_done(ctx_, 0), 0);
  EXPECT_EQ(stat(0), kFree);
  EXPECT_EQ(sent(), 1u) << "stat_frames_sent bumps on frame_done";
}

/* put_frame copies the app's user_meta into the framebuff, and next_frame
 * threads it into the st20_tx_frame_meta handed to the transport. */
TEST_F(St20PipelineTxTest, UserMetaPassthroughOnPut) {
  static const uint8_t blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};

  ut20ptx_set_user_pacing(ctx_, true);

  struct st_frame* f = get();
  ASSERT_NE(f, nullptr);
  f->user_meta = blob;
  f->user_meta_size = sizeof(blob);
  ASSERT_EQ(put(f), 0);

  uint16_t idx = 0xffff;
  struct st20_tx_frame_meta meta {};
  ASSERT_EQ(ut20ptx_next_frame(ctx_, &idx, &meta), 0);
  ASSERT_NE(meta.user_meta, nullptr);
  EXPECT_EQ(meta.user_meta_size, sizeof(blob));
  EXPECT_EQ(memcmp(meta.user_meta, blob, sizeof(blob)), 0)
      << "user_meta must survive put -> next_frame intact";
}

/* In derive mode (input fmt == transport fmt) put_frame takes the no-convert
 * branch straight to CONVERTED — the converter is never invoked. (The copy
 * lives on the internal_converter / !derive branch.) */
TEST_F(St20PipelineTxTest, ConvertOnPutAppToTransport) {
  struct st_frame* f = get();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(stat(0), kInUser);

  ASSERT_EQ(put(f), 0);
  EXPECT_EQ(stat(0), kConverted)
      << "derive mode: put goes straight to CONVERTED with no conversion copy";
}

/* Pipeline get_session_stats OVERLAYS its frame counters onto the transport
 * stats — the opposite of the new API, where stats_get and io_stats_get are
 * independent surfaces. After sending 3 frames the overlay reports 3 sent. */
TEST_F(St20PipelineTxTest, StatsGetVsIoStatsPassthrough) {
  for (int i = 0; i < 3; i++) {
    struct st_frame* f = get();
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(put(f), 0);
    uint16_t idx = 0xffff;
    ASSERT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), 0);
    ASSERT_EQ(ut20ptx_frame_done(ctx_, idx), 0);
  }
  EXPECT_EQ(sent(), 3u);

  struct st20_tx_user_stats stats {};
  ASSERT_EQ(ut20ptx_get_session_stats(ctx_, &stats), 0);
  EXPECT_EQ(stats.common.stat_frames_sent, 3u)
      << "pipeline overlays stat_frames_sent onto the transport stats";
}

/* get_frame with block_get=false returns NULL once every framebuffer is
 * claimed (IN_USER) — it never blocks. Releasing one frees a slot again. */
TEST_F(St20PipelineTxTest, BufferGetTimeoutSemantics) {
  struct st_frame* held[3];
  for (int i = 0; i < 3; i++) {
    held[i] = get();
    ASSERT_NE(held[i], nullptr);
  }
  EXPECT_EQ(get(), nullptr) << "non-blocking get must return NULL when full";

  ASSERT_EQ(put(held[0]), 0);
  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20ptx_next_frame(ctx_, &idx, nullptr), 0);
  ASSERT_EQ(ut20ptx_frame_done(ctx_, idx), 0);
  EXPECT_NE(get(), nullptr) << "a freed slot is claimable again";
}

/* Every requested framebuffer is an independently claimable slot. */
TEST_F(St20PipelineTxTest, FramebuffCntNoTxClamp) {
  ut20ptx_ctx_destroy(ctx_);
  ctx_ = ut20ptx_ctx_create(/*framebuff_cnt=*/8);
  ASSERT_NE(ctx_, nullptr);

  for (int i = 0; i < 8; i++) {
    struct st_frame* f = get();
    ASSERT_NE(f, nullptr) << "frame " << i << " must be claimable";
    EXPECT_EQ(stat((uint16_t)i), kInUser);
  }
  EXPECT_EQ(get(), nullptr) << "exactly 8 slots, no more";
}
