/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st40p RX wake_block() token cases.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "pipeline/st40p_harness.h"
#include "pipeline/st40p_tx_harness.h"

namespace {

constexpr int kFrameCnt = 3;
constexpr uint64_t kLongTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */
constexpr auto kSettle = std::chrono::milliseconds(80);

uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

class St40RxWakeReview : public ::testing::Test {
 protected:
  ut40p_ctx* ctx_ = nullptr;
  uint8_t udw_ = 0;

  void SetUp() override {
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    ctx_ = ut40p_ctx_create(kFrameCnt);
    ASSERT_NE(ctx_, nullptr);
    ut40p_ctx_enable_blocking(ctx_, kLongTimeoutNs);
  }

  void TearDown() override {
    ut40p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* F1, forced interleaving. One get blocks. A frame turns READY (the window in
 * rx_st40p_frame_ready() between the READY store and the notify) and
 * st40p_rx_wake_block() runs. Before the woken waiter can take the mutex back,
 * a second consumer's get takes the frame on the fast path and clears the
 * token. The blocked get then sleeps to its full block timeout. On
 * upstream/main the fast path does not touch the token, so the wait ends. */
TEST_F(St40RxWakeReview, FastPathGetDoesNotStealTheWakeOfABlockedGet_Forced) {
  struct st40_frame_info* blocked_frame = reinterpret_cast<st40_frame_info*>(1);
  uint64_t blocked_ns = 0;
  std::thread blocked([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    blocked_frame = ut40p_get_frame(ctx_);
    blocked_ns = ns_since(t0);
  });
  std::this_thread::sleep_for(kSettle); /* the get is in the cond wait */

  ut40p_review_set_ready_no_notify(ctx_, 0, &udw_);
  ut40p_review_wake_block_hold_mutex(ctx_); /* = st40p_rx_wake_block() body */
  struct st40_frame_info* fast = nullptr;
  /* second consumer: its get finds the READY frame on the fast path */
  std::thread consumer([&]() { fast = ut40p_get_frame(ctx_); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ut40p_review_unlock_wake_mutex(ctx_);
  consumer.join();
  blocked.join();

  ASSERT_NE(fast, nullptr);
  EXPECT_EQ(blocked_frame, nullptr);
  EXPECT_LT(blocked_ns, kLongTimeoutNs / 2)
      << "a fast-path get took the wake_block() token of the blocked get";
  ut40p_put_frame(ctx_, fast);
}

/* F1, real API calls only, in a loop: the same race with no mutex hold. */
TEST_F(St40RxWakeReview, FastPathGetDoesNotStealTheWakeOfABlockedGet_Race) {
  constexpr int kReps = 20;
  int lost = 0;
  for (int rep = 0; rep < kReps; rep++) {
    uint64_t blocked_ns = 0;
    std::thread blocked([&]() {
      const auto t0 = std::chrono::steady_clock::now();
      struct st40_frame_info* f = ut40p_get_frame(ctx_);
      blocked_ns = ns_since(t0);
      if (f) ut40p_put_frame(ctx_, f);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ut40p_review_set_ready_no_notify(ctx_, rep % kFrameCnt, &udw_);
    ut40p_wake_block(ctx_);
    struct st40_frame_info* fast = ut40p_get_frame(ctx_);
    blocked.join();
    if (fast) ut40p_put_frame(ctx_, fast);
    if (blocked_ns > kLongTimeoutNs / 2) lost++;
  }
  EXPECT_EQ(lost, 0) << lost << " of " << kReps
                     << " wake_block() calls did not end the blocked get";
}

/* F2, forced interleaving. Two gets block. Two st40p_rx_wake_block() calls run
 * before either waiter takes the mutex back. The bool token holds one wake,
 * so only one get ends; the other sleeps to its full block timeout. The TX
 * pipeline (wake_block_cnt) ends both, see the TX case below. */
TEST_F(St40RxWakeReview, TwoWakeBlocksEndTwoBlockedGets_Forced) {
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    struct st40_frame_info* f = ut40p_get_frame(ctx_);
    elapsed_ns[i] = ns_since(t0);
    EXPECT_EQ(f, nullptr);
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);
  std::this_thread::sleep_for(kSettle);

  ut40p_review_wake_block_hold_mutex(ctx_);   /* wake_block() #1 */
  ut40p_review_wake_block_again_locked(ctx_); /* wake_block() #2 */
  ut40p_review_unlock_wake_mutex(ctx_);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++)
    EXPECT_LT(elapsed_ns[i], kLongTimeoutNs / 2)
        << "getter " << i << " was not ended by either of two wake_block() calls";
}

/* F2, real API calls only, in a loop. */
TEST_F(St40RxWakeReview, TwoWakeBlocksEndTwoBlockedGets_Race) {
  constexpr int kReps = 10;
  int lost = 0;
  for (int rep = 0; rep < kReps; rep++) {
    uint64_t elapsed_ns[2] = {};
    auto getter = [&](int i) {
      const auto t0 = std::chrono::steady_clock::now();
      struct st40_frame_info* f = ut40p_get_frame(ctx_);
      elapsed_ns[i] = ns_since(t0);
      if (f) ut40p_put_frame(ctx_, f);
    };
    std::thread g0(getter, 0);
    std::thread g1(getter, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ut40p_wake_block(ctx_);
    ut40p_wake_block(ctx_);
    g0.join();
    g1.join();
    for (int i = 0; i < 2; i++)
      if (elapsed_ns[i] > kLongTimeoutNs / 2) lost++;
  }
  EXPECT_EQ(lost, 0) << lost << " of " << 2 * kReps
                     << " blocked gets were not ended by two wake_block() calls";
}

/* Reference: the TX pipeline ends two blocked gets with two wake_block(). */
TEST(St40TxWakeReview, TwoWakeBlocksEndTwoBlockedGets_Race) {
  ASSERT_EQ(ut40p_tx_init(), 0);
  constexpr int kReps = 10;
  int lost = 0;
  for (int rep = 0; rep < kReps; rep++) {
    ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
    ASSERT_NE(ctx, nullptr);
    ut40p_tx_ctx_enable_blocking(ctx, kLongTimeoutNs);
    struct st40_frame_info* held = ut40p_tx_get_frame(ctx);
    ASSERT_NE(held, nullptr);
    uint64_t elapsed_ns[2] = {};
    auto getter = [&](int i) {
      const auto t0 = std::chrono::steady_clock::now();
      struct st40_frame_info* f = ut40p_tx_get_frame(ctx);
      elapsed_ns[i] = ns_since(t0);
      EXPECT_EQ(f, nullptr);
    };
    std::thread g0(getter, 0);
    std::thread g1(getter, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ut40p_tx_wake_block(ctx);
    ut40p_tx_wake_block(ctx);
    g0.join();
    g1.join();
    for (int i = 0; i < 2; i++)
      if (elapsed_ns[i] > kLongTimeoutNs / 2) lost++;
    ut40p_tx_put_frame_abort(ctx, held);
    ut40p_tx_ctx_destroy(ctx);
  }
  EXPECT_EQ(lost, 0);
}

/* F3, forced interleaving (pre-existing, not from this PR). A TX get finds no
 * free frame and is about to take block_wake_mutex when st40p_tx_wake_block()
 * runs (the stop-flag pattern: stop = true; wake_block(); join()). The get
 * reads wake_block_cnt only after it takes the mutex, so it records the new
 * count and sleeps to its full block timeout. The RX get in the same place
 * sees the token and returns at once (see the RX case below). */
TEST(St40TxWakeReview, WakeBlockDuringGetBeforeTheWaitIsNotLost_Forced) {
  ASSERT_EQ(ut40p_tx_init(), 0);
  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut40p_tx_ctx_enable_blocking(ctx, kLongTimeoutNs);
  struct st40_frame_info* held = ut40p_tx_get_frame(ctx);
  ASSERT_NE(held, nullptr);

  ut40p_tx_review_lock_wake_mutex(ctx);
  uint64_t elapsed_ns = 0;
  struct st40_frame_info* frame = reinterpret_cast<st40_frame_info*>(1);
  std::thread getter([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    frame = ut40p_tx_get_frame(ctx); /* fast claim fails, then blocks on the mutex */
    elapsed_ns = ns_since(t0);
  });
  std::this_thread::sleep_for(kSettle);
  ut40p_tx_review_wake_block_locked(ctx); /* = st40p_tx_wake_block() body */
  ut40p_tx_review_unlock_wake_mutex(ctx);
  getter.join();

  EXPECT_EQ(frame, nullptr);
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2)
      << "a wake_block() posted during the get, before its wait, was lost";
  ut40p_tx_put_frame_abort(ctx, held);
  ut40p_tx_ctx_destroy(ctx);
}

/* RX reference for F3: same interleaving, the wake ends the get. */
TEST_F(St40RxWakeReview, WakeBlockDuringGetBeforeTheWaitIsNotLost_Forced) {
  ut40p_review_lock_wake_mutex(ctx_);
  uint64_t elapsed_ns = 0;
  std::thread getter([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    struct st40_frame_info* f = ut40p_get_frame(ctx_);
    elapsed_ns = ns_since(t0);
    EXPECT_EQ(f, nullptr);
  });
  std::this_thread::sleep_for(kSettle);
  ut40p_review_wake_block_again_locked(ctx_); /* = wake_block() body */
  ut40p_review_unlock_wake_mutex(ctx_);
  getter.join();
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2);
}
