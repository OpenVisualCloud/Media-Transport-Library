/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Adversarial ST20p (video) TX pipeline concurrency tests: deliberately try to
 * break the lock-free framebuffer ring by driving topologies harder than the
 * library's normal usage.
 *
 *   TxConcurrentConsumersNoDoubleClaim
 *     Runs next_frame from MANY threads at once. The claim
 *     CONVERTED -> IN_TRANSMITTING was a scan (newest_available) followed by a
 *     plain store -- NOT atomic. Two consumers can both observe the same
 *     CONVERTED slot and both claim it. FAILS before the claim is a CAS,
 *     PASSES after. Mirrors the Tx/RxConcurrentConverters CAS tests.
 *
 *   TxMixedPutAbortNoViolation
 *     Full supported topology under one roof: N producers randomly complete
 *     (put_frame) or cancel (put_frame_abort) each slot, plus one transport
 *     consumer. Exercises the IN_USER -> FREE abort edge racing the
 *     FREE -> IN_USER reclaim, which the base suite never touches.
 */

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pipeline/st20p_tx_harness.h"

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

TEST(St20PipelineConcurrencyStress, TxConcurrentConsumersNoDoubleClaim) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kProducers = 3;
  constexpr int kConsumers = 4;
  constexpr int kTarget = 50000;

  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(kFrameCnt);
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
      while ((f = ut20p_tx_get_frame(ctx)) == nullptr) {
        if (stop.load(std::memory_order_relaxed)) return;
      }
      int idx = ut20p_tx_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      dwell();
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (ut20p_tx_put_frame(ctx, f) != 0) api_error.store(true);
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  };

  auto consumer = [&]() {
    while (consumed.load(std::memory_order_relaxed) < kTarget) {
      if (stop.load(std::memory_order_relaxed)) break;
      uint16_t idx = 0;
      if (ut20p_tx_next_frame(ctx, &idx) != 0) continue; /* nothing CONVERTED */
      /* If two consumers claimed the same slot, the second exchange sees -1. */
      if (holder[idx].exchange(-1) != 0) ownership_violation.store(true);
      dwell();
      if (holder[idx].exchange(0) != -1) ownership_violation.store(true);
      if (ut20p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
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
  EXPECT_TRUE(ut20p_tx_all_free(ctx)) << "framebuffer leaked (not back to FREE)";

  ut20p_tx_ctx_destroy(ctx);
}

TEST(St20PipelineConcurrencyStress, TxMixedPutAbortNoViolation) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kProducers = 6;
  constexpr int kTarget = 100000;

  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  std::atomic<int> to_get{kTarget};            /* remaining get_frame attempts */
  std::atomic<int> producers_live{kProducers}; /* producers still running */
  std::atomic<int> aborted{0};
  std::atomic<int> put{0}; /* handed to consumer */
  std::atomic<int> transmitted{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false};

  auto producer = [&](int id) {
    uint32_t rng = 0x9e3779b9u * (uint32_t)id + 1u;
    while (!stop.load(std::memory_order_relaxed)) {
      if (to_get.fetch_sub(1, std::memory_order_relaxed) <= 0) break;
      struct st_frame* f = nullptr;
      while ((f = ut20p_tx_get_frame(ctx)) == nullptr) {
        if (stop.load(std::memory_order_relaxed)) {
          producers_live.fetch_sub(1, std::memory_order_release);
          return;
        }
      }
      int idx = ut20p_tx_frame_idx(f);
      if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
      dwell();
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      bool do_abort = (rng & 3u) == 0u; /* ~25% of frames cancelled */
      if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      if (do_abort) {
        if (ut20p_tx_put_frame_abort(ctx, f) != 0) api_error.store(true);
        aborted.fetch_add(1, std::memory_order_relaxed);
      } else {
        if (ut20p_tx_put_frame(ctx, f) != 0) api_error.store(true);
        put.fetch_add(1, std::memory_order_relaxed);
      }
    }
    producers_live.fetch_sub(1, std::memory_order_release);
  };

  /* `put` is only final once every producer has exited; until then the consumer
   * must keep draining even if it momentarily catches up. */
  auto done = [&]() {
    return producers_live.load(std::memory_order_acquire) == 0 &&
           transmitted.load(std::memory_order_relaxed) >=
               put.load(std::memory_order_relaxed);
  };

  auto consumer = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      if (done()) break;
      uint16_t idx = 0;
      if (ut20p_tx_next_frame(ctx, &idx) != 0) continue;
      if (holder[idx].exchange(-1) != 0) ownership_violation.store(true);
      dwell();
      if (holder[idx].exchange(0) != -1) ownership_violation.store(true);
      if (ut20p_tx_frame_done(ctx, idx) != 0) api_error.store(true);
      transmitted.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(consumer);
  for (int p = 1; p <= kProducers; p++) threads.emplace_back(producer, p);
  for (size_t i = 0; i < threads.size(); i++) pin_worker(threads[i], (int)i);

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  while (!done()) {
    if (std::chrono::steady_clock::now() - start > kRunBudget) {
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  for (auto& t : threads) t.join();

  EXPECT_FALSE(timed_out) << "deadlock/livelock: transmitted " << transmitted.load()
                          << " put " << put.load();
  EXPECT_FALSE(ownership_violation.load())
      << "two actors held the same framebuffer during mixed put/abort";
  EXPECT_FALSE(api_error.load()) << "put/abort/frame_done returned an error";
  EXPECT_EQ(aborted.load() + put.load(), kTarget) << "frames lost between get and put";
  EXPECT_EQ(transmitted.load(), put.load()) << "transmitted != put (lost/dup on wire)";
  EXPECT_EQ(ut20p_tx_stat_frames_sent(ctx), (uint64_t)put.load());
  EXPECT_TRUE(ut20p_tx_all_free(ctx)) << "framebuffer leaked (not back to FREE)";

  ut20p_tx_ctx_destroy(ctx);
}
