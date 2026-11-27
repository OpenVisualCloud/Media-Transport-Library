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
 * Replaces blind sleep(N) loops with event-driven waits and bounded rate
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

inline int sha_checked_frames(const tests_context* c) {
  return relaxed_load(&c->check_sha_frame_cnt);
}

/* Poll pred() every poll_ms until true or timeout_ms expires. */
template <typename Pred>
inline bool wait_until(Pred&& pred, int timeout_ms, int poll_ms = 5) {
  const uint64_t deadline =
      st_test_get_monotonic_time() +
      static_cast<uint64_t>(timeout_ms > 0 ? timeout_ms : 0) * NS_PER_MS;
  const unsigned int sleep_us =
      static_cast<unsigned int>(poll_ms > 0 ? poll_ms : 1) * 1000;
  while (true) {
    if (pred()) return true;
    if (st_test_get_monotonic_time() >= deadline) return false;
    st_usleep(sleep_us);
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

namespace detail {

struct WindowTiming {
  bool warmed_up = false;
  bool complete = false;
  double dt_s = 0.0;
};

template <typename CtxPtr, typename Getter, typename SnapshotFn>
inline WindowTiming run_rate_window(const std::vector<CtxPtr>& primary_ctxs,
                                    Getter&& get_primary, int warmup_frames,
                                    int min_delta_frames, int min_window_ms,
                                    int warmup_timeout_ms, int window_timeout_ms,
                                    SnapshotFn&& on_window_start) {
  WindowTiming w;
  if (primary_ctxs.empty()) {
    w.warmed_up = true;
    w.complete = true;
    return w;
  }

  w.warmed_up = wait_for_all(primary_ctxs, get_primary, warmup_frames, warmup_timeout_ms);
  if (!w.warmed_up) return w;

  const size_t n = primary_ctxs.size();
  std::vector<int> start_cnt(n, 0);
  const uint64_t t0 = st_test_get_monotonic_time();
  for (size_t i = 0; i < n; i++) {
    if (primary_ctxs[i]) start_cnt[i] = get_primary(primary_ctxs[i]);
  }
  on_window_start(start_cnt);

  const uint64_t min_end_ns =
      t0 + static_cast<uint64_t>(min_window_ms > 0 ? min_window_ms : 0) * NS_PER_MS;
  w.complete = wait_until(
      [&]() {
        if (st_test_get_monotonic_time() < min_end_ns) return false;
        for (size_t i = 0; i < n; i++) {
          if (primary_ctxs[i] &&
              get_primary(primary_ctxs[i]) - start_cnt[i] < min_delta_frames) {
            return false;
          }
        }
        return true;
      },
      window_timeout_ms);

  w.dt_s = static_cast<double>(st_test_get_monotonic_time() - t0) / NS_PER_S;
  return w;
}

template <typename CtxPtr, typename Getter>
inline void fill_rates(std::vector<double>& out, const std::vector<CtxPtr>& ctxs,
                       const std::vector<int>& start_cnt, Getter&& get_cnt, double dt_s) {
  if (dt_s <= 0.0) return;
  for (size_t i = 0; i < ctxs.size(); i++) {
    if (ctxs[i]) out[i] = static_cast<double>(get_cnt(ctxs[i]) - start_cnt[i]) / dt_s;
  }
}

}  // namespace detail

/*
 * Pacing-aware steady-state rate measurement window.
 *
 * Waits for warmup_frames on all sessions, then measures over at least
 * min_delta_frames and min_window_ms. 30 frames over 1.0 s has <= 1/30 (3.3%)
 * edge quantization error, well inside the +/-10% integration FPS tolerance.
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
  res.rate.assign(ctxs.size(), 0.0);
  std::vector<int> start_cnt;
  const auto w = detail::run_rate_window(
      ctxs, get_cnt, warmup_frames, min_delta_frames, min_window_ms, warmup_timeout_ms,
      window_timeout_ms, [&](const std::vector<int>& primary0) { start_cnt = primary0; });
  res.warmed_up = w.warmed_up;
  res.complete = w.complete;
  detail::fill_rates(res.rate, ctxs, start_cnt, get_cnt, w.dt_s);
  return res;
}

/* Dual-counter variant measuring TX/RX frame rates and vsync rates in one window. */
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

  std::vector<int> tx0(n_tx, 0), tv0(n_tx, 0), rx0(n_rx, 0), rv0(n_rx, 0);
  const auto w = detail::run_rate_window(
      rx_ctxs, rx_frames, warmup_frames, min_delta_frames, min_window_ms,
      warmup_timeout_ms, window_timeout_ms, [&](const std::vector<int>& primary0) {
        rx0 = primary0;
        for (size_t i = 0; i < n_tx; i++) {
          if (!tx_ctxs[i]) continue;
          tx0[i] = tx_frames(tx_ctxs[i]);
          tv0[i] = vsync_count(tx_ctxs[i]);
        }
        for (size_t i = 0; i < n_rx; i++) {
          if (rx_ctxs[i]) rv0[i] = vsync_count(rx_ctxs[i]);
        }
      });
  res.warmed_up = w.warmed_up;
  res.complete = w.complete;
  detail::fill_rates(res.tx_fps, tx_ctxs, tx0, tx_frames, w.dt_s);
  detail::fill_rates(res.tx_vsync, tx_ctxs, tv0, vsync_count, w.dt_s);
  detail::fill_rates(res.rx_fps, rx_ctxs, rx0, rx_frames, w.dt_s);
  detail::fill_rates(res.rx_vsync, rx_ctxs, rv0, vsync_count, w.dt_s);
  return res;
}

}  // namespace mtl_test
