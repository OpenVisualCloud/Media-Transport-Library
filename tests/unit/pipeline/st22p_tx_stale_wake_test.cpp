/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p TX pipeline blocking get_frame() stale-wake regression test.
 *
 * With ST22P_TX_FLAG_BLOCK_GET, each frame done sets block_wake_pending, also
 * when no thread waits. A later get_frame() that finds no free frame sees the
 * old flag, skips the wait, and returns NULL at once, while the transport is
 * about to free a frame. The FFmpeg mtl_st22p muxer (3 frame buffers) stopped
 * after 4 frames with "st22p_tx_get_frame timeout" and EIO.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "pipeline/st22p_tx_harness.h"

TEST(St22PipelineTxBlocking, StaleWakeDoesNotSkipTheWait) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 3;
  constexpr uint64_t kBlockTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  /* The app holds all 3 frames. */
  struct st_frame* frames[kFrameCnt];
  for (int i = 0; i < kFrameCnt; i++) {
    frames[i] = ut22p_tx_get_frame(ctx);
    ASSERT_NE(frames[i], nullptr) << "frame " << i;
  }

  /* The transport sends frame 0. Its frame done wakes a get_frame() that no
   * thread is in, so the wake stays pending. */
  uint16_t idx;
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frames[0]), 0);
  ASSERT_EQ(ut22p_tx_next_frame(ctx, &idx), 0);
  ASSERT_EQ(ut22p_tx_frame_done(ctx, idx), 0);

  /* The free frame is there, so this get does not wait and the stale wake
   * stays set. */
  frames[0] = ut22p_tx_get_frame(ctx);
  ASSERT_NE(frames[0], nullptr);

  /* Frame 1 goes to the transport, which frees it after 100 ms. */
  ASSERT_EQ(ut22p_tx_put_frame(ctx, frames[1]), 0);
  int release_ret[2] = {-1, -1};
  std::thread transport([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint16_t i;
    release_ret[0] = ut22p_tx_next_frame(ctx, &i);
    release_ret[1] = ut22p_tx_frame_done(ctx, i);
  });

  /* No frame is free now. The get must wait for the frame done of frame 1,
   * not return NULL on the stale wake of frame 0. */
  struct st_frame* frame = ut22p_tx_get_frame(ctx);
  transport.join();

  EXPECT_EQ(release_ret[0], 0);
  EXPECT_EQ(release_ret[1], 0);
  EXPECT_NE(frame, nullptr)
      << "get_frame returned NULL at once on a stale wake instead of waiting "
         "for the frame the transport freed";

  ut22p_tx_ctx_destroy(ctx);
}
