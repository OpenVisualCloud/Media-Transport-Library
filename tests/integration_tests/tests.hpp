/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#pragma once

#include <gtest/gtest.h>
#include <inttypes.h>
#include <math.h>
#include <mtl/st30_api.h>
#include <mtl/st30_pipeline_api.h>
#include <mtl/st40_api.h>
#include <mtl/st_convert_api.h>
#include <mtl/st_pipeline_api.h>

#include <atomic>

#include "test_util.h"

#define TEST_SHA_HIST_NUM (2)
#define ST22_TEST_SHA_HIST_NUM (3)
#define TEST_MAX_SHA_HIST_NUM (3)

struct st_tests_context {
  struct mtl_init_params para;
  uint8_t init_sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  mtl_handle handle;
  uint8_t mcast_ip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  uint64_t ptp_time;
  bool user_p_sip;
  bool dhcp;
  enum mtl_iova_mode iova;
  enum mtl_rss_mode rss_mode;
  bool same_dual_port;

  st22_encoder_dev_handle encoder_dev_handle;
  st22_decoder_dev_handle decoder_dev_handle;
  st20_converter_dev_handle converter_dev_handle;
  bool encoder_use_block_get;
  bool decoder_use_block_get;
  int plugin_fail_interval;
  int plugin_timeout_interval;
  int plugin_timeout_ms;
  int plugin_rand_ratio;
  bool user_log_level;
  bool single_process;
};

struct st_tests_context* st_test_ctx(void);

int st_test_sch_cnt(struct st_tests_context* ctx);

bool st_test_dma_available(struct st_tests_context* ctx);

int st_test_st22_plugin_register(struct st_tests_context* ctx);

int st_test_st22_plugin_unregister(struct st_tests_context* ctx);

int st_test_convert_plugin_register(struct st_tests_context* ctx);

int st_test_convert_plugin_unregister(struct st_tests_context* ctx);

void sha_frame_check(void* args);

struct fmd_annex_a;

class tests_context {
 public:
  struct st_tests_context* ctx = NULL;
  int idx = 0;
  int fb_cnt = 0;
  uint16_t fb_idx = 0;
  int fb_send = 0;
  int fb_send_done = 0;
  int fb_rec = 0;
  int vsync_cnt = 0;
  int packet_rec = 0;
  uint64_t start_time = 0;
  void* handle = NULL;
  std::atomic<bool> ready{true};
  void* priv = NULL; /* private data for the test */
  std::atomic<bool> stop{false};
  std::mutex mtx = {};
  std::condition_variable cv = {};
  struct st20_pgroup st20_pg = {};
  int total_pkts_in_frame = 0;
  int seq_id = 0;
  int frame_base_seq_id = 0; /* for ooo_mapping */
  int pkt_idx = 0;
  uint32_t rtp_tmstamp = 0;
  int rtp_delta = 0;
  int pkt_data_len = 0;
  int pkts_in_line = 0;
  int bytes_in_line = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  enum st_frame_fmt fmt = ST_FRAME_FMT_MAX;
  int stride = 0;
  bool single_line = false;
  bool slice = false;
  std::queue<void*> buf_q = {};
  std::queue<bool> second_field_q = {};
  int lines_per_slice = 0;

  /* audio */
  enum st30_fmt audio_fmt = ST30_FMT_MAX;
  uint16_t audio_channel = 0;
  enum st30_sampling audio_sampling = ST30_SAMPLING_MAX;
  enum st30_ptime audio_ptime = ST30_PTIME_MAX;

  size_t frame_size = 0;
  size_t fb_size = 0;
  size_t uframe_size = 0;
  uint8_t shas[TEST_MAX_SHA_HIST_NUM][SHA256_DIGEST_LENGTH] = {};
  /* frame buff alloc in the test context */
  uint8_t* frame_buf[TEST_MAX_SHA_HIST_NUM] = {};
  uint16_t lines_ready[TEST_MAX_SHA_HIST_NUM] = {};
  bool check_sha = false;
  int sha_fail_cnt = 0; /* fail as sha check fail */
  int tx_tmstamp_delta_fail_cnt = 0;
  int rx_meta_fail_cnt = 0;
  int rx_field_fail_cnt = 0;
  int incomplete_frame_cnt = 0;
  int meta_timing_fail_cnt = 0;
  int incomplete_slice_cnt = 0;
  int check_sha_frame_cnt = 0;
  int last_user_meta_frame_idx = 0;
  int user_meta_fail_cnt = 0;
  int tp_fail_cnt = 0;
  bool tp_other_fail_logged = false;
  int field_phase_fail_cnt = 0;
  int field_phase_unverified_cnt = 0;
  /* ns per field period, 0 disables the interlaced field-phase check */
  double field_period_ns = 0;
  bool out_of_order_pkt = false; /* out of order pkt index */
  int* ooo_mapping = NULL;
  int slice_cnt = 0;
  uint32_t slice_recv_lines = 0;
  uint64_t slice_recv_timestamp = 0;
  void* ext_fb_malloc = nullptr;
  uint8_t* ext_fb = NULL;
  mtl_iova_t ext_fb_iova = 0;
  size_t ext_fb_iova_map_sz = 0;
  struct st20_ext_frame* ext_frames = nullptr;
  int ext_idx = 0;
  bool ext_fb_in_use[3] = {false}; /* assume 3 framebuffer */
  mtl_dma_mem_handle dma_mem = NULL;

  bool user_pacing = false;
  /* user timestamp which advanced by 1 for every frame */
  bool user_timestamp = false;
  uint32_t pre_timestamp = 0;
  double frame_time = 0; /* in ns */
  uint64_t ptp_time_first_frame = 0;
  bool user_meta = false;
  bool block_get = false;
  bool rx_timing_parser = false;
  bool st40_empty_frame = false;
  bool st40_fmd = false;
  /* ST 2110-41 Annex A plan, owned by the test */
  struct fmd_annex_a* st40_annex_a = nullptr;
  uint64_t last_rx_time = 0; /* monotonic ns, 0 before the first packet */
  uint64_t max_rx_gap = 0;   /* ns */
  uint64_t rx_seq_lost = 0;
};

#define TEST_USER_META_MAGIC ST_PLUGIN_MAGIC('U', 'S', 'M', 'T')

struct test_user_meta {
  uint32_t magic;
  int session_idx;
  int frame_idx;
};

int tests_context_unit(tests_context* ctx);

int test_ctx_notify_event(void* priv, enum st_event event, void* args);

int tx_next_frame(void* priv, uint16_t* next_frame_idx);

#define TEST_CREATE_FREE_MAX (16)
