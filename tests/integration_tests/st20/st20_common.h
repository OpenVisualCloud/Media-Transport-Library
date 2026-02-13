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
#include "../tests.hpp"

#define ST20_TRAIN_TIME_S (1)
#define ST20_TEST_PAYLOAD_TYPE (112)
#define DUMP_INCOMPLITE_SLICE (0)

struct St20SessionConfig {
  enum st20_type type;
  enum st20_packing packing;
  enum st_fps fps;
  int width;
  int height;
  bool interlaced;
  enum st20_fmt fmt;
};

uint16_t udp_port_for_idx(int idx, bool hdr_split = false, int base = 10000);
std::vector<St20SessionConfig> build_sessions(int sessions, enum st20_type type[],
                                              enum st20_packing packing[],
                                              enum st_fps fps[], int width[],
                                              int height[], bool interlaced[],
                                              enum st20_fmt fmt[]);
tests_context* init_test_ctx(struct st_tests_context* global_ctx, int idx,
                             uint16_t fb_cnt, bool check_sha = false);
void tx_feed_packet(void* args);
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
void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx);

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

 private:
  void cleanup();

  mtl_handle m_handle_;
  bool started_;
  bool stopped_;
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
