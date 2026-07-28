/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) TX pipeline-layer concurrency test for the lock-free
 * framebuffer ring.
 *
 * get_frame() claims a FREE slot by scanning tx_st20p_next_available() for a
 * candidate and then issuing one compare-exchange on it. The CAS itself
 * prevents two producers from ever owning the same slot, but a naive
 * implementation gives up and reports "no frame available" the instant that
 * single CAS loses a race -- even when other FREE slots exist -- instead of
 * continuing the scan for another candidate.
 *
 * next_available() always scans the ring from index 0, so when many threads
 * call get_frame() at (approximately) the same instant they overwhelmingly
 * converge on the same lowest-index FREE candidate. With N producer threads
 * releasing at once against N FREE slots, a "scan once + single CAS attempt"
 * implementation lets only the CAS winner succeed per candidate and leaves
 * the rest reporting "no frame" -- a spurious claim failure -- even though
 * N-1 other slots are still FREE. The fix is for the claim itself to retry
 * the scan on a lost race, so a single get_frame() call only returns NULL
 * once every slot has genuinely been checked and found unavailable.
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

/* Pin a worker to its own core so all producers are genuinely running in
 * parallel when they hit the barrier release, instead of the scheduler
 * serializing them onto one core. Core 0 is skipped on purpose: ut_eal_init()
 * starts DPDK with "-c1", which pins the calling (main) thread to core 0.
 * Best-effort: failure leaves the thread unpinned. */
inline void pin_worker(std::thread& t, int slot) {
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  if (nproc <= 1) return;
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(1 + (slot % (int)(nproc - 1)), &one);
  pthread_setaffinity_np(t.native_handle(), sizeof(one), &one);
}

}  // namespace

TEST(St20PipelineTxConcurrency, MultiProducerNoSpuriousClaimFailure) {
  ASSERT_EQ(ut20p_tx_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kThreads = kFrameCnt; /* one thread per slot: none should miss */
  constexpr int kTrials = 200;
  constexpr auto kRunBudget = std::chrono::seconds(45);

  ut20p_tx_ctx* ctx = ut20p_tx_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  int spurious_failures = 0;
  int ownership_violations = 0;

  const auto start = std::chrono::steady_clock::now();
  for (int trial = 0; trial < kTrials; trial++) {
    ASSERT_LT(std::chrono::steady_clock::now() - start, kRunBudget)
        << "deadlock/livelock across trials";

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<struct st_frame*> results(kThreads, nullptr);
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&, t]() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!go.load(std::memory_order_acquire)) {
        }
        /* single-shot: exactly one get_frame() call per thread, no retry. */
        results[t] = ut20p_tx_get_frame(ctx);
      });
      pin_worker(threads[t], t);
    }
    while (ready.load(std::memory_order_relaxed) < kThreads) {
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    int got = 0;
    std::vector<int> idx_owner(kFrameCnt, -1);
    for (int t = 0; t < kThreads; t++) {
      if (!results[t]) continue;
      got++;
      int idx = ut20p_tx_frame_idx(results[t]);
      if (idx_owner[idx] != -1) ownership_violations++; /* two threads, one slot */
      idx_owner[idx] = t;
    }
    if (got < kFrameCnt) spurious_failures += (kFrameCnt - got);

    /* release every claimed slot before the next trial */
    for (auto* f : results) {
      if (f) {
        ASSERT_EQ(ut20p_tx_put_frame_abort(ctx, f), 0);
      }
    }
  }

  EXPECT_EQ(ownership_violations, 0) << "two producer threads claimed the same slot";
  EXPECT_EQ(spurious_failures, 0)
      << "get_frame() reported \"no frame\" while a FREE slot still existed";

  ut20p_tx_ctx_destroy(ctx);
}
