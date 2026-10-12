/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p (ancillary) RX pipeline blocking get_frame() tests.
 */

#include <gtest/gtest.h>

#include <chrono>

#include "pipeline/st40p_harness.h"

namespace {

constexpr int kFrameCnt = 3;
constexpr uint32_t kTimestamp = 1000;
constexpr uint64_t kShortTimeoutNs = 300ULL * 1000 * 1000; /* 300 ms */

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

/* The stale frame wake that this PR fixes for st20p, still present in st40p.
 *
 * Every frame-ready notify calls rx_st40p_block_wake(), which sets
 * block_wake_pending even when no get waits. A get that claims its frame at
 * once never reaches the wait and never clears the flag. The next get that
 * finds no frame sees the flag, skips the wait and returns NULL at once
 * instead of blocking for block_timeout_ns. That happens as soon as the app
 * falls a frame behind and catches up. RxTxApp (rx_st40p_app.c) and the st40p
 * RX sample both use ST40P_RX_FLAG_BLOCK_GET. The st20p fix applies unchanged:
 * the notify only signals, and the waiter claims on every wake up to one
 * deadline.
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
