/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Adversarial ST22p (compressed video) TX pipeline concurrency test.
 *
 * TxConcurrentConsumersNoDoubleClaim drives tx_st22p_next_frame from many
 * threads at once. The claim ENCODED -> IN_TRANSMITTING was a scan
 * (newest_available) followed by a plain atomic store -- NOT a CAS -- so two
 * consumers could both observe the same ENCODED slot and both claim it. FAILS
 * before the claim is a CAS, PASSES after. Direct mirror of the st20p stress
 * test that first exposed this class of bug.
 */

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pipeline/st22p_tx_harness.h"

namespace {

constexpr auto kRunBudget = std::chrono::seconds(45);

inline void dwell() {
  for (volatile int i = 0; i < 64; i++) {
  }
}

inline void pin_worker(std::thread& t, int slot) {
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  if (nproc <= 1) return;
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(1 + (slot % (int)(nproc - 1)), &one);
  pthread_setaffinity_np(t.native_handle(), sizeof(one), &one);
}

}  // namespace

TEST(St22PipelineConcurrencyStress, TxConcurrentConsumersNoDoubleClaim) {
  ASSERT_EQ(ut22p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kProducers = 3;
  constexpr int kConsumers = 4;
  constexpr int kTarget = 50000;

  ut22p_tx_ctx* ctx = ut22p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  /* holder[idx]: 0 free/queued, >0 owning producer id, -1 a consumer. */
  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  std::atomic<int> to_produce{kTarget};
  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false};

  auto producer = [&](int id) {
    while (!stop.load(std::memory_order_relaxed)) {
      if (to_produce.fetch_sub(1, std::memory_order_relaxed) <= 0) break;
      struct st_frame* f = nullptr;
      while ((f = ut22p_tx_get_frame(ctx)) == nullptr) {
        if (stop.load(std::memory_order_relaxed)) return;
      }
      int idx = ut22p_tx_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut22p_tx_put_frame(ctx, f) != 0) api_error.store(true);
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto consumer = [&]() {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      uint16_t idx = 0;
      if (ut22p_tx_next_frame(ctx, &idx) != 0) continue; /* nothing ENCODED */
      /* If two consumers claimed the same slot, the second exchange sees -1. */
      if (holder[idx].exchange(-1) != 0) ownership_violation.store(true);
      dwell();
      if (holder[idx].exchange(0) != -1) ownership_violation.store(true);
      if (ut22p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int c = 0; c < kConsumers; c++) threads.emplace_back(consumer);
  for (int p = 1; p <= kProducers; p++) threads.emplace_back(producer, p);
  for (size_t i = 0; i < threads.size(); i++) pin_worker(threads[i], (int)i);

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  while (consumed.load(std::memory_order_relaxed) < kTarget) {
    if (ownership_violation.load() || api_error.load()) break; /* fail fast */
    if (std::chrono::steady_clock::now() - start > kRunBudget) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  for (auto& t : threads) t.join();

  EXPECT_FALSE(timed_out) << "deadlock/livelock: consumed " << consumed.load() << "/"
                          << kTarget;
  EXPECT_FALSE(ownership_violation.load())
      << "two consumer threads claimed the same framebuffer simultaneously";
  EXPECT_FALSE(api_error.load()) << "frame_done returned an error (double-claim)";
  EXPECT_TRUE(ut22p_tx_all_free(ctx)) << "framebuffer leaked (not back to FREE)";

  ut22p_tx_ctx_destroy(ctx);
}
