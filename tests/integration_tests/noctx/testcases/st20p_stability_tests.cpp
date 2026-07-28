/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Long-running multi-threaded stability test for the ST20p TX pipeline
 * framebuffer ring (lib/src/st2110/pipeline/st20_pipeline_tx.c).
 *
 * Many application producer threads pound st20p_tx_get_frame /
 * st20p_tx_put_frame on ONE handle while the real transport tasklet consumes
 * via the get_next_frame callback. This is the on-hardware counterpart of the
 * unit-level St20PipelineConcurrency tests: it validates the same lock-free
 * ownership protocol (FREE->IN_USER CAS, CONVERTED->IN_TRANSMITTING CAS) under
 * a genuine DPDK transport, real pacing and real memory for a sustained
 * duration.
 *
 * A per-framebuffer ownership token (holder[]) detects any moment where two
 * actors believe they own the same slot. The buffer is written while owned so
 * ASan catches any use-after-free / out-of-bounds if the protocol ever hands
 * the same slot to two threads.
 *
 * Duration defaults to 30 minutes; override with NOCTX_STABILITY_SECONDS
 * (e.g. NOCTX_STABILITY_SECONDS=30 for a smoke run). Thread and framebuffer
 * counts are overridable via NOCTX_STABILITY_PRODUCERS / NOCTX_STABILITY_FBS.
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

namespace {

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  int parsed = std::atoi(v);
  return parsed > 0 ? parsed : fallback;
}

}  // namespace

TEST_F(NoCtxTest, st20p_tx_multithread_stability) {
  initDefaultContext();

  /* Small ring + many producers = maximum contention on the claim CAS. */
  const int kFrameCnt = envInt("NOCTX_STABILITY_FBS", 4);
  const int kProducers = envInt("NOCTX_STABILITY_PRODUCERS", 8);
  const int kSeconds = envInt("NOCTX_STABILITY_SECONDS", 1800); /* 30 min default */

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* h) { return new St20pDefaultTimestamp(h); },
      [kFrameCnt](St20pHandler* h) {
        h->sessionsOpsTx.framebuff_cnt = (uint16_t)kFrameCnt;
      });

  auto* handler = bundle.handler;
  ASSERT_NE(handler, nullptr);
  st20p_tx_handle tx = handler->sessionsHandleTx;
  ASSERT_NE(tx, nullptr);

  /* Map each framebuffer address to its slot index so a returned st_frame can
   * be attributed to a specific slot for ownership tracking. */
  std::unordered_map<void*, int> addr2idx;
  for (int i = 0; i < kFrameCnt; i++) {
    void* a = st20p_tx_get_fb_addr(tx, i);
    ASSERT_NE(a, nullptr) << "framebuffer " << i << " has no address";
    addr2idx[a] = i;
  }

  std::vector<std::atomic<int>> holder(kFrameCnt);
  for (auto& h : holder) h.store(0);

  const size_t frameSize = st20p_tx_frame_size(tx);
  ASSERT_GT(frameSize, 0u);

  std::atomic<bool> stop{false};
  std::atomic<bool> ownership_violation{false};
  std::atomic<bool> api_error{false};
  std::atomic<bool> bad_frame{false};
  std::atomic<uint64_t> produced{0};

  StartFakePtpClock();
  ASSERT_GE(mtl_start(ctx->handle), 0);

  auto producer = [&](int id) {
    while (!stop.load(std::memory_order_relaxed)) {
      struct st_frame* f = st20p_tx_get_frame(tx);
      if (!f) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      if (!f->addr[0]) {
        bad_frame.store(true);
        continue;
      }
      auto it = addr2idx.find(f->addr[0]);
      if (it == addr2idx.end()) {
        bad_frame.store(true); /* returned a buffer we never registered */
      } else {
        int idx = it->second;
        /* Claim the slot; a non-zero previous owner means a double-claim. */
        if (holder[idx].exchange(id) != 0) ownership_violation.store(true);
        /* Write while owned: ASan flags any aliasing to another live slot. */
        std::memset(f->addr[0], (uint8_t)id, 64);
        if (holder[idx].exchange(0) != id) ownership_violation.store(true);
      }
      f->data_size = frameSize;
      if (st20p_tx_put_frame(tx, f) < 0) api_error.store(true);
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  for (int p = 1; p <= kProducers; p++) threads.emplace_back(producer, p);

  const auto start = std::chrono::steady_clock::now();
  const auto budget = std::chrono::seconds(kSeconds);
  uint64_t last_report = 0;
  while (std::chrono::steady_clock::now() - start < budget) {
    if (ownership_violation.load() || api_error.load() || bad_frame.load() ||
        HasFailure())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    /* Heartbeat every ~60s so a 30-minute run shows liveness. */
    uint64_t elapsed = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    if (elapsed - last_report >= 60) {
      last_report = elapsed;
      fprintf(stderr, "st20p_tx_multithread_stability: %llus, produced %llu\n",
              (unsigned long long)elapsed, (unsigned long long)produced.load());
    }
  }

  stop.store(true);
  for (auto& t : threads) t.join();

  EXPECT_FALSE(ownership_violation.load())
      << "two threads owned the same framebuffer simultaneously";
  EXPECT_FALSE(api_error.load()) << "st20p_tx_put_frame returned an error";
  EXPECT_FALSE(bad_frame.load()) << "get_frame returned a null/unregistered buffer";
  EXPECT_GT(produced.load(), 0u) << "no frames were produced";

  struct st20_tx_user_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (st20p_tx_get_session_stats(tx, &stats) == 0) {
    EXPECT_LE(stats.common.stat_frames_sent, produced.load())
        << "transport reported more sent than produced (duplicate transmit)";
    fprintf(stderr,
            "st20p_tx_multithread_stability: produced %llu, sent %llu, dropped %llu\n",
            (unsigned long long)produced.load(),
            (unsigned long long)stats.common.stat_frames_sent,
            (unsigned long long)stats.common.stat_frames_dropped);
  }

  handler->stopSession();
}
