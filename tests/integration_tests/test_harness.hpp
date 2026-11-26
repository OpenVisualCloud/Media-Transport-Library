/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once

#include <cstdint>
#include <vector>

#include "harness/lifecycle.hpp"
#include "harness/memory.hpp"
#include "harness/sessions.hpp"
#include "tests.hpp"

/*
 * Shared fast, deterministic harness for MTL integration tests.
 *
 * Replaces blind sleep(10) / sleep(5) / sleep(ST20_TRAIN_TIME_S * sessions)
 * loops with event-driven frame/packet waits and bounded FPS measurement
 * windows, and re-exports the RAII session, memory, and lifecycle utilities
 * from tests/integration_tests/harness/.
 */
namespace mtl_test {

/* Precondition guards: report honest GTEST_SKIP() instead of silent return. */
#define MTL_TEST_REQUIRE_PORTS(ctx, n)                              \
  do {                                                              \
    if ((ctx)->para.num_ports < (n))                                \
      GTEST_SKIP() << "Test requires at least " << (n) << " ports"; \
  } while (0)

#define MTL_TEST_REQUIRE_DMA(ctx)                                                   \
  do {                                                                              \
    if (!st_test_dma_available(ctx)) GTEST_SKIP() << "DMA device is not available"; \
  } while (0)

#define MTL_TEST_REQUIRE_IOVA_VA(ctx)                                                  \
  do {                                                                                 \
    if ((ctx)->iova == MTL_IOVA_MODE_PA) GTEST_SKIP() << "Test requires IOVA VA mode"; \
  } while (0)

#define MTL_TEST_REQUIRE_DPDK_PMD(ctx, port)          \
  do {                                                \
    if ((ctx)->para.pmd[(port)] != MTL_PMD_DPDK_USER) \
      GTEST_SKIP() << "Test requires DPDK user PMD";  \
  } while (0)

#define MTL_TEST_REQUIRE_LEVEL(ctx, lvl)                                                 \
  do {                                                                                   \
    if ((lvl) < (ctx)->level) GTEST_SKIP() << "Test level is below the requested level"; \
  } while (0)

template <typename T>
inline T relaxed_load(const T* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

inline int rx_frames(const tests_context* c) {
  return relaxed_load(&c->fb_rec);
}

inline int tx_frames(const tests_context* c) {
  return relaxed_load(&c->fb_send);
}

inline int tx_done_frames(const tests_context* c) {
  return relaxed_load(&c->fb_send_done);
}

inline int vsync_count(const tests_context* c) {
  return relaxed_load(&c->vsync_cnt);
}

inline int rx_packets(const tests_context* c) {
  return relaxed_load(&c->packet_rec);
}

inline int sha_checked_frames(const tests_context* c) {
  return relaxed_load(&c->check_sha_frame_cnt);
}

/* Poll pred() every poll_ms until true or timeout_ms expires. */
template <typename Pred>
inline bool wait_until(Pred&& pred, int timeout_ms, int poll_ms = 5) {
  const int clamped_timeout_ms = timeout_ms > 0 ? timeout_ms : 0;
  const int clamped_poll_ms = poll_ms > 0 ? poll_ms : 1;
  const uint64_t deadline = st_test_get_monotonic_time() +
                            static_cast<uint64_t>(clamped_timeout_ms) * NS_PER_MS;
  while (true) {
    if (pred()) return true;
    if (st_test_get_monotonic_time() >= deadline) return false;
    st_usleep(static_cast<unsigned int>(clamped_poll_ms) * 1000);
  }
}

/* Wait until every context in ctxs reaches get_val(c) >= target. */
template <typename CtxPtr, typename Getter>
inline bool wait_for_all(const std::vector<CtxPtr>& ctxs, Getter&& get_val, int target,
                         int timeout_ms = 8000, int poll_ms = 5) {
  return wait_until(
      [&]() {
        for (const auto& c : ctxs) {
          if (!c || get_val(c) < target) return false;
        }
        return true;
      },
      timeout_ms, poll_ms);
}

/*
 * Pacing-aware rate measurement window.
 *
 * 1. Waits until every session reaches warmup_frames (organically absorbs RL
 *    pacing training and TX/RX startup without hardcoded sleep(N)).
 * 2. Snapshots (t0_ns, count0) for every session.
 * 3. Waits until every session advances by at least min_delta_frames AND the
 *    window has run at least min_window_ms.
 * 4. Computes exact steady-state rate = (count1 - count0) / elapsed_sec.
 *
 * Math: 30 frames over 1.0 s has <= 1/30 (3.3%) edge quantization error, well
 * inside the standard +/-10% integration FPS tolerance, while running 6-15x
 * faster than legacy sleep(10) loops.
 */
struct RateWindowResult {
  std::vector<double> rate;
  bool warmed_up = false;
  bool complete = false;
};

template <typename CtxPtr, typename Getter>
inline RateWindowResult measure_rate_window(const std::vector<CtxPtr>& ctxs,
                                            Getter&& get_cnt, int warmup_frames = 3,
                                            int min_delta_frames = 25,
                                            int min_window_ms = 1000,
                                            int warmup_timeout_ms = 6000,
                                            int window_timeout_ms = 6000) {
  RateWindowResult res;
  const size_t n = ctxs.size();
  res.rate.assign(n, 0.0);
  if (n == 0) {
    res.warmed_up = true;
    res.complete = true;
    return res;
  }

  res.warmed_up = wait_for_all(ctxs, get_cnt, warmup_frames, warmup_timeout_ms);
  if (!res.warmed_up) return res;

  std::vector<int> start_cnt(n, 0);
  const uint64_t t0 = st_test_get_monotonic_time();
  for (size_t i = 0; i < n; i++) {
    start_cnt[i] = ctxs[i] ? get_cnt(ctxs[i]) : 0;
  }

  const int clamped_min_window_ms = min_window_ms > 0 ? min_window_ms : 0;
  const uint64_t min_end_ns =
      t0 + static_cast<uint64_t>(clamped_min_window_ms) * NS_PER_MS;
  res.complete = wait_until(
      [&]() {
        if (st_test_get_monotonic_time() < min_end_ns) return false;
        for (size_t i = 0; i < n; i++) {
          if (!ctxs[i]) continue;
          if (get_cnt(ctxs[i]) - start_cnt[i] < min_delta_frames) return false;
        }
        return true;
      },
      window_timeout_ms);

  const uint64_t t1 = st_test_get_monotonic_time();
  const double dt = static_cast<double>(t1 - t0) / NS_PER_S;
  if (dt > 0.0) {
    for (size_t i = 0; i < n; i++) {
      if (!ctxs[i]) continue;
      const int delta = get_cnt(ctxs[i]) - start_cnt[i];
      res.rate[i] = static_cast<double>(delta) / dt;
    }
  }
  return res;
}

/*
 * Dual-counter variant for tests that verify both frame rate and vsync rate
 * (such as st20p_rx_digest_test) in one short window.
 */
struct DualRateWindowResult {
  std::vector<double> tx_fps;
  std::vector<double> rx_fps;
  std::vector<double> tx_vsync;
  std::vector<double> rx_vsync;
  bool warmed_up = false;
  bool complete = false;
};

inline DualRateWindowResult measure_pipeline_window(
    const std::vector<tests_context*>& tx_ctxs,
    const std::vector<tests_context*>& rx_ctxs, int warmup_frames = 3,
    int min_delta_frames = 25, int min_window_ms = 1000, int warmup_timeout_ms = 6000,
    int window_timeout_ms = 6000) {
  DualRateWindowResult res;
  const size_t n_tx = tx_ctxs.size();
  const size_t n_rx = rx_ctxs.size();
  res.tx_fps.assign(n_tx, 0.0);
  res.rx_fps.assign(n_rx, 0.0);
  res.tx_vsync.assign(n_tx, 0.0);
  res.rx_vsync.assign(n_rx, 0.0);
  if (n_rx == 0) {
    res.warmed_up = true;
    res.complete = true;
    return res;
  }

  res.warmed_up = wait_for_all(rx_ctxs, rx_frames, warmup_frames, warmup_timeout_ms);
  if (!res.warmed_up) return res;

  std::vector<int> tx0(n_tx, 0), tv0(n_tx, 0);
  std::vector<int> rx0(n_rx, 0), rv0(n_rx, 0);
  const uint64_t t0 = st_test_get_monotonic_time();
  for (size_t i = 0; i < n_tx; i++) {
    if (!tx_ctxs[i]) continue;
    tx0[i] = tx_frames(tx_ctxs[i]);
    tv0[i] = vsync_count(tx_ctxs[i]);
  }
  for (size_t i = 0; i < n_rx; i++) {
    if (!rx_ctxs[i]) continue;
    rx0[i] = rx_frames(rx_ctxs[i]);
    rv0[i] = vsync_count(rx_ctxs[i]);
  }

  const int clamped_min_window_ms = min_window_ms > 0 ? min_window_ms : 0;
  const uint64_t min_end_ns =
      t0 + static_cast<uint64_t>(clamped_min_window_ms) * NS_PER_MS;
  res.complete = wait_until(
      [&]() {
        if (st_test_get_monotonic_time() < min_end_ns) return false;
        for (size_t i = 0; i < n_rx; i++) {
          if (!rx_ctxs[i]) continue;
          if (rx_frames(rx_ctxs[i]) - rx0[i] < min_delta_frames) return false;
        }
        return true;
      },
      window_timeout_ms);

  const uint64_t t1 = st_test_get_monotonic_time();
  const double dt = static_cast<double>(t1 - t0) / NS_PER_S;
  if (dt > 0.0) {
    for (size_t i = 0; i < n_tx; i++) {
      if (!tx_ctxs[i]) continue;
      res.tx_fps[i] = static_cast<double>(tx_frames(tx_ctxs[i]) - tx0[i]) / dt;
      res.tx_vsync[i] = static_cast<double>(vsync_count(tx_ctxs[i]) - tv0[i]) / dt;
    }
    for (size_t i = 0; i < n_rx; i++) {
      if (!rx_ctxs[i]) continue;
      res.rx_fps[i] = static_cast<double>(rx_frames(rx_ctxs[i]) - rx0[i]) / dt;
      res.rx_vsync[i] = static_cast<double>(vsync_count(rx_ctxs[i]) - rv0[i]) / dt;
    }
  }
  return res;
}

}  // namespace mtl_test
