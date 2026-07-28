/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST20p (video) RX pipeline-layer concurrency test for the lock-free
 * framebuffer ring.
 *
 * st20p_rx_get_frame() claims a delivered slot by scanning
 * rx_st20p_next_available() for a candidate (CONVERTED, or READY in the
 * internal-converter path) and then issuing one compare-exchange to move it
 * to IN_USER. The CAS itself prevents two consumers from ever owning the same
 * slot, but a naive implementation gives up and reports "no frame available"
 * the instant that single CAS loses a race -- even when other delivered slots
 * exist -- instead of continuing the scan for another candidate.
 *
 * next_available() scans the ring from a shared consumer cursor, so when many
 * threads call get_frame() at (approximately) the same instant they converge
 * on the same candidate. With N consumer threads releasing at once against N
 * delivered slots, a "scan once + single CAS attempt" implementation lets only
 * the CAS winner succeed per candidate and leaves the rest reporting "no
 * frame" -- a spurious claim failure -- even though N-1 other slots are still
 * deliverable. The fix is for the claim itself to retry the scan on a lost
 * race, so a single get_frame() call only returns NULL once every slot has
 * genuinely been checked and found unavailable.
 */

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pipeline/st20p_harness.h"

namespace {

/* Pin a worker to its own core so all consumers are genuinely running in
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

TEST(St20PipelineRxConcurrency, MultiConsumerNoSpuriousClaimFailure) {
  ASSERT_EQ(ut20p_init(), 0) << "EAL init failed";

  constexpr int kFrameCnt = 8;
  constexpr int kThreads = kFrameCnt; /* one thread per slot: none should miss */
  constexpr int kTrials = 200;
  constexpr auto kRunBudget = std::chrono::seconds(45);

  ut20p_ctx* ctx = ut20p_ctx_create(kFrameCnt);
  ASSERT_NE(ctx, nullptr);

  int spurious_failures = 0;
  int ownership_violations = 0;

  const auto start = std::chrono::steady_clock::now();
  for (int trial = 0; trial < kTrials; trial++) {
    ASSERT_LT(std::chrono::steady_clock::now() - start, kRunBudget)
        << "deadlock/livelock across trials";

    /* Fill the whole ring: each inject drives one FREE slot to CONVERTED
     * (derive path), the state get_frame() consumes. */
    for (int i = 0; i < kFrameCnt; i++) {
      ASSERT_EQ(ut20p_inject_frame(ctx, ST_FRAME_STATUS_COMPLETE, (uint32_t)(i + 1)), 0);
    }

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
        results[t] = ut20p_get_frame(ctx);
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
      int idx = ut20p_frame_idx(results[t]);
      if (idx_owner[idx] != -1) ownership_violations++; /* two threads, one slot */
      idx_owner[idx] = t;
    }
    if (got < kFrameCnt) spurious_failures += (kFrameCnt - got);

    /* Release claimed slots, then single-threaded drain any slots a buggy
     * get_frame() spuriously left behind, so every trial starts from an
     * all-FREE ring regardless of the outcome above (contention is over, so
     * this drain always succeeds). */
    for (auto* f : results) {
      if (f) {
        ASSERT_EQ(ut20p_put_frame(ctx, f), 0);
      }
    }
    struct st_frame* leftover;
    while ((leftover = ut20p_get_frame(ctx)) != nullptr) {
      ASSERT_EQ(ut20p_put_frame(ctx, leftover), 0);
    }
  }

  EXPECT_EQ(ownership_violations, 0) << "two consumer threads claimed the same slot";
  EXPECT_EQ(spurious_failures, 0)
      << "get_frame() reported \"no frame\" while a delivered slot still existed";

  ut20p_ctx_destroy(ctx);
}
