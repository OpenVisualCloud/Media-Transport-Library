/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p (compressed video) RX pipeline blocking-wait wake-token regression
 * tests, for both blocking waits in st22_pipeline_rx.c:
 *   - st22p_rx_get_frame() under ST22P_RX_FLAG_BLOCK_GET (app-facing), woken
 *     by rx_st22p_block_wake() via st22p_rx_wake_block / frame_ready / destroy.
 *   - rx_st22p_decode_get_block_wait() under ST22_DECODER_RESP_FLAG_BLOCK_GET
 *     (decoder-plugin-facing), woken by rx_st22p_decode_block_wake().
 *
 * The producer of claimable frames runs on a different thread than the waiter,
 * so a wake can be signalled in the window after the waiter's claim fails but
 * before it enters pthread_cond_timedwait. pthread_cond_signal only wakes
 * threads already waiting, so a single-shot "timedwait()" waiter loses that
 * wake and sleeps the entire timeout; a spurious wakeup makes the same waiter
 * give up early.
 *
 * The fix records the wake in a sticky *_wake_pending flag, re-checks it in a
 * predicate loop, and clears it on the way out. Both halves need pinning and
 * neither case pins both: WakePostedBeforeWaitIsNotLost only fails if the
 * waker stops recording the token (flag = true), OneWakeSatisfiesOnlyOneWait
 * only fails if the waiter stops spending it (flag = false).
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>

#include "pipeline/st22p_harness.h"

namespace {

/* Long enough that a lost wake (full-timeout block) is unmistakably distinct
 * from the correct near-instant return, yet short enough to bound the suite if
 * the bug regresses. */
constexpr uint64_t kLostWakeTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

/* The token-spending cases pay this timeout in full on every run, so keep it
 * small. */
constexpr uint64_t kSpendTokenTimeoutNs = 400ULL * 1000 * 1000; /* 400 ms */

}  // namespace

TEST(St22PipelineRxBlocking, WakePostedBeforeWaitIsNotLost) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLostWakeTimeoutNs);

  /* Every framebuffer starts FREE, so the DECODED->IN_USER claim fails and the
   * first get_frame() enters the wait. Posting the wake before that call is
   * exactly the lost-wake window a real frame_ready producer hits. */
  ut22p_wake_block(ctx);

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut22p_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* No frame was actually decoded, so the claim still fails -- what matters is
   * that it failed FAST, proving the pre-posted wake was not lost. */
  EXPECT_EQ(frame, nullptr) << "no frame was decoded; get_frame must return NULL";
  EXPECT_LT(elapsed, std::chrono::nanoseconds(kLostWakeTimeoutNs) / 4)
      << "blocking get_frame slept ~full timeout: the pre-posted wake was lost";

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, OneWakeSatisfiesOnlyOneWait) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kSpendTokenTimeoutNs);

  ut22p_wake_block(ctx); /* exactly one wake */

  /* Spends the token; untimed, that is the other case's job. */
  EXPECT_EQ(ut22p_get_frame(ctx), nullptr);

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut22p_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  /* The wait derives its deadline from CLOCK_MONOTONIC after t0 was read from
   * the same clock, so the full timeout must elapse -- exact, not approximate.
   * A spurious wakeup re-arms a fresh deadline, so this stays a lower bound. */
  EXPECT_EQ(frame, nullptr);
  EXPECT_GE(elapsed, std::chrono::nanoseconds(kSpendTokenTimeoutNs))
      << "second get_frame returned early: the first wait did not spend the token";

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, DecodeWakePostedBeforeWaitIsNotLost) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_decode_blocking(ctx, kLostWakeTimeoutNs);

  /* All-FREE framebuffers fail the READY->IN_DECODING claim, so the plugin's
   * first get_frame() enters the wait. */
  ut22p_decode_wake_block(ctx);

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_decode_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_EQ(ret, -EBUSY) << "no frame was received; decode_get_frame must hand back none";
  EXPECT_LT(elapsed, std::chrono::nanoseconds(kLostWakeTimeoutNs) / 4)
      << "blocking decode_get_frame slept ~full timeout: the pre-posted wake was lost";

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, DecodeOneWakeSatisfiesOnlyOneWait) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_decode_blocking(ctx, kSpendTokenTimeoutNs);

  ut22p_decode_wake_block(ctx); /* exactly one wake */

  EXPECT_EQ(ut22p_decode_get_frame(ctx), -EBUSY);

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_decode_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_EQ(ret, -EBUSY);
  EXPECT_GE(elapsed, std::chrono::nanoseconds(kSpendTokenTimeoutNs))
      << "second decode_get_frame returned early: the first wait did not spend "
         "the token";

  ut22p_ctx_destroy(ctx);
}
