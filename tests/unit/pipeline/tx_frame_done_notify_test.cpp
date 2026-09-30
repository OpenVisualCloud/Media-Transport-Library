/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Every frame the app puts must get its own notify_frame_done, also when
 * the app gets the freed slot again while the previous frame_done is still
 * running. Here the app does that from inside the callback, the widest form
 * of the race between the tasklet and an app thread.
 */

#include <gtest/gtest.h>

#include "pipeline/st22p_tx_harness.h"
#include "pipeline/st30p_tx_harness.h"
#include "pipeline/st40p_tx_harness.h"

namespace {

template <typename Ctx, typename Frame>
struct DoneSpy {
  Ctx* ctx = nullptr;
  Frame* (*get_frame)(Ctx*) = nullptr;
  int (*put_frame)(Ctx*, Frame*) = nullptr;
  int calls = 0;
  bool refilled = false;
};

/* On the first done, get the slot again and put it back if it is free. */
template <typename Ctx, typename Frame>
int OnFrameDone(void* priv, Frame* frame) {
  auto* spy = static_cast<DoneSpy<Ctx, Frame>*>(priv);
  (void)frame;
  if (++spy->calls == 1) {
    Frame* next = spy->get_frame(spy->ctx);
    if (next) spy->refilled = spy->put_frame(spy->ctx, next) == 0;
  }
  return 0;
}

template <typename Ctx, typename Frame>
void ExpectDonePerFrame(Ctx* ctx, DoneSpy<Ctx, Frame>* spy,
                        int (*next_frame)(Ctx*, uint16_t*),
                        int (*frame_done)(Ctx*, uint16_t)) {
  Frame* frame = spy->get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(spy->put_frame(ctx, frame), 0);
  uint16_t idx;
  ASSERT_EQ(next_frame(ctx, &idx), 0);
  ASSERT_EQ(frame_done(ctx, idx), 0);
  ASSERT_EQ(spy->calls, 1);

  if (!spy->refilled) {
    frame = spy->get_frame(ctx);
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(spy->put_frame(ctx, frame), 0);
  }
  ASSERT_EQ(next_frame(ctx, &idx), 0);
  ASSERT_EQ(frame_done(ctx, idx), 0);

  EXPECT_EQ(spy->calls, 2) << "the second frame got no notify_frame_done";
}

}  // namespace

TEST(PipelineTxFrameDone, St22pNotifiesEveryFrame) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";
  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  DoneSpy<ut22p_tx_ctx, struct st_frame> spy;
  spy.ctx = ctx;
  spy.get_frame = ut22p_tx_get_frame;
  spy.put_frame = ut22p_tx_put_frame;
  ut22p_tx_set_notify_frame_done(ctx, OnFrameDone<ut22p_tx_ctx, struct st_frame>, &spy);

  ExpectDonePerFrame(ctx, &spy, ut22p_tx_next_frame, ut22p_tx_frame_done);

  ut22p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxFrameDone, St30pNotifiesEveryFrame) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  DoneSpy<ut30p_tx_ctx, struct st30_frame> spy;
  spy.ctx = ctx;
  spy.get_frame = ut30p_tx_get_frame;
  spy.put_frame = ut30p_tx_put_frame;
  ut30p_tx_set_notify_frame_done(ctx, OnFrameDone<ut30p_tx_ctx, struct st30_frame>, &spy);

  ExpectDonePerFrame(ctx, &spy, ut30p_tx_next_frame, ut30p_tx_frame_done);

  ut30p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxFrameDone, St40pNotifiesEveryFrame) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  DoneSpy<ut40p_tx_ctx, struct st40_frame_info> spy;
  spy.ctx = ctx;
  spy.get_frame = ut40p_tx_get_frame;
  spy.put_frame = ut40p_tx_put_frame;
  ut40p_tx_set_notify_frame_done(ctx, OnFrameDone<ut40p_tx_ctx, struct st40_frame_info>,
                                 &spy);

  ExpectDonePerFrame(ctx, &spy, ut40p_tx_next_frame, ut40p_tx_frame_done);

  ut40p_tx_ctx_destroy(ctx);
}
