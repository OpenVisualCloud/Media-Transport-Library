/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20 NEW-API (unified session) TX contract tests.
 *
 * Parallel TX copy of tests/unit/new_api/st20_rx_test.cpp / the pipeline TX
 * suite. Drives the FREE -> APP_OWNED -> READY -> TRANSMITTING -> FREE frame
 * state machine directly through the production vtable callbacks.
 *
 * DropWhenLate* pin MTL_SESSION_FLAG_DROP_WHEN_LATE, which has no raw st20
 * equivalent and is implemented inside video_tx_get_next_frame: a READY frame
 * whose TAI timestamp has fallen more than one frame period behind the PTP
 * wall clock is recycled to FREE, counted in buffers_dropped, and surfaced via
 * an MTL_EVENT_FRAME_LATE event — mirroring pipeline tx_st20p_if_frame_late.
 */

#include <gtest/gtest.h>

#include "new_api/st20_tx_harness.h"

/* P59_94: one frame period is ~16.683 ms. A TAI base of 1 s keeps both the
 * "on time" and "late" wall clocks positive and unambiguous. */
static constexpr uint64_t kFrameTai = 1000000000ULL;
static constexpr uint64_t kPeriodNs = 16683350ULL; /* ~1e9 / 59.94 */

class St20NewApiTxTest : public ::testing::Test {
 protected:
  ut20tx_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20tx_init(), 0) << "EAL init failed";
    ctx_ = ut20tx_ctx_create(/*framebuff_cnt=*/3);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut20tx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  mtl_buffer_t* get() {
    return ut20tx_buffer_get(ctx_);
  }
  int put(mtl_buffer_t* b) {
    return ut20tx_buffer_put(ctx_, b);
  }
  uint64_t dropped() {
    return ut20tx_buffers_dropped(ctx_);
  }
  uint64_t processed() {
    return ut20tx_buffers_processed(ctx_);
  }
  int state(uint16_t i) {
    return ut20tx_frame_state(ctx_, i);
  }

  /* Put one READY frame stamped at kFrameTai, then arm drop-when-late. */
  void put_stamped_frame(uint64_t tai) {
    mtl_buffer_t* b = get();
    ASSERT_NE(b, nullptr);
    ut20tx_buffer_set_timestamp(b, tai);
    ASSERT_EQ(put(b), 0);
  }
};

/* enum tx_frame_state mirror (private to the .c). */
enum { kFree = 0, kAppOwned = 1, kReady = 2, kTransmitting = 3 };

/* DropWhenLate ON + USER_PACING: a READY frame whose TAI is a full period in
 * the past must NOT be selected. The slot returns to FREE, buffers_dropped
 * bumps once, and an MTL_EVENT_FRAME_LATE event is posted. */
TEST_F(St20NewApiTxTest, DropWhenLateDropsLateFrame) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(state(0), kReady);

  /* wall clock well past the frame's TX window (> one frame period late) */
  ut20tx_set_ptp_now(ctx_, kFrameTai + 2 * kPeriodNs);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), -EBUSY)
      << "a late frame must not be selected for transmission";

  EXPECT_EQ(state(0), kFree) << "dropped frame must be recycled to FREE";
  EXPECT_EQ(dropped(), 1u);
  EXPECT_EQ(processed(), 0u) << "a dropped frame is not transmitted";

  mtl_event_t ev;
  ASSERT_EQ(ut20tx_poll_event(ctx_, &ev), 0) << "a FRAME_LATE event must be posted";
  EXPECT_EQ(ev.type, MTL_EVENT_FRAME_LATE);
}

/* DropWhenLate ON + USER_PACING but the frame is still within its window:
 * it is selected and transmitted normally, nothing is dropped. */
TEST_F(St20NewApiTxTest, DropWhenLateKeepsOnTimeFrame) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(state(0), kReady);

  /* within one frame period — on time */
  ut20tx_set_ptp_now(ctx_, kFrameTai + kPeriodNs / 2);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 0u);

  mtl_event_t ev;
  EXPECT_EQ(ut20tx_poll_event(ctx_, &ev), -ETIMEDOUT) << "no late event expected";
}

/* DropWhenLate ON but USER_PACING absent: drop-when-late requires user pacing,
 * so the late check is skipped and the (late) frame is transmitted normally. */
TEST_F(St20NewApiTxTest, DropWhenLateIgnoredWithoutUserPacing) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, false);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);

  put_stamped_frame(kFrameTai);
  ASSERT_EQ(state(0), kReady);

  ut20tx_set_ptp_now(ctx_, kFrameTai + 100 * kPeriodNs);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0)
      << "without USER_PACING the late frame must still be transmitted";
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 0u);
}

/* DropWhenLate ON + USER_PACING but the frame was never stamped with a TAI
 * timestamp (timestamp==0/tfmt==0). The late gate must treat an unstamped slot
 * as not-late regardless of the wall clock, otherwise every unstamped frame
 * looks infinitely late and is silently dropped. */
TEST_F(St20NewApiTxTest, DropWhenLateIgnoresUnstampedFrame) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);

  /* put a READY frame WITHOUT stamping any TAI timestamp on it */
  mtl_buffer_t* b = get();
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(put(b), 0);
  ASSERT_EQ(state(0), kReady);

  /* wall clock far in the future: a timestamp==0 slot would look infinitely
   * late under a naive gate, but an unstamped frame must NOT be dropped. */
  ut20tx_set_ptp_now(ctx_, kFrameTai + 1000 * kPeriodNs);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0)
      << "an unstamped frame must be transmitted, not dropped";
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 0u);
}

/* DropWhenLate ON + USER_PACING, library-owned slot reuse: the low-level TX
 * pacing path stamps st20_frames[idx].tv_meta with a non-zero PAST cursor on
 * every transmitted frame (st_tx_video_session.c). When the app later reuses
 * that same slot with an UNSTAMPED buffer, the stale stamp must NOT be mistaken
 * for an app deadline — the frame must transmit, not drop. */
TEST_F(St20NewApiTxTest, DropWhenLateUnstampedSurvivesSlotReuse) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);

  /* First use: drive an unstamped frame through slot 0 to TRANSMITTING. */
  mtl_buffer_t* b = get();
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(put(b), 0);
  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
  ASSERT_EQ(idx, 0u);

  /* Mimic the low-level pacing path stamping tv_meta with a past TAI cursor. */
  ut20tx_frame_set_timestamp(ctx_, idx, kFrameTai);

  /* Transmission completes: the slot returns to FREE. */
  ASSERT_EQ(ut20tx_frame_done(ctx_, idx), 0);
  ASSERT_EQ(state(0), kFree);

  /* Reuse the slot with an UNSTAMPED buffer while the wall clock is far past
   * the stale stamp. */
  ut20tx_set_ptp_now(ctx_, kFrameTai + 1000 * kPeriodNs);
  mtl_buffer_t* b2 = get();
  ASSERT_NE(b2, nullptr);
  ASSERT_EQ(put(b2), 0);

  uint16_t idx2 = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx2), 0)
      << "a reused slot with an unstamped buffer must transmit, not drop on a "
         "stale low-level stamp";
  EXPECT_EQ(idx2, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 0u);
}

/* DropWhenLate ON + USER_PACING, USER_OWNED slot reuse after a completed
 * transmit. User-owned bind never runs tx_apply_buffer_metadata, so only
 * notify_frame_done (clear A) can drop the transmit-time pacing stamp. A slot
 * stamped during transmit must start unstamped when the app rebinds it with a
 * fresh buffer, otherwise the stale past stamp false-drops it. Reverting clear A
 * alone makes this fail — clear B cannot mask it. */
TEST_F(St20NewApiTxTest, DropWhenLateUserOwnedSurvivesSlotReuseAfterTransmit) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);
  ASSERT_EQ(ut20tx_set_user_owned(ctx_), 0);

  int data1 = 0, uctx1 = 0;
  ASSERT_EQ(ut20tx_post_user_buffer(ctx_, &data1, &uctx1), 0);

  /* First use: an unstamped user buffer binds slot 0 and transmits. */
  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
  ASSERT_EQ(idx, 0u);
  ASSERT_EQ(state(0), kTransmitting);

  /* Mimic the low-level pacing path stamping tv_meta with a past TAI cursor. */
  ut20tx_frame_set_timestamp(ctx_, idx, kFrameTai);

  /* Transmission completes: notify_frame_done (clear A) drops that stamp. */
  ASSERT_EQ(ut20tx_frame_done(ctx_, idx), 0);
  ASSERT_EQ(state(0), kFree);

  /* Rebind the same slot with a fresh unstamped buffer, wall clock far past. */
  ut20tx_set_ptp_now(ctx_, kFrameTai + 1000 * kPeriodNs);
  int data2 = 0, uctx2 = 0;
  ASSERT_EQ(ut20tx_post_user_buffer(ctx_, &data2, &uctx2), 0);

  uint16_t idx2 = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx2), 0)
      << "a user-owned reused slot must transmit, not drop on a stale "
         "transmit-time pacing stamp";
  EXPECT_EQ(idx2, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 0u);
}

/* DropWhenLate ON + USER_PACING, USER_OWNED slot reuse after a drop. When a
 * stamped buffer is dropped as late, the get_next_frame drop path itself
 * (clear C) must clear the slot's tv_meta, otherwise the next fresh buffer that
 * rebinds the slot inherits the stale past stamp and is false-dropped too.
 * Reverting clear C alone makes this fail. */
TEST_F(St20NewApiTxTest, DropWhenLateUserOwnedSurvivesSlotReuseAfterDrop) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);
  ASSERT_EQ(ut20tx_set_user_owned(ctx_), 0);

  /* Arm a drop: stamp slot 0 past TAI, wall clock far ahead. The bound buffer
   * inherits the slot stamp — user-owned never re-stamps tv_meta. */
  ut20tx_frame_set_timestamp(ctx_, 0, kFrameTai);
  ut20tx_set_ptp_now(ctx_, kFrameTai + 1000 * kPeriodNs);

  int data1 = 0, uctx1 = 0;
  ASSERT_EQ(ut20tx_post_user_buffer(ctx_, &data1, &uctx1), 0);
  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), -EBUSY)
      << "the stamped late user buffer must be dropped";
  EXPECT_EQ(state(0), kFree);
  EXPECT_EQ(dropped(), 1u);

  /* Rebind the same slot with a fresh unstamped buffer. */
  int data2 = 0, uctx2 = 0;
  ASSERT_EQ(ut20tx_post_user_buffer(ctx_, &data2, &uctx2), 0);
  uint16_t idx2 = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx2), 0)
      << "a user-owned reused slot must transmit, not drop on a stale "
         "drop-path pacing stamp";
  EXPECT_EQ(idx2, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(dropped(), 1u);
}

/* DropWhenLate ON + USER_PACING in MTL_BUFFER_USER_OWNED mode: when a posted
 * external buffer is dropped as late, the app must still reclaim it. The drop
 * path must deliver MTL_EVENT_BUFFER_DONE carrying the exact user_ctx and clear
 * the per-frame ctx slot, otherwise the external buffer leaks. */
TEST_F(St20NewApiTxTest, DropWhenLateUserOwnedReturnsBuffer) {
  ut20tx_set_drop_when_late(ctx_, true);
  ut20tx_set_user_pacing(ctx_, true);
  ut20tx_set_fps(ctx_, ST_FPS_P59_94);
  ASSERT_EQ(ut20tx_set_user_owned(ctx_), 0);

  int user_data = 0;
  void* user_ctx = &user_data;

  /* stamp slot 0 so the bound user buffer is eligible for the late check */
  ut20tx_frame_set_timestamp(ctx_, 0, kFrameTai);
  ut20tx_set_ptp_now(ctx_, kFrameTai + 2 * kPeriodNs);

  ASSERT_EQ(ut20tx_post_user_buffer(ctx_, &user_data, user_ctx), 0);

  uint16_t idx = 0xffff;
  EXPECT_EQ(ut20tx_get_next_frame(ctx_, &idx), -EBUSY)
      << "the late user buffer must be dropped, not transmitted";
  EXPECT_EQ(state(0), kFree);
  EXPECT_EQ(dropped(), 1u);

  bool got_done = false;
  mtl_event_t ev;
  while (ut20tx_poll_event(ctx_, &ev) == 0) {
    if (ev.type == MTL_EVENT_BUFFER_DONE) {
      EXPECT_EQ(ev.ctx, user_ctx);
      got_done = true;
    }
  }
  EXPECT_TRUE(got_done) << "user-owned drop must return the buffer to the app";
  EXPECT_EQ(ut20tx_user_buf_ctx(ctx_, 0), nullptr)
      << "the per-frame user_ctx slot must be cleared, not leaked";
}

/* Full FREE -> APP_OWNED -> READY -> TRANSMITTING -> FREE walk via the public
 * callbacks. buffers_processed bumps on frame_done (transmission complete),
 * not on get or put. */
TEST_F(St20NewApiTxTest, FrameLifecycleGetPutTransmit) {
  mtl_buffer_t* b = get();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(state(0), kAppOwned);
  EXPECT_EQ(processed(), 0u);

  EXPECT_EQ(put(b), 0);
  EXPECT_EQ(state(0), kReady);
  EXPECT_EQ(processed(), 0u);

  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
  EXPECT_EQ(idx, 0u);
  EXPECT_EQ(state(0), kTransmitting);
  EXPECT_EQ(processed(), 0u) << "not yet done — still on the wire";

  ASSERT_EQ(ut20tx_frame_done(ctx_, 0), 0);
  EXPECT_EQ(state(0), kFree);
  EXPECT_EQ(processed(), 1u) << "buffers_processed bumps on frame_done";
}

/* buffer_put threads the buffer's user_meta pointer/size into the frame slot's
 * tv_meta, so the transport sends it alongside the frame. */
TEST_F(St20NewApiTxTest, UserMetaPassthroughOnPut) {
  static uint8_t meta_blob[16] = {0xAB};

  mtl_buffer_t* b = get();
  ASSERT_NE(b, nullptr);
  ut20tx_buffer_set_user_meta(b, meta_blob, sizeof(meta_blob));
  ASSERT_EQ(put(b), 0);

  EXPECT_EQ(ut20tx_frame_user_meta(ctx_, 0), meta_blob)
      << "put must record the user_meta pointer on the frame's tv_meta";
}

/* In derive mode (transport fmt == app fmt) buffer_get hands the app the
 * transport framebuffer directly, so the app writes straight into the wire
 * buffer and put is a pure handoff with no copy. (The conversion copy lives on
 * the !derive branch of buffer_put.) */
TEST_F(St20NewApiTxTest, ConvertOnPutAppToTransport) {
  mtl_buffer_t* b = get();
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->data, ut20tx_frame_addr(ctx_, 0))
      << "derive mode: app buffer IS the transport framebuffer (zero-copy)";

  /* app fills the buffer; in derive mode this lands directly on the wire */
  memset(b->data, 0x5A, 16);
  ASSERT_EQ(put(b), 0);
  EXPECT_EQ(((uint8_t*)ut20tx_frame_addr(ctx_, 0))[0], 0x5A)
      << "no conversion copy in derive mode — data is already in place";
}

/* Two distinct stats surfaces: stats_get returns the abstract unified counters
 * the test drove; io_stats_get is a pure passthrough of the transport's own
 * (stubbed, zeroed) stats. They are independent, not an overlay. */
TEST_F(St20NewApiTxTest, StatsGetVsIoStatsPassthrough) {
  for (int i = 0; i < 3; i++) {
    mtl_buffer_t* b = get();
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(put(b), 0);
    uint16_t idx = 0xffff;
    ASSERT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
    ASSERT_EQ(ut20tx_frame_done(ctx_, idx), 0);
  }

  mtl_session_stats_t abstract{};
  ASSERT_EQ(ut20tx_stats_get(ctx_, &abstract), 0);
  EXPECT_EQ(abstract.buffers_processed, processed());
  EXPECT_EQ(abstract.buffers_processed, 3u);

  struct st20_tx_user_stats io {};
  ASSERT_EQ(ut20tx_io_stats_get(ctx_, &io), 0);
  EXPECT_EQ(io.common.stat_frames_sent, 0u)
      << "io_stats_get is transport passthrough, not an overlay of the "
         "abstract counters";
}

/* buffer_get with timeout_ms=0 returns NULL (-ETIMEDOUT) once every frame is
 * claimed — it never blocks and never invents a slot. */
TEST_F(St20NewApiTxTest, BufferGetTimeoutSemantics) {
  mtl_buffer_t* held[3];
  for (int i = 0; i < 3; i++) {
    held[i] = get();
    ASSERT_NE(held[i], nullptr);
  }
  /* all 3 frames are APP_OWNED — none FREE */
  EXPECT_EQ(get(), nullptr) << "non-blocking get must return NULL when full";

  /* releasing one (put + transmit + done) frees a slot again */
  ASSERT_EQ(put(held[0]), 0);
  uint16_t idx = 0xffff;
  ASSERT_EQ(ut20tx_get_next_frame(ctx_, &idx), 0);
  ASSERT_EQ(ut20tx_frame_done(ctx_, idx), 0);
  EXPECT_NE(get(), nullptr) << "a freed slot is claimable again";
}

/* The unified TX honors the requested framebuffer count with no max clamp:
 * every one of the N frames is an independently claimable slot. */
TEST_F(St20NewApiTxTest, FramebuffCntNoTxClamp) {
  ut20tx_ctx_destroy(ctx_);
  ctx_ = ut20tx_ctx_create(/*framebuff_cnt=*/8);
  ASSERT_NE(ctx_, nullptr);

  for (int i = 0; i < 8; i++) {
    mtl_buffer_t* b = get();
    ASSERT_NE(b, nullptr) << "frame " << i << " must be claimable (no clamp)";
    EXPECT_EQ(state((uint16_t)i), kAppOwned);
  }
  EXPECT_EQ(get(), nullptr) << "exactly 8 slots, no more";
}
