/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p (compressed video) TX pipeline encoder-plugin blocking-wait wake-token
 * regression tests for tx_st22p_encode_get_block_wait(), the wait
 * st22_encoder_get_frame() takes under ST22_ENCODER_RESP_FLAG_BLOCK_GET. It is
 * woken by tx_st22p_encode_block_wake() from put_frame, from the encoder's
 * wake_block callback, and from the destroy hook.
 *
 * The app thread that readies frames is not the encoder plugin thread, so a
 * wake can be signalled in the window after the plugin's claim fails but
 * before it enters pthread_cond_timedwait. pthread_cond_signal only wakes
 * threads already waiting, so a single-shot "timedwait()" waiter loses that
 * wake and sleeps the entire timeout; a spurious wakeup makes the same waiter
 * give up early.
 *
 * The fix records the wake in a sticky encode_block_wake_pending flag,
 * re-checks it in a predicate loop, and clears it on the way out. Both halves
 * need pinning and neither case pins both: WakePostedBeforeWaitIsNotLost only
 * fails if the waker stops recording the token (flag = true),
 * OneWakeSatisfiesOnlyOneWait only fails if the waiter stops spending it
 * (flag = false).
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>

#include "pipeline/st22p_tx_harness.h"

namespace {

/* Long enough that a lost wake (full-timeout block) is unmistakably distinct
 * from the correct near-instant return, yet short enough to bound the suite if
 * the bug regresses. */
constexpr uint64_t kLostWakeTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

/* The token-spending case pays this timeout in full on every run, so keep it
 * small. */
constexpr uint64_t kSpendTokenTimeoutNs = 400ULL * 1000 * 1000; /* 400 ms */

}  // namespace

TEST(St22PipelineTxEncodeBlocking, WakePostedBeforeWaitIsNotLost) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_enable_encode_blocking(ctx, kLostWakeTimeoutNs);

  /* Every framebuffer starts FREE, so the READY->IN_ENCODING claim fails and
   * the first encode_get_frame() enters the wait. Posting the wake before that
   * call is exactly the lost-wake window a real put_frame producer hits. */
  ut22p_tx_encode_wake_block(ctx);

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_tx_encode_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* No frame was actually readied, so the claim still fails -- what matters is
   * that it failed FAST, proving the pre-posted wake was not lost. */
  EXPECT_EQ(ret, -EBUSY) << "no frame was readied; encode_get_frame must hand back none";
  EXPECT_LT(elapsed, std::chrono::nanoseconds(kLostWakeTimeoutNs) / 4)
      << "blocking encode_get_frame slept ~full timeout: the pre-posted wake was lost";

  ut22p_tx_ctx_destroy(ctx);
}

TEST(St22PipelineTxEncodeBlocking, OneWakeSatisfiesOnlyOneWait) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_enable_encode_blocking(ctx, kSpendTokenTimeoutNs);

  ut22p_tx_encode_wake_block(ctx); /* exactly one wake */

  /* Spends the token; untimed, that is the other case's job. */
  EXPECT_EQ(ut22p_tx_encode_get_frame(ctx), -EBUSY);

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_tx_encode_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* The wait derives its deadline from CLOCK_MONOTONIC after t0 was read from
   * the same clock, so the full timeout must elapse -- exact, not approximate.
   * A spurious wakeup re-arms a fresh deadline, so this stays a lower bound. */
  EXPECT_EQ(ret, -EBUSY);
  EXPECT_GE(elapsed, std::chrono::nanoseconds(kSpendTokenTimeoutNs))
      << "second encode_get_frame returned early: the first wait did not spend "
         "the token";

  ut22p_tx_ctx_destroy(ctx);
}
