/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Review tests for PR 1796, st22p RX pipeline.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "pipeline/st22p_harness.h"

extern "C" int st22p_rx_free(st22p_rx_handle handle);

namespace {
constexpr uint64_t kTimeoutNs = 400ULL * 1000 * 1000; /* 400 ms */
constexpr uint64_t kLongTimeoutNs = 2ULL * 1000 * 1000 * 1000;
}  // namespace

/* R1: st22p_rx_free() puts the decoder and sets decode_impl = NULL before it
 * frees the transport. The transport still delivers frames until
 * st22_rx_free() detaches it, and rx_st22p_frame_ready() is not guarded and
 * still sees ready == true, so it calls rx_st22p_decode_notify_frame_ready(),
 * which dereferences decode_impl == NULL. */
TEST(St22PipelineRxReview, FrameDuringFreeDoesNotCrash) {
  ASSERT_EQ(ut22p_init(), 0);
  EXPECT_EXIT(
      {
        st22p_rx_handle h = ut22p_review_create_decoder_ctx(2);
        if (!h) std::_Exit(2);
        ut22p_review_deliver_frame_in_rx_free(h);
        st22p_rx_free(h);
        std::_Exit(ut22p_review_rx_free_calls_get() == 1 ? 0 : 3);
      },
      ::testing::ExitedWithCode(0), "");
}

/* R2: decoder-side wake_block() token is not spent by a fast-path get, unlike
 * the app side after "Spend a wake_block() on the next ... get". */
TEST(St22PipelineRxReview, DecodeWakeBlockIsSpentByAFastPathGet) {
  ASSERT_EQ(ut22p_init(), 0);
  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_set_decoder(ctx);
  ut22p_ctx_enable_decode_blocking(ctx, kTimeoutNs);

  ut22p_decode_wake_block(ctx);
  ASSERT_EQ(ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000), 0);
  ASSERT_EQ(ut22p_decode_get_frame(ctx), 0); /* fast path */

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_decode_get_frame(ctx);
  const uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
  EXPECT_EQ(ret, -EBUSY);
  EXPECT_GT(elapsed_ns, kTimeoutNs / 2)
      << "a decoder wake_block spent by a fast-path get ended a later wait";
  ut22p_ctx_destroy(ctx);
}

/* R3: with two consumers, a fast-path get on thread A clears the wake_block()
 * token meant for the get blocked on thread B; B then sleeps its full
 * block timeout. On upstream/main the fast path does not touch the token. */
TEST(St22PipelineRxReview, WakeBlockEndsTheBlockedGetDespiteAFastPathGet) {
  ASSERT_EQ(ut22p_init(), 0);
  ut22p_ctx* ctx = ut22p_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_ctx_enable_blocking(ctx, kLongTimeoutNs);

  std::atomic<uint64_t> b_elapsed_ns{0};
  struct st_frame* b_frame = nullptr;
  std::thread b([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    b_frame = ut22p_get_frame(ctx);
    b_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ut22p_wake_block(ctx); /* meant for B */
  ASSERT_EQ(ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, 1000), 0);
  struct st_frame* a_frame = ut22p_get_frame(ctx); /* A, fast path */
  b.join();

  if (a_frame) {
    EXPECT_EQ(ut22p_put_frame(ctx, a_frame), 0);
  }
  if (b_frame) {
    EXPECT_EQ(ut22p_put_frame(ctx, b_frame), 0);
  }
  EXPECT_LT(b_elapsed_ns.load(), kLongTimeoutNs / 2)
      << "wake_block() did not end the blocked get: a fast-path get took the token";
  ut22p_ctx_destroy(ctx);
}
