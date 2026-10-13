/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p (compressed video) RX pipeline blocking-wait wake-token regression
 * tests, for both blocking waits in st22_pipeline_rx.c:
 *   - st22p_rx_get_frame() under ST22P_RX_FLAG_BLOCK_GET (app-facing), woken
 *     by rx_st22p_block_wake() via st22p_rx_wake_block / destroy, or by frame_ready.
 *   - rx_st22p_decode_get_frame() under ST22_DECODER_RESP_FLAG_BLOCK_GET
 *     (decoder-plugin-facing), woken by rx_st22p_decode_block_wake() or by frame_ready.
 *
 * The producer of claimable frames runs on a different thread than the waiter,
 * so a wake can be signalled in the window after the waiter's claim fails but
 * before it enters pthread_cond_timedwait. pthread_cond_signal only wakes
 * threads already waiting, so a single-shot "timedwait()" waiter loses that
 * wake and sleeps the entire timeout; a spurious wakeup makes the same waiter
 * give up early.
 *
 * A frame cannot fall in that window: the waiter claims under the wake mutex
 * on every wake, up to one deadline. wake_block() and destroy record the wake
 * in a sticky *_wake_pending flag, which the predicate loop re-checks and
 * clears on the way out. Both halves need pinning and neither case pins both:
 * WakePostedBeforeWaitIsNotLost only fails if the waker stops recording the
 * token (flag = true), OneWakeSatisfiesOnlyOneWait only fails if the waiter
 * stops spending it (flag = false).
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "pipeline/st22p_harness.h"

namespace {

/* Long enough that a lost wake (full-timeout block) is unmistakably distinct
 * from the correct near-instant return, yet short enough to bound the suite if
 * the bug regresses. */
constexpr uint64_t kLostWakeTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

/* The token-spending cases pay this timeout in full on every run, so keep it
 * small. */
constexpr uint64_t kSpendTokenTimeoutNs = 400ULL * 1000 * 1000; /* 400 ms */

constexpr auto kWakeDelay = std::chrono::milliseconds(80);

}  // namespace

TEST(St22PipelineRxBlocking, WakePostedBeforeWaitIsNotLost) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLostWakeTimeoutNs);

  /* Every framebuffer starts FREE, so the DECODED->IN_USER claim fails and the
   * first get_frame() enters the wait. A wake_block() posted before that call
   * must still end it. */
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

  /* the deadline is read from CLOCK_MONOTONIC after t0, so the full timeout elapses */
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

/* The stale frame wake, fixed here as for st20p.
 *
 * Every frame-ready notify called rx_st22p_block_wake(), which set
 * block_wake_pending even when no get waited. A get that claimed its frame at
 * once never reached the wait and never cleared the flag. The next get that
 * found no frame saw the flag, skipped the wait and returned NULL at once
 * instead of blocking for block_timeout_ns. That happened as soon as the app
 * fell a frame behind and caught up. The FFmpeg st22p demuxer
 * (mtl_st22p_rx.c) sets ST22P_RX_FLAG_BLOCK_GET and turns that NULL into EIO,
 * the failure st20p had too. As for st20p, the notify now only signals, and
 * the waiter claims on every wake up to one deadline.
 */
TEST(St22PipelineRxBlocking, StaleFrameWakeDoesNotSkipTheWait) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kSpendTokenTimeoutNs);

  ASSERT_EQ(ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000), 0);
  struct st_frame* frame = ut22p_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut22p_put_frame(ctx, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut22p_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kSpendTokenTimeoutNs / 2)
      << "the empty get returned without waiting for block_timeout_ns";

  ut22p_ctx_destroy(ctx);
}

/* The same stale wake on the decoder-plugin side of st22p RX.
 *
 * Every frame handed to the decoder called rx_st22p_decode_block_wake(), which
 * set decode_block_wake_pending even when the decoder was not waiting. A
 * decoder get that claimed its frame at once never cleared it, so the
 * decoder's next empty get returned -EBUSY at once instead of blocking for the
 * decode timeout. Plugins that retry only lost time; one that treats -EBUSY
 * from a blocking get as end of stream stopped decoding.
 */
TEST(St22PipelineRxBlocking, DecodeStaleFrameWakeDoesNotSkipTheWait) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_set_decoder(ctx);
  ut22p_ctx_enable_decode_blocking(ctx, kSpendTokenTimeoutNs);

  ASSERT_EQ(ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000), 0);
  ASSERT_EQ(ut22p_decode_get_frame(ctx), 0);

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_decode_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();

  EXPECT_EQ(ret, -EBUSY);
  EXPECT_GT(elapsed_ns, kSpendTokenTimeoutNs / 2)
      << "the empty decode_get_frame returned without waiting for the timeout";

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, FrameReadyWakesABlockedGet) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLostWakeTimeoutNs);

  int inject_ret = -1;
  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    inject_ret = ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st_frame* frame = ut22p_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
  producer.join();

  EXPECT_EQ(inject_ret, 0);
  ASSERT_NE(frame, nullptr) << "the frame that arrived during the wait was not returned";
  EXPECT_LT(elapsed_ns, kLostWakeTimeoutNs / 2)
      << "the frame-ready notify did not wake the get";
  EXPECT_EQ(ut22p_put_frame(ctx, frame), 0);

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, DecodeFrameReadyWakesABlockedGet) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_set_decoder(ctx);
  ut22p_ctx_enable_decode_blocking(ctx, kLostWakeTimeoutNs);

  int inject_ret = -1;
  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    inject_ret = ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000);
  });

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_decode_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
  producer.join();

  EXPECT_EQ(inject_ret, 0);
  EXPECT_EQ(ret, 0) << "the frame that arrived during the wait was not handed back";
  EXPECT_LT(elapsed_ns, kLostWakeTimeoutNs / 2)
      << "the decoder frame notify did not wake the decode_get_frame";

  ut22p_ctx_destroy(ctx);
}

/* A decoder can call get before create sets ready; the first frame ends that wait. */
TEST(St22PipelineRxBlocking, DecodeFirstFrameEndsANotReadyWait) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_set_decoder(ctx);
  ut22p_ctx_enable_decode_blocking(ctx, kLostWakeTimeoutNs);
  ut22p_set_ready(ctx, false);

  int inject_ret = -1;
  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    ut22p_set_ready(ctx, true);
    inject_ret = ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000);
  });

  const auto t0 = std::chrono::steady_clock::now();
  ut22p_decode_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
  producer.join();

  EXPECT_EQ(inject_ret, 0);
  EXPECT_LT(elapsed_ns, kLostWakeTimeoutNs / 2)
      << "the first frame did not end the decode_get_frame that waited for ready";

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, DestroyEndsEveryBlockedGet) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLostWakeTimeoutNs);
  ut22p_ctx_enable_decode_blocking(ctx, kLostWakeTimeoutNs);

  struct st_frame* frames[2] = {};
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    frames[i] = ut22p_get_frame(ctx);
    elapsed_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);

  std::this_thread::sleep_for(kWakeDelay);
  ut22p_begin_destroy(ctx);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(frames[i], nullptr) << "getter " << i;
    EXPECT_LT(elapsed_ns[i], kLostWakeTimeoutNs / 2)
        << "destroy did not end the wait of getter " << i;
  }

  ut22p_ctx_destroy(ctx);
}

TEST(St22PipelineRxBlocking, DecodeDestroyEndsEveryBlockedGet) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLostWakeTimeoutNs);
  ut22p_ctx_enable_decode_blocking(ctx, kLostWakeTimeoutNs);

  int rets[2] = {0, 0};
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    rets[i] = ut22p_decode_get_frame(ctx);
    elapsed_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);

  std::this_thread::sleep_for(kWakeDelay);
  ut22p_begin_destroy(ctx);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(rets[i], -EBUSY) << "getter " << i;
    EXPECT_LT(elapsed_ns[i], kLostWakeTimeoutNs / 2)
        << "destroy did not end the wait of getter " << i;
  }

  ut22p_ctx_destroy(ctx);
}
