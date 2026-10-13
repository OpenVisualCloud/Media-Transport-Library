/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * PR 1796 review: st30p wake_block() wake-loss cases.
 *
 * Each case forces one interleaving by holding block_wake_mutex from the test
 * thread, so the order in which the other threads get the mutex is fixed by
 * the futex FIFO queue and not by a timing race.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pipeline/st30p_harness.h"
#include "pipeline/st30p_tx_harness.h"

namespace {

constexpr uint64_t kTimeoutNs = 600ULL * 1000 * 1000; /* 600 ms */
constexpr auto kSettle = std::chrono::milliseconds(50);

uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

/* Two RX consumers. B blocks in get_frame. The app calls wake_block() to end
 * B's wait. Before B runs again, consumer C (here: the test thread) gets a
 * frame on the fast path (in its own thread). Since 7f3afbd8, C's fast path clears
 * block_wake_pending, so it spends the token that was posted for B, and B
 * sleeps on to its full block timeout. Before the PR (and with the fix below)
 * B returns promptly. */
TEST(St30pReview, RxWakeBlockForABlockedGetIsNotSpentByAnotherFastPathGet) {
  ASSERT_EQ(ut30p_init(), 0);
  ut30p_ctx* ctx = ut30p_ctx_create(3);
  ASSERT_NE(ctx, nullptr);
  ut30p_ctx_enable_blocking(ctx, kTimeoutNs);

  struct st30_frame* b_frame = nullptr;
  uint64_t b_elapsed_ns = 0;
  std::atomic<uint64_t> wake_posted_at{0};
  std::thread consumer_b([&]() {
    b_frame = ut30p_get_frame(ctx); /* blocks: no frame */
    b_elapsed_ns = ns_since(std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(wake_posted_at.load())));
  });
  std::this_thread::sleep_for(kSettle); /* B now sleeps in timedwait */

  /* wake_block(): token + broadcast. B is woken but must wait for the mutex. */
  ut30p_lock_block_mutex(ctx);
  wake_posted_at = std::chrono::steady_clock::now().time_since_epoch().count();
  ut30p_post_wake_token_locked(ctx);

  /* A frame arrives (its notify then blocks on the mutex, after READY is set). */
  std::thread transport(
      [&]() { ut30p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000); });
  const auto spin0 = std::chrono::steady_clock::now();
  while (ut30p_frame_stat(ctx, 0) != 1 /* ST30P_RX_FRAME_READY */) {
    ASSERT_LT(ns_since(spin0), 1000ULL * 1000 * 1000) << "inject did not run";
  }

  /* Consumer C takes the frame on the fast path. */
  struct st30_frame* c_frame = nullptr;
  std::thread consumer_c([&]() { c_frame = ut30p_get_frame(ctx); });
  const auto spin1 = std::chrono::steady_clock::now();
  while (ut30p_frame_stat(ctx, 0) != 2 /* ST30P_RX_FRAME_IN_USER */) {
    ASSERT_LT(ns_since(spin1), 1000ULL * 1000 * 1000) << "C did not claim";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); /* C ends its get */
  ut30p_unlock_block_mutex(ctx);

  consumer_c.join();
  transport.join();
  consumer_b.join();
  ASSERT_NE(c_frame, nullptr);

  EXPECT_EQ(b_frame, nullptr);
  EXPECT_LT(b_elapsed_ns, kTimeoutNs / 2)
      << "wake_block() did not end the blocked get: another consumer's "
         "fast-path get spent its token";

  ut30p_put_frame(ctx, c_frame);
  ut30p_ctx_destroy(ctx);
}

/* RX reference for the TX case below: a wake_block() that lands while a get
 * is in flight (fast-path claim failed, wait mutex not yet taken) ends that
 * get. The sticky block_wake_pending token gives this. */
TEST(St30pReview, RxWakeBlockDuringAnInFlightGetEndsIt) {
  ASSERT_EQ(ut30p_init(), 0);
  ut30p_ctx* ctx = ut30p_ctx_create(3);
  ASSERT_NE(ctx, nullptr);
  ut30p_ctx_enable_blocking(ctx, kTimeoutNs);

  ut30p_lock_block_mutex(ctx);
  std::thread waker([&]() { ut30p_wake_block(ctx); }); /* queues on the mutex */
  std::this_thread::sleep_for(kSettle);

  struct st30_frame* frame = nullptr;
  uint64_t elapsed_ns = 0;
  std::thread getter([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    frame = ut30p_get_frame(ctx); /* fast claim fails, queues on the mutex */
    elapsed_ns = ns_since(t0);
  });
  std::this_thread::sleep_for(kSettle);
  ut30p_unlock_block_mutex(ctx); /* waker gets the mutex first, then getter */

  waker.join();
  getter.join();
  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kSettle.count() * 1000ULL * 1000 + kTimeoutNs / 2)
      << "a wake_block() during an in-flight RX get was lost";
  ut30p_ctx_destroy(ctx);
}

/* Same interleaving on TX. st30p_tx_get_frame() reads wake_block_cnt only
 * after it takes block_wake_mutex, so a wake_block() that ran between the
 * failed fast-path claim and that lock is invisible, and the get sleeps the
 * full block timeout. */
TEST(St30pReview, TxWakeBlockDuringAnInFlightGetEndsIt) {
  ASSERT_EQ(ut30p_tx_init(), 0);
  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_ctx_enable_blocking(ctx, kTimeoutNs);

  struct st30_frame* held = ut30p_tx_get_frame(ctx); /* no free frame left */
  ASSERT_NE(held, nullptr);

  ut30p_tx_lock_block_mutex(ctx);
  std::thread waker([&]() { ut30p_tx_wake_block(ctx); }); /* queues on the mutex */
  std::this_thread::sleep_for(kSettle);

  struct st30_frame* frame = nullptr;
  uint64_t elapsed_ns = 0;
  std::thread getter([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    frame = ut30p_tx_get_frame(ctx); /* fast claim fails, queues on the mutex */
    elapsed_ns = ns_since(t0);
  });
  std::this_thread::sleep_for(kSettle);
  ut30p_tx_unlock_block_mutex(ctx); /* waker gets the mutex first, then getter */

  waker.join();
  getter.join();
  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kSettle.count() * 1000ULL * 1000 + kTimeoutNs / 2)
      << "a wake_block() during an in-flight TX get was lost: the get slept "
         "its full block timeout";

  ASSERT_EQ(ut30p_tx_put_frame_abort(ctx, held), 0);
  ut30p_tx_ctx_destroy(ctx);
}
