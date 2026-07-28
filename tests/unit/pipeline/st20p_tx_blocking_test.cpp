/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) TX pipeline blocking get_frame() wake-loss regression test.
 *
 * In ST20P_TX_FLAG_BLOCK_GET mode, st20p_tx_get_frame() blocks on a condition
 * variable when no FREE slot is available and is woken by tx_st20p_block_wake()
 * (fired from frame_done, the public wake API, and the destroy hook).
 *
 * The producer of FREE slots (the transport frame_done callback) runs on a
 * different thread than the app consumer, so a wake can be signalled in the
 * window after the consumer's claim fails but before it enters
 * pthread_cond_timedwait. pthread_cond_signal only wakes threads already
 * waiting, so that wake is lost: a single-shot "if (!destroying) timedwait()"
 * consumer then sleeps the entire block timeout despite the wake, and a
 * spurious wakeup lets it return NULL before the timeout elapses.
 *
 * The fix records the wake in a sticky block_wake_pending flag and re-checks it
 * in a predicate loop, so a wake posted before the wait is observed
 * immediately. This test posts the wake while no consumer is waiting (the exact
 * lost-wake window) and asserts the next blocking get_frame() returns promptly
 * instead of sleeping the full timeout.
 */

#include <gtest/gtest.h>

#include <chrono>

#include "pipeline/st20p_tx_harness.h"

TEST(St20PipelineTxBlocking, WakePostedBeforeWaitIsNotLost) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 2;
  /* Long enough that a lost wake (full-timeout block) is unmistakably distinct
   * from the correct near-instant return, yet short enough to bound the suite
   * if the bug regresses. */
  constexpr uint64_t kBlockTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut20p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  /* Drain every FREE slot so the next get_frame() must enter the blocking wait
   * path (its initial claim finds nothing). These claims return immediately
   * because slots are FREE. */
  struct st_frame* held[kFrameCnt];
  for (int i = 0; i < kFrameCnt; i++) {
    held[i] = ut20p_tx_get_frame(ctx);
    ASSERT_NE(held[i], nullptr) << "initial claim of FREE slot " << i << " failed";
  }

  /* Post a wake while no consumer is waiting yet -- exactly the lost-wake
   * window a real frame_done producer hits. A correct implementation records it
   * (block_wake_pending) so the very next blocking get_frame observes it and
   * skips the sleep; the buggy single-shot wait discards the signal and blocks
   * for the full kBlockTimeoutNs. */
  ut20p_tx_wake_block(ctx);

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut20p_tx_get_frame(ctx); /* no FREE slot -> wait path */
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* No slot was actually freed, so the claim still fails -- what matters is that
   * it failed FAST, proving the pre-posted wake was not lost. */
  EXPECT_EQ(frame, nullptr) << "no slot was freed; get_frame must return NULL";
  EXPECT_LT(elapsed, std::chrono::nanoseconds(kBlockTimeoutNs) / 4)
      << "blocking get_frame slept ~full timeout: the pre-posted wake was lost";

  for (int i = 0; i < kFrameCnt; i++)
    ASSERT_EQ(ut20p_tx_put_frame_abort(ctx, held[i]), 0);
  ut20p_tx_ctx_destroy(ctx);
}
