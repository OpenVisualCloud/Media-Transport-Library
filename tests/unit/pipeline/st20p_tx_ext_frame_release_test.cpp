/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) TX EXT_FRAME_MANUAL_RELEASE notify_frame_done regression test.
 *
 * tx_st20p_frame_done() transitions a completed frame IN_TRANSMITTING->IN_USER
 * *before* invoking ctx->ops.notify_frame_done(), specifically so the app can
 * call st20p_tx_notify_ext_frame_free() (IN_USER->FREE) from inside that
 * callback. Only after the callback returns does it write
 * framebuff->frame_done_cb_called = true -- a plain, non-atomic bool, unlike
 * every other cross-thread-shared field on the framebuff, which uses C11
 * atomics.
 *
 * A natural producer immediately pulls the next frame once its slot is freed
 * (e.g. woken by the release, or -- as here -- from directly inside the same
 * notify_frame_done call). get_frame() claims the just-freed slot and resets
 * frame_done_cb_called to false for the new cycle. That reset lands *before*
 * the still-executing outer frame_done() writes frame_done_cb_called = true
 * for the *previous* cycle, clobbering the new cycle's flag back to true.
 * When the new cycle's own frame_done() runs later, its guard
 * (!frame_done_cb_called) is now false, so notify_frame_done is silently
 * skipped for a frame that genuinely completed transmission -- the app is
 * never told to release its external buffer.
 */

#include <gtest/gtest.h>

#include "pipeline/st20p_tx_harness.h"

namespace {

struct CallbackCtx {
  ut20p_tx_ctx* tx_ctx;
  int call_count = 0;
  struct st_frame* next_cycle_frame = nullptr;
};

int OnFrameDone(void* priv, struct st_frame* frame) {
  auto* cb = static_cast<CallbackCtx*>(priv);
  cb->call_count++;

  /* release the ext buffer, exactly as EXT_FRAME_MANUAL_RELEASE intends. */
  ut20p_tx_notify_ext_frame_free(cb->tx_ctx, ut20p_tx_frame_idx(frame));

  if (cb->call_count == 1) {
    /* pull the next frame immediately, as a tight producer loop would once
     * woken by the release above. */
    cb->next_cycle_frame = ut20p_tx_get_frame(cb->tx_ctx);
  }

  return 0;
}

}  // namespace

TEST(St20PipelineTxExtFrameRelease, DoneNotSkippedAcrossReusedSlot) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";

  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_tx_ctx_set_manual_release(ctx);

  CallbackCtx cb_ctx;
  cb_ctx.tx_ctx = ctx;
  ut20p_tx_set_notify_frame_done(ctx, OnFrameDone, &cb_ctx);

  /* cycle 1: get -> put -> next -> done. done's callback releases the slot
   * and immediately claims it again for cycle 2. */
  struct st_frame* frame1 = ut20p_tx_get_frame(ctx);
  ASSERT_NE(frame1, nullptr);
  ASSERT_EQ(ut20p_tx_put_frame(ctx, frame1), 0);
  uint16_t idx;
  ASSERT_EQ(ut20p_tx_next_frame(ctx, &idx), 0);
  ASSERT_EQ(ut20p_tx_frame_done(ctx, idx), 0);

  ASSERT_EQ(cb_ctx.call_count, 1) << "cycle 1 must notify exactly once";
  ASSERT_NE(cb_ctx.next_cycle_frame, nullptr)
      << "cycle 2 should have claimed the slot cycle 1's callback just freed";

  /* cycle 2: put -> next -> done, using the frame claimed from inside cycle
   * 1's callback. This is a genuinely new, fully-transmitted frame and must
   * get its own notify_frame_done call. */
  struct st_frame* frame2 = cb_ctx.next_cycle_frame;
  ASSERT_EQ(ut20p_tx_put_frame(ctx, frame2), 0);
  ASSERT_EQ(ut20p_tx_next_frame(ctx, &idx), 0);
  ASSERT_EQ(ut20p_tx_frame_done(ctx, idx), 0);

  EXPECT_EQ(cb_ctx.call_count, 2)
      << "cycle 2 completed transmission but notify_frame_done was silently "
         "skipped: frame_done_cb_called was clobbered back to true by cycle "
         "1's post-callback write racing cycle 2's get_frame() reset";

  ut20p_tx_ctx_destroy(ctx);
}
