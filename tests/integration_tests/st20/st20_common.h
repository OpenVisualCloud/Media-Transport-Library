/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */
#pragma once

#include <openssl/sha.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../log.h"
#include "../test_harness.hpp"
#include "../tests.hpp"

#define ST20_TEST_PAYLOAD_TYPE (112)
#define DUMP_INCOMPLITE_SLICE (0)

/*
 * Steady-state FPS measurement window parameters:
 * - kSt20WarmupFrames=12 (~200-400 ms) lets RL pacing training, TSC fallback,
 *   and multi-session startup bursts settle completely before snapshotting t0.
 * - kSt20MinRateFrames=35 over kSt20MinRateWindowMs=1500 gives <=2.9% edge
 *   quantization error at 25 fps and <=1.1% at 59.94 fps (vs +/-10% tolerance).
 */
constexpr int kSt20WarmupFrames = 12;
constexpr int kSt20MinRateFrames = 35;
constexpr int kSt20MinRateWindowMs = 1500;
constexpr int kSt20WarmupTimeoutMs = 10000;
constexpr int kSt20WindowTimeoutMs = 8000;

/* Functional/digest wait targets when check_fps is false (~0.4-0.8 s). */
constexpr int kSt20MinRxFrames = 12;
constexpr int kSt20MinSliceRxFrames = 25;
constexpr int kSt20MinShaFrames = 4;
constexpr int kSt20EventTimeoutMs = 8000;

inline void st20_set_handle(tests_context* c, void* h) {
  __atomic_store_n(&c->handle, h, __ATOMIC_RELEASE);
}

inline void* st20_get_handle(const tests_context* c) {
  return __atomic_load_n(&c->handle, __ATOMIC_ACQUIRE);
}

uint16_t udp_port_for_idx(int idx, bool hdr_split = false, int base = 10000);
tests_context* init_test_ctx(struct st_tests_context* global_ctx, int idx,
                             uint16_t fb_cnt, bool check_sha = false);
bool st20_alloc_ext_frames(mtl_handle m_handle, tests_context* tctx, size_t frame_size);
bool st20_alloc_dma_ext_frames(mtl_handle m_handle, tests_context* tctx, size_t fb_size);

inline mtl_test::RateWindowResult st20_wait_rx_window(
    const std::vector<tests_context*>& rx_ctx, bool check_fps, bool check_sha,
    int min_rx_frames = kSt20MinRxFrames) {
  mtl_test::RateWindowResult res;
  if (check_fps) {
    res = mtl_test::measure_rate_window(rx_ctx, mtl_test::rx_frames, kSt20WarmupFrames,
                                        kSt20MinRateFrames, kSt20MinRateWindowMs,
                                        kSt20WarmupTimeoutMs, kSt20WindowTimeoutMs);
    if (check_sha) {
      res.complete =
          res.complete && mtl_test::wait_for_all(rx_ctx, mtl_test::sha_checked_frames,
                                                 kSt20MinShaFrames, kSt20EventTimeoutMs);
    }
  } else {
    res.rate.assign(rx_ctx.size(), 0.0);
    res.warmed_up = mtl_test::wait_for_all(rx_ctx, mtl_test::rx_frames, min_rx_frames,
                                           kSt20WarmupTimeoutMs);
    res.complete =
        res.warmed_up &&
        (!check_sha || mtl_test::wait_for_all(rx_ctx, mtl_test::sha_checked_frames,
                                              kSt20MinShaFrames, kSt20EventTimeoutMs));
  }
  return res;
}

void tx_feed_packet(void* args);
int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                              uint16_t* pkt_len);
int tx_next_video_frame(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta);
int tx_next_video_frame_timestamp(void* priv, uint16_t* next_frame_idx,
                                  struct st20_tx_frame_meta* meta);
int tx_next_ext_video_frame(void* priv, uint16_t* next_frame_idx,
                            struct st20_tx_frame_meta* meta);
int tx_next_ext_video_field(void* priv, uint16_t* next_frame_idx,
                            struct st20_tx_frame_meta* meta);
int tx_notify_ext_frame_done(void* priv, uint16_t frame_idx,
                             struct st20_tx_frame_meta* meta);
int tx_notify_timestamp_frame_done(void* priv, uint16_t frame_idx,
                                   struct st20_tx_frame_meta* meta);
int tx_notify_frame_done_check_tmstamp(void* priv, uint16_t frame_idx,
                                       struct st20_tx_frame_meta* meta);
int tx_next_video_field(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta);
int tx_frame_lines_ready(void* priv, uint16_t frame_idx, struct st20_tx_slice_meta* meta);
int tx_video_build_ooo_mapping(tests_context* s);

int rx_rtp_ready(void* args);
void rx_get_packet(void* args);
int st20_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta);

void st20_tx_ops_init(tests_context* st20, struct st20_tx_ops* ops);
void st20_rx_ops_init(tests_context* st20, struct st20_rx_ops* ops);
void st20_tx_assert_cnt(int expect_s20_tx_cnt);
void st20_rx_assert_cnt(int expect_s20_rx_cnt);
void init_single_port_tx(struct st20_tx_ops& ops, tests_context* tctx, const char* name,
                         uint16_t udp_port);
void init_single_port_rx(struct st20_rx_ops& ops, tests_context* tctx, const char* name,
                         uint16_t udp_port);
void st20_rx_drain_bufq_put_framebuff(tests_context* ctx);
void st20_rx_drain_bufq_free(tests_context* ctx);
/* rtp_pkt_size: BPM only, size the session and its largest pkt to it; 0 for the
 * default 1260-byte payload */
void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx,
                          uint16_t rtp_pkt_size = 0);

class St20DeinitGuard {
 public:
  using CtxCleanupFn = std::function<void(tests_context*)>;

  St20DeinitGuard(mtl_handle handle, std::vector<tests_context*>& tx_ctx,
                  std::vector<tests_context*>& rx_ctx,
                  std::vector<st20_tx_handle>& tx_handle,
                  std::vector<st20_rx_handle>& rx_handle,
                  std::vector<std::thread>* tx_threads = nullptr,
                  std::vector<std::thread>* rx_threads = nullptr);

  ~St20DeinitGuard();

  St20DeinitGuard(const St20DeinitGuard&) = delete;
  St20DeinitGuard& operator=(const St20DeinitGuard&) = delete;

  void set_started(bool started);
  void set_ext_buf(bool ext_buf);
  void add_thread_group(std::vector<std::thread>& threads);
  void set_tx_ctx_cleanup(CtxCleanupFn fn);
  void set_rx_ctx_cleanup(CtxCleanupFn fn);

  /* Stop threads and stop mtl, but do not free handles/contexts.
   * Safe to call multiple times.
   */
  void stop();
  /* Stop background work and free session handles, but retain test contexts. */
  void release_sessions();

 private:
  void cleanup();

  mtl_handle m_handle_;
  bool started_;
  bool stopped_;
  bool sessions_released_;
  bool cleaned_;
  bool ext_buf_;

  std::vector<tests_context*>& tx_ctx_;
  std::vector<tests_context*>& rx_ctx_;
  std::vector<st20_tx_handle>& tx_handle_;
  std::vector<st20_rx_handle>& rx_handle_;
  std::vector<std::thread>* tx_threads_;
  std::vector<std::thread>* rx_threads_;
  std::vector<std::vector<std::thread>*> extra_thread_groups_;

  CtxCleanupFn tx_ctx_cleanup_;
  CtxCleanupFn rx_ctx_cleanup_;
};

int st20_rx_uframe_pg_callback(void* priv, void* frame,
                               struct st20_rx_uframe_pg_meta* meta);

int st20_digest_rx_frame_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta);
int st20_digest_rx_slice_ready(void* priv, void* frame, struct st20_rx_slice_meta* meta);
int st20_digest_rx_field_ready(void* priv, void* frame, struct st20_rx_frame_meta* meta);
void st20_digest_rx_frame_check(void* args);
void st20_digest_rx_field_check(void* args);
