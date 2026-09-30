/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st*p_tx_wake_block() must end a get_frame() that is blocked in
 * BLOCK_GET mode, so an app can stop its frame thread without waiting for
 * block_timeout_ns. A wake posted while no thread waits must still not cut
 * a later wait short (the StaleWake* cases in the *_blocking_test.cpp files).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pipeline/st20p_tx_harness.h"
#include "pipeline/st22p_tx_harness.h"
#include "pipeline/st30p_tx_harness.h"
#include "pipeline/st40p_tx_harness.h"

namespace {

constexpr uint64_t kBlockTimeoutNs = 2ULL * 1000 * 1000 * 1000; /* 2 s */

/* The only frame is held by the app, so get_frame() can only return on a
 * wake or on the timeout. The wake repeats until the waiter returns: one
 * wake sent before the waiter reaches the wait would be lost. */
template <typename Ctx, typename Frame>
void ExpectWakeEndsBlockedGet(Ctx* ctx, Frame* (*get_frame)(Ctx*),
                              void (*wake_block)(Ctx*)) {
  Frame* held = get_frame(ctx);
  ASSERT_NE(held, nullptr);

  std::atomic<bool> returned{false};
  Frame* frame = held;
  std::chrono::steady_clock::duration elapsed{};
  std::thread waiter([&]() {
    const auto t0 = std::chrono::steady_clock::now();
    frame = get_frame(ctx);
    elapsed = std::chrono::steady_clock::now() - t0;
    returned.store(true);
  });

  const auto wake_deadline =
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(kBlockTimeoutNs) / 2;
  while (!returned.load() && std::chrono::steady_clock::now() < wake_deadline) {
    wake_block(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  waiter.join();

  EXPECT_EQ(frame, nullptr) << "no frame was freed, get_frame must return NULL";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            static_cast<long long>(kBlockTimeoutNs / 2 / 1000000))
      << "wake_block did not end the blocked get_frame, it waited for the timeout";
}

}  // namespace

TEST(PipelineTxWakeBlock, St20pWakeEndsBlockedGetFrame) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";
  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut20p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  ExpectWakeEndsBlockedGet(ctx, ut20p_tx_get_frame, ut20p_tx_wake_block);

  ut20p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxWakeBlock, St22pWakeEndsBlockedGetFrame) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";
  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut22p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  ExpectWakeEndsBlockedGet(ctx, ut22p_tx_get_frame, ut22p_tx_wake_block);

  ut22p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxWakeBlock, St30pWakeEndsBlockedGetFrame) {
  ASSERT_EQ(ut30p_tx_init(), 0) << "EAL init failed";
  ut30p_tx_ctx* ctx = ut30p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut30p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  ExpectWakeEndsBlockedGet(ctx, ut30p_tx_get_frame, ut30p_tx_wake_block);

  ut30p_tx_ctx_destroy(ctx);
}

TEST(PipelineTxWakeBlock, St40pWakeEndsBlockedGetFrame) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";
  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(1);
  ASSERT_NE(ctx, nullptr);
  ut40p_tx_ctx_enable_blocking(ctx, kBlockTimeoutNs);

  ExpectWakeEndsBlockedGet(ctx, ut40p_tx_get_frame, ut40p_tx_wake_block);

  ut40p_tx_ctx_destroy(ctx);
}
