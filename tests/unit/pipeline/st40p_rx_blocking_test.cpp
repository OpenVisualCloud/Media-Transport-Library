/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p (ancillary) RX pipeline blocking get_frame() tests.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "pipeline/st40p_harness.h"

namespace {

constexpr int kFrameCnt = 3;
constexpr uint32_t kTimestamp = 1000;
constexpr uint64_t kShortTimeoutNs = 300ULL * 1000 * 1000;     /* 300 ms */
constexpr uint64_t kLongTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */
constexpr auto kWakeDelay = std::chrono::milliseconds(80);

uint64_t ns_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

}  // namespace

class St40PipelineRxBlocking : public ::testing::Test {
 protected:
  ut40p_ctx* ctx_ = nullptr;
  uint8_t udw_ = 0;

  void SetUp() override {
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    ctx_ = ut40p_ctx_create(kFrameCnt);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

/* The stale frame wake, fixed here as for st20p.
 *
 * Every frame-ready notify called rx_st40p_block_wake(), which set
 * block_wake_pending even when no get waited. A get that claimed its frame at
 * once never reached the wait and never cleared the flag. The next get that
 * found no frame saw the flag, skipped the wait and returned NULL at once
 * instead of blocking for block_timeout_ns. That happened as soon as the app
 * fell a frame behind and caught up. RxTxApp (rx_st40p_app.c) and the st40p
 * RX sample both use ST40P_RX_FLAG_BLOCK_GET. As for st20p, the notify now
 * only signals, and the waiter claims on every wake up to one deadline.
 */
TEST_F(St40PipelineRxBlocking, StaleFrameWakeDoesNotSkipTheWait) {
  ut40p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  ASSERT_EQ(ut40p_inject_frame(ctx_, &udw_, ST_FRAME_STATUS_COMPLETE, false, kTimestamp),
            0);
  struct st40_frame_info* frame = ut40p_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut40p_put_frame(ctx_, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut40p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "the empty get returned without waiting for block_timeout_ns";
}

TEST_F(St40PipelineRxBlocking, FrameReadyWakesABlockedGet) {
  ut40p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  int inject_ret = -1;
  std::thread producer([&]() {
    std::this_thread::sleep_for(kWakeDelay);
    inject_ret =
        ut40p_inject_frame(ctx_, &udw_, ST_FRAME_STATUS_COMPLETE, false, kTimestamp);
  });

  const auto t0 = std::chrono::steady_clock::now();
  struct st40_frame_info* frame = ut40p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);
  producer.join();

  EXPECT_EQ(inject_ret, 0);
  ASSERT_NE(frame, nullptr) << "the frame that arrived during the wait was not returned";
  EXPECT_LT(elapsed_ns, kLongTimeoutNs / 2)
      << "the frame-ready notify did not wake the get";
  EXPECT_EQ(ut40p_put_frame(ctx_, frame), 0);
}

TEST_F(St40PipelineRxBlocking, DestroyEndsEveryBlockedGet) {
  ut40p_ctx_enable_blocking(ctx_, kLongTimeoutNs);

  struct st40_frame_info* frames[2] = {};
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    frames[i] = ut40p_get_frame(ctx_);
    elapsed_ns[i] = ns_since(t0);
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);

  std::this_thread::sleep_for(kWakeDelay);
  ut40p_begin_destroy(ctx_);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++) {
    EXPECT_EQ(frames[i], nullptr) << "getter " << i;
    EXPECT_LT(elapsed_ns[i], kLongTimeoutNs / 2)
        << "destroy did not end the wait of getter " << i;
  }
}

/* A wake_block() posted while no get waits must not end a later, unrelated wait. */
TEST_F(St40PipelineRxBlocking, WakeBlockIsSpentByAFastPathGet) {
  ut40p_ctx_enable_blocking(ctx_, kShortTimeoutNs);

  ut40p_wake_block(ctx_);
  ASSERT_EQ(ut40p_inject_frame(ctx_, &udw_, ST_FRAME_STATUS_COMPLETE, false, kTimestamp),
            0);
  struct st40_frame_info* frame = ut40p_get_frame(ctx_);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut40p_put_frame(ctx_, frame), 0);

  const auto t0 = std::chrono::steady_clock::now();
  frame = ut40p_get_frame(ctx_);
  const uint64_t elapsed_ns = ns_since(t0);

  EXPECT_EQ(frame, nullptr);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "a wake_block spent by a fast-path get ended a later wait";
}
