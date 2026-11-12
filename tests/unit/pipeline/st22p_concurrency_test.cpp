/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST22p (compressed video) pipeline-layer concurrency tests for the lock-free
 * framebuffer ring. Mirrors st20p_concurrency_test: many threads hammer the
 * atomic claim (compare-exchange) and assert single-ownership, conservation
 * and deadlock-freedom.
 *
 * Both halves run in the derive (no codec) path so the app/transport state
 * machine is exercised in isolation:
 *   TX: N app producers (get_frame/put_frame -> ENCODED) + 1 transport consumer
 *       (next_frame/frame_done).
 *   RX: 1 transport producer (inject_frame -> DECODED) + N app consumers
 *       (get_frame/put_frame).
 */

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/ut_concurrency.h"
#include "pipeline/st22p_harness.h"    /* RX role: ut22p_* */
#include "pipeline/st22p_tx_harness.h" /* TX role: ut22p_tx_* */

namespace {

/* Wall-clock budget for a whole run. With each worker pinned to its own core a
 * correct lock-free design finishes in well under a second; only a genuine
 * livelock/deadlock approaches this. */
constexpr auto kRunBudget = std::chrono::seconds(45);

}  // namespace

/* ------------------------------------------------------------------ TX ---- */

TEST(St22PipelineConcurrency, TxMultiProducerSingleConsumerNoDeadlock) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kProducers = 4;
  constexpr int kTarget = 50000;

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  /* holder[idx]: 0 = free/queued, >0 = owning producer id, -1 = consumer. */
  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  std::atomic<int> to_produce{kTarget}; /* remaining production slots */
  std::atomic<int> produced{0};         /* frames handed to transport */
  std::atomic<int> consumed{0};         /* frames completed by transport */
  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false}; /* worker threads cannot call gtest macros */

  auto producer = [&](int id) {
    while (!stop.load(std::memory_order_relaxed)) {
      if (to_produce.fetch_sub(1, std::memory_order_relaxed) <= 0) break;
      struct st_frame* f = nullptr;
      while ((f = ut22p_tx_get_frame(ctx)) == nullptr) {
        if (stop.load(std::memory_order_relaxed)) return;
      }
      int idx = ut22p_tx_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut22p_tx_put_frame(ctx, f) != 0) api_error.store(true);
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto consumer = [&]() {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      uint16_t idx = 0;
      if (ut22p_tx_next_frame(ctx, &idx) != 0) continue; /* nothing ENCODED yet */
      if (holder[idx].exchange(-1) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != -1) ownership_violation.store(true);
      if (ut22p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(consumer);
  for (int p = 1; p <= kProducers; p++) threads.emplace_back(producer, p);
  for (size_t i = 0; i < threads.size(); i++) ut_pin_worker(threads[i], (int)i);

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  while (consumed.load(std::memory_order_relaxed) < kTarget) {
    if (std::chrono::steady_clock::now() - start > kRunBudget) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  for (auto& t : threads) t.join();

  if (timed_out) {
    fprintf(stderr, "[TX timeout] produced=%d consumed=%d\n", produced.load(),
            consumed.load());
    for (int i = 0; i < kFrameCnt; i++)
      fprintf(stderr, "  buf %d stat=%d holder=%d\n", i, ut22p_tx_frame_stat(ctx, i),
              holder[i].load());
  }

  EXPECT_FALSE(timed_out) << "deadlock/livelock: consumed " << consumed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load()) << "two actors held the same framebuffer";
  EXPECT_FALSE(api_error.load()) << "put_frame/frame_done returned an error";
  EXPECT_EQ(produced.load(), kTarget);
  EXPECT_EQ(consumed.load(), kTarget) << "frames lost or duplicated";
  EXPECT_TRUE(ut22p_tx_all_free(ctx)) << "framebuffer leaked (not back to FREE)";

  ut22p_tx_ctx_destroy(ctx);
}

/* ------------------------------------------------------------------ RX ---- */

TEST(St22PipelineConcurrency, RxSingleProducerMultiConsumerNoDeadlock) {
  ASSERT_EQ(ut22p_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kConsumers = 4;
  constexpr int kTarget = 50000;

  ut22p_ctx* ctx = ut22p_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  /* holder[idx]: 0 = free/queued, >0 = owning consumer id. */
  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false}; /* worker threads cannot call gtest macros */

  auto producer = [&]() {
    uint32_t ts = 1;
    while (produced.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      if (ut22p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, ts++) == 0)
        produced.fetch_add(1, std::memory_order_relaxed);
      /* else -EBUSY: ring full, consumers will drain it; retry. */
    }
  };

  auto consumer = [&](int id) {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      struct st_frame* f = ut22p_get_frame(ctx);
      if (!f) continue; /* nothing DECODED yet */
      int idx = ut22p_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut22p_put_frame(ctx, f) != 0) api_error.store(true);
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(producer);
  for (int c = 1; c <= kConsumers; c++) threads.emplace_back(consumer, c);
  for (size_t i = 0; i < threads.size(); i++) ut_pin_worker(threads[i], (int)i);

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  while (consumed.load(std::memory_order_relaxed) < kTarget) {
    if (std::chrono::steady_clock::now() - start > kRunBudget) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  for (auto& t : threads) t.join();

  if (timed_out) {
    fprintf(stderr, "[RX timeout] produced=%d consumed=%d\n", produced.load(),
            consumed.load());
    for (int i = 0; i < kFrameCnt; i++)
      fprintf(stderr, "  buf %d stat=%d holder=%d\n", i, ut22p_frame_stat(ctx, i),
              holder[i].load());
  }

  EXPECT_FALSE(timed_out) << "deadlock/livelock: consumed " << consumed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load()) << "two consumers held the same framebuffer";
  EXPECT_FALSE(api_error.load()) << "put_frame returned an error";
  EXPECT_EQ(produced.load(), kTarget);
  EXPECT_EQ(consumed.load(), kTarget) << "frames lost or duplicated";
  EXPECT_TRUE(ut22p_framebuff_cnt(ctx) == kFrameCnt);
  EXPECT_GT(ut22p_stub_call_count(), 0U)
      << "st22p_rx_put_frame never reached the transport put path";

  ut22p_ctx_destroy(ctx);
}
