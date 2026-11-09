/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST40p (ancillary) pipeline-layer concurrency tests for the lock-free
 * framebuffer ring. Mirrors st30p_concurrency_test: many threads hammer the
 * atomic claim (compare-exchange) and assert single-ownership, conservation
 * and deadlock-freedom.
 *
 * Topologies:
 *   TX: N app producers (get_frame/put_frame) + 1 transport consumer
 *       (next_frame/frame_done).
 *   RX: 1 transport producer (inject_frame) + N app consumers
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
#include "pipeline/st40p_harness.h"    /* RX role: ut40p_* */
#include "pipeline/st40p_tx_harness.h" /* TX role: ut40p_tx_* */

namespace {

/* Wall-clock budget for a whole run. With each worker pinned to its own core a
 * correct lock-free design finishes in well under a second; only a genuine
 * livelock/deadlock approaches this. */
constexpr auto kRunBudget = std::chrono::seconds(45);

}  // namespace

/* ------------------------------------------------------------------ TX ---- */

TEST(St40PipelineConcurrency, TxMultiProducerSingleConsumerNoDeadlock) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kProducers = 4;
  constexpr int kTarget = 50000;

  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(kFrameCnt);
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
      struct st40_frame_info* f = nullptr;
      while ((f = ut40p_tx_get_frame(ctx)) == nullptr) {
        if (stop.load(std::memory_order_relaxed)) return;
      }
      int idx = ut40p_tx_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut40p_tx_put_frame(ctx, f) != 0) api_error.store(true);
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto consumer = [&]() {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      uint16_t idx = 0;
      if (ut40p_tx_next_frame(ctx, &idx) != 0) continue; /* nothing READY yet */
      if (holder[idx].exchange(-1) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != -1) ownership_violation.store(true);
      if (ut40p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
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
      fprintf(stderr, "  buf %d stat=%d holder=%d\n", i, ut40p_tx_frame_stat(ctx, i),
              holder[i].load());
  }

  EXPECT_FALSE(timed_out) << "deadlock/livelock: consumed " << consumed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load()) << "two actors held the same framebuffer";
  EXPECT_FALSE(api_error.load()) << "put_frame/frame_done returned an error";
  EXPECT_EQ(produced.load(), kTarget);
  EXPECT_EQ(consumed.load(), kTarget) << "frames lost or duplicated";
  EXPECT_EQ(ut40p_tx_stat_frames_sent(ctx), (uint64_t)kTarget);
  EXPECT_TRUE(ut40p_tx_all_free(ctx)) << "framebuffer leaked (not back to FREE)";

  ut40p_tx_ctx_destroy(ctx);
}

/* ------------------------------------------------------------------ RX ---- */

TEST(St40PipelineConcurrency, RxSingleProducerMultiConsumerNoDeadlock) {
  ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kConsumers = 4;
  constexpr int kTarget = 50000;

  ut40p_ctx* ctx = ut40p_ctx_create(kFrameCnt);
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
      if (ut40p_inject_frame(ctx, (void*)(uintptr_t)0x1, ST_FRAME_STATUS_COMPLETE, false,
                             ts++) == 0)
        produced.fetch_add(1, std::memory_order_relaxed);
      /* else -EBUSY: ring full, consumers will drain it; retry. */
    }
  };

  auto consumer = [&](int id) {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      struct st40_frame_info* f = ut40p_get_frame(ctx);
      if (!f) continue; /* nothing READY yet */
      int idx = ut40p_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut40p_put_frame(ctx, f) != 0) api_error.store(true);
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
      fprintf(stderr, "  buf %d stat=%d holder=%d\n", i, ut40p_frame_stat(ctx, i),
              holder[i].load());
  }

  EXPECT_FALSE(timed_out) << "deadlock/livelock: consumed " << consumed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load()) << "two consumers held the same framebuffer";
  EXPECT_FALSE(api_error.load()) << "put_frame returned an error";
  EXPECT_EQ(produced.load(), kTarget);
  EXPECT_EQ(consumed.load(), kTarget) << "frames lost or duplicated";
  EXPECT_TRUE(ut40p_framebuff_cnt(ctx) == kFrameCnt);

  ut40p_ctx_destroy(ctx);
}

/* ---- TX: concurrent transport consumers TOCTOU (READY->IN_TRANSMITTING) ---- */

/* tx_st40p_next_frame scans for a READY frame and writes IN_TRANSMITTING with a
 * plain store — no CAS.  Two concurrent transport threads can both observe READY
 * on the same slot before either writes IN_TRANSMITTING, producing a double-
 * claim.  The fix is a CAS (READY -> IN_TRANSMITTING) in next_frame.
 *
 * Expected: FAIL before the CAS is added; PASS after. */
TEST(St40PipelineConcurrency, TxConcurrentTransportNoDoubleClaim) {
  ASSERT_EQ(ut40p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kTransports = 4;
  constexpr int kTarget = 50000;

  ut40p_tx_ctx* ctx = ut40p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  std::atomic<int> claimed{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false};

  auto refiller = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kFrameCnt; i++) {
        if (ut40p_tx_frame_stat(ctx, i) == 0 /* FREE */) ut40p_tx_set_frame_ready(ctx, i);
      }
    }
  };

  auto transport = [&](int id) {
    while (claimed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      uint16_t idx = 0;
      if (ut40p_tx_next_frame(ctx, &idx) != 0) continue;
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      ut_dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut40p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
      claimed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(refiller);
  for (int t = 1; t <= kTransports; t++) threads.emplace_back(transport, t);
  for (size_t i = 0; i < threads.size(); i++) ut_pin_worker(threads[i], (int)i);

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  while (claimed.load(std::memory_order_relaxed) < kTarget) {
    if (std::chrono::steady_clock::now() - start > kRunBudget) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  for (auto& t : threads) t.join();

  if (timed_out) {
    fprintf(stderr, "[TX-transport timeout] claimed=%d\n", claimed.load());
    for (int i = 0; i < kFrameCnt; i++)
      fprintf(stderr, "  buf %d stat=%d holder=%d\n", i, ut40p_tx_frame_stat(ctx, i),
              holder[i].load());
  }

  EXPECT_FALSE(timed_out) << "deadlock/livelock: claimed " << claimed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load())
      << "two transport threads held the same ST40 framebuffer simultaneously";
  EXPECT_FALSE(api_error.load()) << "frame_done returned an error";

  ut40p_tx_ctx_destroy(ctx);
}
