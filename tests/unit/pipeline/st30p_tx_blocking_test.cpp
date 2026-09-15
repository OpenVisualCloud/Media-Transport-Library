/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST30p (audio) TX pipeline blocking get_frame() wake-loss regression test.
 * See st20p_tx_blocking_test.cpp for the equivalent video-pipeline test and
 * design rationale.
 *
 * Bug background: an app running st30p_tx in blocking mode
 * (ST30P_TX_FLAG_BLOCK_GET) sets a block_timeout,
 * e.g. 1 second, expecting st30p_tx_get_frame() to wait up to that long for a
 * frame to become free before giving up. In production this returned after
 * only a few hundred nanoseconds instead, so the app just spun calling
 * get_frame() over and over instead of waiting or dropping cleanly.
 *
 * The original fix recorded wakes in a sticky flag that a fast-path claim
 * (frame already free, no wait needed) had to remember to clear -- easy to
 * get out of sync, which is exactly what happened. The current design has no
 * flag at all: every wake just re-asks "is a frame free right now?" via the
 * same claim used everywhere else, so there is nothing that can go stale.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pipeline/st30p_tx_harness.h"

TEST(St30PipelineTxBlocking, BlockedGetFrameWakesPromptlyOnGenuineFree) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 1;
  constexpr uint64_t kBlockTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  struct st30_frame* held = ut30p_tx_get_frame(ctx);
  ASSERT_NE(held, nullptr);

  int release_ret[3] = {-1, -1, -1};
  std::thread producer([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    /* Real transport release path: IN_USER -> READY -> IN_TRANSMITTING ->
     * FREE. tx_st30p_frame_done() is what calls
     * tx_st30p_notify_frame_available(), the only source of a genuine wake --
     * ut30p_tx_put_frame_abort() skips it entirely. */
    release_ret[0] = ut30p_tx_put_frame(ctx, held);
    uint16_t idx;
    release_ret[1] = ut30p_tx_next_frame(ctx, &idx);
    release_ret[2] = ut30p_tx_frame_done(ctx, idx);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st30_frame* frame = ut30p_tx_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  producer.join();
  EXPECT_EQ(release_ret[0], 0);
  EXPECT_EQ(release_ret[1], 0);
  EXPECT_EQ(release_ret[2], 0);

  ASSERT_NE(frame, nullptr) << "the slot freed by the producer was never claimed";
  EXPECT_LT(elapsed, std::chrono::nanoseconds(kBlockTimeoutNs) / 2)
      << "blocking get_frame() only noticed the freed slot near the full "
         "timeout: the wake was lost";

  ASSERT_EQ(ut30p_tx_put_frame_abort(ctx, frame), 0);
  ut30p_tx_ctx_destroy(ctx);
}

/*
 * Regression guard against ever reintroducing a flag-based shortcut: an
 * unrelated earlier wake must never make a later, genuinely-empty block
 * return early. See st20p_tx_blocking_test.cpp for full history.
 */
TEST(St30PipelineTxBlocking, StaleWakeFromEarlierFastPathDoesNotShortCircuitBlock) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 2;
  constexpr uint64_t kBlockTimeoutNs = 300ULL * 1000 * 1000; /* 300 ms */

  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  /* Step 1: claim slot 0. It is FREE, so get_frame() takes the fast path and
   * returns immediately -- the blocking wait code is never reached here. */
  struct st30_frame* held0 = ut30p_tx_get_frame(ctx);
  ASSERT_NE(held0, nullptr);

  /* Step 2: simulate the background event that happens constantly during
   * real streaming -- some other frame slot finishing transmission and being
   * freed. Nobody is blocked right now, so this just signals a condition
   * variable nobody is waiting on yet -- a true no-op under ground truth. */
  ut30p_tx_wake_block(ctx);

  /* Step 3: claim slot 1. Still FREE, so this is another fast-path claim. */
  struct st30_frame* held1 = ut30p_tx_get_frame(ctx);
  ASSERT_NE(held1, nullptr);

  /* Step 4: both slots are now IN_USER, so this call has nothing to claim and
   * must genuinely wait up to kBlockTimeoutNs. */
  const auto t0 = std::chrono::steady_clock::now();
  struct st30_frame* frame = ut30p_tx_get_frame(ctx);
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_EQ(frame, nullptr) << "no slot was freed; get_frame must return NULL";
  EXPECT_GT(elapsed, std::chrono::nanoseconds(kBlockTimeoutNs) / 2)
      << "blocking get_frame returned early: a stale wake from an earlier "
         "fast-path claim short-circuited the wait";

  ASSERT_EQ(ut30p_tx_put_frame_abort(ctx, held0), 0);
  ASSERT_EQ(ut30p_tx_put_frame_abort(ctx, held1), 0);
  ut30p_tx_ctx_destroy(ctx);
}

/*
 * The ground-truth wait loop attempts its claim before re-checking
 * lc_destroying, so it is possible to legitimately claim a real frame on the
 * very iteration where destroy also starts concurrently. get_frame() must
 * still honor "destroying always wins": release the claim back to FREE and
 * return NULL rather than hand out a frame from a session being torn down.
 *
 * The race window (claim succeeds -> break -> unlock -> destroying check) is
 * a handful of instructions wide, so a single attempt would essentially never
 * land in it. Repeating a tight two-thread race many times, with the racer
 * busy-polling for the reclaim and flipping destroying the instant it is
 * observed, hits the window often enough to catch a missing guard.
 */
TEST(St30PipelineTxBlocking, DestroyDuringWaitNeverHandsOutAClaimedFrame) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 1;
  constexpr uint64_t kBlockTimeoutNs = 200ULL * 1000 * 1000; /* 200 ms */
  constexpr int kReps = 300;
  constexpr auto kSpinBudget = std::chrono::milliseconds(200);

  for (int rep = 0; rep < kReps; rep++) {
    ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(kFrameCnt);
    ASSERT_NE(ctx, nullptr);
    ut30p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

    struct st30_frame* held = ut30p_tx_get_frame(ctx);
    ASSERT_NE(held, nullptr);

    std::thread racer([&]() {
      /* Real free path: wakes the blocked get_frame() below. */
      ut30p_tx_put_frame(ctx, held);
      uint16_t idx;
      ut30p_tx_next_frame(ctx, &idx);
      ut30p_tx_frame_done(ctx, idx);

      /* Flip destroying the instant the main thread reclaims the slot, to
       * land inside the narrow post-claim window as often as possible. Give
       * up (leaving destroying unset) if that reclaim never happens, so a
       * rep that misses the race can't hang the test. */
      const auto spin_start = std::chrono::steady_clock::now();
      while (ut30p_tx_frame_stat(ctx, 0) != 1 /* ST30P_TX_FRAME_IN_USER */) {
        if (std::chrono::steady_clock::now() - spin_start > kSpinBudget) return;
      }
      ut30p_tx_force_destroying(ctx);
    });

    struct st30_frame* frame = ut30p_tx_get_frame(ctx);
    racer.join();

    /* Once force_destroying() has run, every further MT_HANDLE_GUARD-gated
     * call on this ctx -- including put_frame_abort() -- is rejected with
     * -EIO regardless of frame state (that guard is what makes a dying
     * handle safe to tear down concurrently). So there is no valid guarded
     * call left to release `frame` with; ctx_destroy() below frees the raw
     * framebuff memory directly, which is all that's needed here. */
    if (frame == nullptr) {
      EXPECT_EQ(ut30p_tx_frame_stat(ctx, 0), 0 /* ST30P_TX_FRAME_FREE */)
          << "rep " << rep << ": slot left stuck IN_USER after a NULL return";
    } else {
      EXPECT_EQ(ut30p_tx_frame_stat(ctx, 0), 1 /* ST30P_TX_FRAME_IN_USER */)
          << "rep " << rep
          << ": a non-NULL return must correspond to a real IN_USER claim";
    }

    ut30p_tx_ctx_destroy(ctx);
  }
}

/*
 * A relative "wait another full timeout from now" re-arm on every losing wake
 * lets total time in get_frame() grow unbounded under sustained contention.
 * A decoy thread that wins the single slot's claim on every cycle keeps a
 * second, measured consumer losing the race repeatedly; the measured call
 * must still return once the deadline captured at entry has passed, no
 * matter how many times it lost and got woken again in the meantime.
 */
TEST(St30PipelineTxBlocking, ContendedLosingConsumerHonorsAbsoluteDeadline) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 1;
  constexpr uint64_t kBlockTimeoutNs = 150ULL * 1000 * 1000; /* 150 ms */
  constexpr auto kDecoyHoldTime = std::chrono::microseconds(500);
  constexpr auto kDecoyRunTime = std::chrono::milliseconds(450); /* 3x timeout */
  constexpr auto kElapsedCeiling = std::chrono::nanoseconds(kBlockTimeoutNs) * 2;

  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  struct st30_frame* held = ut30p_tx_get_frame(ctx);
  ASSERT_NE(held, nullptr);

  std::atomic<bool> stop{false};
  /* decoy has its own hard cutoff, independent of the measured call below:
   * under the pre-fix bug the measured call never returns on its own while
   * decoy keeps generating wakes, so decoy must give up unprompted for the
   * measured call to ever see a real, un-rearmed timeout and unblock. */
  const auto decoy_deadline = std::chrono::steady_clock::now() + kDecoyRunTime;
  std::thread decoy([&, decoy_deadline]() {
    while (!stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < decoy_deadline) {
      struct st30_frame* f = ut30p_tx_get_frame(ctx);
      if (!f) continue;
      /* Dominate the duty cycle: hold the slot far longer than the brief,
       * unavoidable gap between releasing it and reclaiming it, so a
       * periodic outside sampler almost never catches it FREE. */
      std::this_thread::sleep_for(kDecoyHoldTime);
      ut30p_tx_put_frame(ctx, f);
      uint16_t idx;
      if (ut30p_tx_next_frame(ctx, &idx) == 0) ut30p_tx_frame_done(ctx, idx);
    }
  });

  /* decoy is now blocked on the one slot; free it via the real wake path so
   * decoy claims it deterministically and starts a self-sustaining cycle. */
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_EQ(ut30p_tx_put_frame_abort(ctx, held), 0);
  ut30p_tx_wake_block(ctx);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const auto t0 = std::chrono::steady_clock::now();
  struct st30_frame* measured_frame =
      ut30p_tx_get_frame(ctx); /* loses the claim race repeatedly */
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  stop.store(true, std::memory_order_relaxed);
  decoy.join();

  EXPECT_LE(elapsed, kElapsedCeiling)
      << "get_frame() ran well past block_timeout_ns under repeated losing "
         "wakes: each losing wake re-armed a fresh relative timeout instead "
         "of honoring one absolute deadline";

  if (measured_frame) ut30p_tx_put_frame_abort(ctx, measured_frame);
  ut30p_tx_ctx_destroy(ctx);
}
