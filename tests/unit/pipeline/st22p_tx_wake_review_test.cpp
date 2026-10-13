/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Review tests for PR 1796, st22p TX pipeline blocking waits.
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "pipeline/st22p_tx_harness.h"

namespace {
constexpr uint64_t kTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */
constexpr uint64_t kShortTimeoutNs = 400ULL * 1000 * 1000; /* 400 ms */

uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}
}  // namespace

/* Two app threads block in st22p_tx_get_frame(). The transport frees two
 * frames back to back. Each frame done sets the bool block_wake_pending and
 * signals one waiter. The first waiter to get the mutex clears the flag and
 * claims one frame; the second waiter then sees no flag and sleeps again, so
 * a FREE frame waits for block_timeout_ns. */
TEST(St22PipelineTxWakeReview, TwoFrameDonesWakeTwoBlockedGets) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 2;
  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_enable_blocking(ctx, kTimeoutNs);

  /* all frames go to the transport */
  for (int i = 0; i < kFrameCnt; i++) {
    struct st_frame* f = ut22p_tx_get_frame(ctx);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(ut22p_tx_put_frame(ctx, f), 0);
  }
  uint16_t idx[kFrameCnt];
  for (int i = 0; i < kFrameCnt; i++) ASSERT_EQ(ut22p_tx_next_frame(ctx, &idx[i]), 0);

  struct st_frame* got[2] = {};
  uint64_t elapsed_ns[2] = {};
  auto getter = [&](int i) {
    const auto t0 = std::chrono::steady_clock::now();
    got[i] = ut22p_tx_get_frame(ctx);
    elapsed_ns[i] = ElapsedNs(t0);
  };
  std::thread g0(getter, 0);
  std::thread g1(getter, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  /* two frame dones, back to back */
  ASSERT_EQ(ut22p_tx_frame_done(ctx, idx[0]), 0);
  ASSERT_EQ(ut22p_tx_frame_done(ctx, idx[1]), 0);
  g0.join();
  g1.join();

  for (int i = 0; i < 2; i++) {
    EXPECT_NE(got[i], nullptr) << "getter " << i;
    EXPECT_LT(elapsed_ns[i], kTimeoutNs / 2)
        << "getter " << i << " slept the block timeout while a frame was FREE";
  }
  ut22p_tx_ctx_destroy(ctx);
}

/* An encoder wake_block posted while no encoder get waits is kept through a
 * get that claims a frame on the fast path, so the next empty get returns NULL
 * at once. PR commit 7f3afbd8 fixes this exact fault for the app RX gets
 * ("the next get now spends it on either path"), but not for the encoder. */
TEST(St22PipelineTxWakeReview, EncoderWakeIsSpentByAFastPathGet) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(2);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_set_encoder(ctx);
  ut22p_tx_ctx_enable_encode_blocking(ctx, kShortTimeoutNs);

  ut22p_tx_encode_wake_block(ctx); /* no encoder get waits */

  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frame), 0);
  ASSERT_EQ(ut22p_tx_encode_get_frame(ctx), 0); /* fast path, no wait */

  const auto t0 = std::chrono::steady_clock::now();
  int ret = ut22p_tx_encode_get_frame(ctx);
  const uint64_t elapsed_ns = ElapsedNs(t0);

  EXPECT_EQ(ret, -EBUSY);
  EXPECT_GT(elapsed_ns, kShortTimeoutNs / 2)
      << "the empty encode get returned at once on the old wake_block token";
  ut22p_tx_ctx_destroy(ctx);
}
