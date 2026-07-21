/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p (compressed video) TX ST22P_TX_FLAG_DROP_WHEN_LATE regression test.
 *
 * tx_st22p_if_frame_late() only fires when both ST22P_TX_FLAG_DROP_WHEN_LATE
 * and ST22P_TX_FLAG_USER_PACING are set and the newest ENCODED frame carries a
 * TAI timestamp already past its one-frame-period TX window. On a hit it
 * drives ENCODED -> DROPPED -> FREE, firing notify_frame_done(DROPPED) once
 * and then notify_frame_late(), before tx_st22p_next_frame() reports -EBUSY.
 */

#include <gtest/gtest.h>

#include "pipeline/st22p_tx_harness.h"

namespace {

constexpr uint64_t kBaseTaiNs = 1000000000ULL;        /* 1s */
constexpr uint64_t kFramePeriodNs25Fps = 40000000ULL; /* NS_PER_S / 25 */

struct CallbackCtx {
  int done_calls = 0;
  enum st_frame_status last_done_status = ST_FRAME_STATUS_MAX;
  int late_calls = 0;
};

int OnFrameDone(void* priv, struct st_frame* frame) {
  auto* cb = static_cast<CallbackCtx*>(priv);
  cb->done_calls++;
  cb->last_done_status = frame->status;
  return 0;
}

int OnFrameLate(void* priv, uint64_t epoch_skipped) {
  auto* cb = static_cast<CallbackCtx*>(priv);
  (void)epoch_skipped;
  cb->late_calls++;
  return 0;
}

}  // namespace

TEST(St22PipelineTxDropWhenLate, LateFrameIsDroppedAndFreed) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_set_flags(ctx, ST22P_TX_FLAG_DROP_WHEN_LATE | ST22P_TX_FLAG_USER_PACING);
  ut22p_tx_set_fps(ctx, ST_FPS_P25);

  CallbackCtx cb_ctx;
  ut22p_tx_set_notify_frame_done(ctx, OnFrameDone, &cb_ctx);
  ut22p_tx_set_notify_frame_late(ctx, OnFrameLate, &cb_ctx);

  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
  frame->timestamp = kBaseTaiNs;
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frame), 0);
  int idx0 = ut22p_tx_frame_idx(frame);

  /* mock wall clock already past the one-period TX window. */
  ut22p_tx_set_ptp_ns(ctx, kBaseTaiNs + kFramePeriodNs25Fps);

  uint16_t idx;
  EXPECT_EQ(ut22p_tx_next_frame(ctx, &idx), -EBUSY)
      << "dropped frame leaves nothing to transmit";
  EXPECT_EQ(ut22p_tx_frame_stat(ctx, idx0), 0 /* FREE */);
  EXPECT_EQ(cb_ctx.done_calls, 1);
  EXPECT_EQ(cb_ctx.last_done_status, ST_FRAME_STATUS_DROPPED);
  EXPECT_EQ(cb_ctx.late_calls, 1);

  ut22p_tx_ctx_destroy(ctx);
}

TEST(St22PipelineTxDropWhenLate, OnTimeFrameIsTransmittedNotDropped) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_set_flags(ctx, ST22P_TX_FLAG_DROP_WHEN_LATE | ST22P_TX_FLAG_USER_PACING);
  ut22p_tx_set_fps(ctx, ST_FPS_P25);

  CallbackCtx cb_ctx;
  ut22p_tx_set_notify_frame_done(ctx, OnFrameDone, &cb_ctx);
  ut22p_tx_set_notify_frame_late(ctx, OnFrameLate, &cb_ctx);

  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
  frame->timestamp = kBaseTaiNs;
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frame), 0);
  int idx0 = ut22p_tx_frame_idx(frame);

  /* mock wall clock still within the TX window. */
  ut22p_tx_set_ptp_ns(ctx, kBaseTaiNs + kFramePeriodNs25Fps - 1);

  uint16_t idx;
  EXPECT_EQ(ut22p_tx_next_frame(ctx, &idx), 0);
  EXPECT_EQ(idx, idx0);
  EXPECT_EQ(ut22p_tx_frame_stat(ctx, idx0), 6 /* IN_TRANSMITTING */);
  EXPECT_EQ(cb_ctx.late_calls, 0);

  ut22p_tx_ctx_destroy(ctx);
}

TEST(St22PipelineTxDropWhenLate, MissingUserPacingFlagDoesNotDrop) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_set_flags(ctx, ST22P_TX_FLAG_DROP_WHEN_LATE); /* no USER_PACING */
  ut22p_tx_set_fps(ctx, ST_FPS_P25);

  CallbackCtx cb_ctx;
  ut22p_tx_set_notify_frame_done(ctx, OnFrameDone, &cb_ctx);
  ut22p_tx_set_notify_frame_late(ctx, OnFrameLate, &cb_ctx);

  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
  frame->timestamp = kBaseTaiNs;
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frame), 0);
  int idx0 = ut22p_tx_frame_idx(frame);

  /* would be late if the gate were satisfied. */
  ut22p_tx_set_ptp_ns(ctx, kBaseTaiNs + kFramePeriodNs25Fps);

  uint16_t idx;
  EXPECT_EQ(ut22p_tx_next_frame(ctx, &idx), 0);
  EXPECT_EQ(idx, idx0);
  EXPECT_EQ(ut22p_tx_frame_stat(ctx, idx0), 6 /* IN_TRANSMITTING */);
  EXPECT_EQ(cb_ctx.late_calls, 0);

  ut22p_tx_ctx_destroy(ctx);
}

TEST(St22PipelineTxDropWhenLate, MissingDropWhenLateFlagDoesNotDrop) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_set_flags(ctx, ST22P_TX_FLAG_USER_PACING); /* no DROP_WHEN_LATE */
  ut22p_tx_set_fps(ctx, ST_FPS_P25);

  CallbackCtx cb_ctx;
  ut22p_tx_set_notify_frame_done(ctx, OnFrameDone, &cb_ctx);
  ut22p_tx_set_notify_frame_late(ctx, OnFrameLate, &cb_ctx);

  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  frame->tfmt = ST10_TIMESTAMP_FMT_TAI;
  frame->timestamp = kBaseTaiNs;
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frame), 0);
  int idx0 = ut22p_tx_frame_idx(frame);

  ut22p_tx_set_ptp_ns(ctx, kBaseTaiNs + kFramePeriodNs25Fps);

  uint16_t idx;
  EXPECT_EQ(ut22p_tx_next_frame(ctx, &idx), 0);
  EXPECT_EQ(idx, idx0);
  EXPECT_EQ(ut22p_tx_frame_stat(ctx, idx0), 6 /* IN_TRANSMITTING */);
  EXPECT_EQ(cb_ctx.late_calls, 0);

  ut22p_tx_ctx_destroy(ctx);
}
