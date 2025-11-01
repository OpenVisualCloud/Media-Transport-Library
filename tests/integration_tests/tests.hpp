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

#include "test_util.h"

#define TEST_LCORE_LIST_MAX_LEN (128)
#define TEST_SHA_HIST_NUM (2)
#define ST22_TEST_SHA_HIST_NUM (3)
#define TEST_MAX_SHA_HIST_NUM (3)

#define MAX_TEST_ENCODER_SESSIONS (8)
#define MAX_TEST_DECODER_SESSIONS (8)
#define MAX_TEST_CONVERTER_SESSIONS (8)

#ifdef MTL_GTEST_AFTER_1_9_0
#define PARAMETERIZED_TEST INSTANTIATE_TEST_SUITE_P
#else
#define PARAMETERIZED_TEST INSTANTIATE_TEST_CASE_P
#endif

/* forward declare */
struct st_tests_context;

struct test_converter_session {
  int idx;

  struct st20_converter_create_req req;
  st20p_convert_session session_p;
  bool stop;
  pthread_t convert_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int sleep_time_us;

  int frame_cnt;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
};

struct test_st22_encoder_session {
  int idx;

  struct st22_encoder_create_req req;
  st22p_encode_session session_p;
  bool stop;
  pthread_t encode_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int sleep_time_us;

  int frame_cnt;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;
  int rand_ratio;

  struct st_tests_context* ctx;
};

struct test_st22_decoder_session {
  int idx;

  struct st22_decoder_create_req req;
  st22p_decode_session session_p;
  bool stop;
  pthread_t decode_thread;
  pthread_cond_t wake_cond;
  pthread_mutex_t wake_mutex;

  int sleep_time_us;

  int frame_cnt;
  int fail_interval;
  int timeout_interval;
  int timeout_ms;

  struct st_tests_context* ctx;
};

struct st_tests_context {
  struct mtl_init_params para;
  mtl_handle handle;
  char lcores_list[TEST_LCORE_LIST_MAX_LEN];
  uint8_t mcast_ip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  uint64_t ptp_time;
  enum st_test_level level;
  bool hdr_split;
  bool dhcp;
  bool mcast_only;
  enum mtl_iova_mode iova;
  enum mtl_rss_mode rss_mode;
  bool same_dual_port;

  enum st30_tx_pacing_way tx_audio_pacing_way;

  st22_encoder_dev_handle encoder_dev_handle;
  st22_decoder_dev_handle decoder_dev_handle;
  st20_converter_dev_handle converter_dev_handle;
  struct test_st22_encoder_session* encoder_sessions[MAX_TEST_ENCODER_SESSIONS];
  struct test_st22_decoder_session* decoder_sessions[MAX_TEST_DECODER_SESSIONS];
  struct test_converter_session* converter_sessions[MAX_TEST_CONVERTER_SESSIONS];
  bool encoder_use_block_get;
  bool decoder_use_block_get;
  int plugin_fail_interval;
  int plugin_timeout_interval;
  int plugin_timeout_ms;
  int plugin_rand_ratio;
  bool noctx_tests;
};

struct st_tests_context* st_test_ctx(void);

static inline int st_test_num_port(struct st_tests_context* ctx) {
  return ctx->para.num_ports;
}

static inline void st_test_jxs_fail_interval(struct st_tests_context* ctx, int interval) {
  ctx->plugin_fail_interval = interval;
}

static inline void st_test_jxs_timeout_interval(struct st_tests_context* ctx,
                                                int interval) {
  ctx->plugin_timeout_interval = interval;
}

static inline void st_test_jxs_timeout_ms(struct st_tests_context* ctx, int ms) {
  ctx->plugin_timeout_ms = ms;
}

static inline void st_test_jxs_rand_ratio(struct st_tests_context* ctx, int rand_ratio) {
  ctx->plugin_rand_ratio = rand_ratio;
}

static inline void st_test_jxs_use_block_get(struct st_tests_context* ctx, bool block) {
  ctx->decoder_use_block_get = block;
  ctx->encoder_use_block_get = block;
}

int st_test_sch_cnt(struct st_tests_context* ctx);

bool st_test_dma_available(struct st_tests_context* ctx);

int st_test_st22_plugin_register(struct st_tests_context* ctx);

int st_test_st22_plugin_unregister(struct st_tests_context* ctx);

int st_test_convert_plugin_register(struct st_tests_context* ctx);

int st_test_convert_plugin_unregister(struct st_tests_context* ctx);

void sha_frame_check(void* args);

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
  uint64_t first_vsync_time = 0;
  int packet_rec = 0;
  uint64_t start_time = 0;
  void* handle = NULL;
  void* priv = NULL; /* private data for the test */
  bool stop = false;
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
  size_t fb_size;
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
  bool out_of_order_pkt = false; /* out of order pkt index */
  int* ooo_mapping = NULL;
  int slice_cnt = 0;
  uint32_t slice_recv_lines = 0;
  uint64_t slice_recv_timestamp = 0;
  void* ext_fb_malloc;
  uint8_t* ext_fb = NULL;
  mtl_iova_t ext_fb_iova = 0;
  size_t ext_fb_iova_map_sz = 0;
  struct st20_ext_frame* ext_frames;
  struct st_ext_frame* p_ext_frames;
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

#define create_free_max(A, max)                             \
  do {                                                      \
    auto ctx = st_test_ctx();                               \
    auto m_handle = ctx->handle;                            \
    auto act = 0;                                           \
    int ret, expect_cnt = 0;                                \
    struct A##_ops ops;                                     \
    auto test_ctx = new tests_context();                    \
    ASSERT_TRUE(test_ctx != NULL);                          \
    auto sch_cnt = st_test_sch_cnt(ctx);                    \
    test_ctx->idx = 0;                                      \
    test_ctx->ctx = ctx;                                    \
    test_ctx->fb_cnt = 2;                                   \
    test_ctx->fb_idx = 0;                                   \
    A##_ops_init(test_ctx, &ops);                           \
                                                            \
    A##_handle handle[max];                                 \
    for (int i = 0; i < max; i++) {                         \
      handle[i] = A##_create(m_handle, &ops);               \
      if (!handle[i]) break;                                \
      ops.udp_port[MTL_SESSION_PORT_P]++;                   \
      ops.udp_port[MTL_SESSION_PORT_R]++;                   \
      expect_cnt++;                                         \
      A##_assert_cnt(expect_cnt);                           \
      st_usleep(100 * 1000);                                \
    }                                                       \
    info("%s, max session cnt %d\n", __func__, expect_cnt); \
    act = expect_cnt;                                       \
    for (int i = 0; i < act; i++) {                         \
      ret = A##_free(handle[i]);                            \
      EXPECT_GE(ret, 0);                                    \
      expect_cnt--;                                         \
      A##_assert_cnt(expect_cnt);                           \
      st_usleep(100 * 1000);                                \
    }                                                       \
                                                            \
    A##_assert_cnt(0);                                      \
    EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));               \
                                                            \
    delete test_ctx;                                        \
  } while (0)

#define create_free_test(A, base, step, repeat)    \
  do {                                             \
    auto ctx = st_test_ctx();                      \
    auto m_handle = ctx->handle;                   \
    int ret, expect_cnt = 0;                       \
    struct A##_ops ops;                            \
    auto test_ctx = new tests_context();           \
    ASSERT_TRUE(test_ctx != NULL);                 \
                                                   \
    test_ctx->idx = 0;                             \
    test_ctx->ctx = ctx;                           \
    test_ctx->fb_cnt = 2;                          \
    test_ctx->fb_idx = 0;                          \
    A##_ops_init(test_ctx, &ops);                  \
                                                   \
    A##_handle handle_base[base];                  \
    for (int i = 0; i < base; i++) {               \
      handle_base[i] = A##_create(m_handle, &ops); \
      ASSERT_TRUE(handle_base[i]);                 \
      ops.udp_port[MTL_SESSION_PORT_P]++;          \
      ops.udp_port[MTL_SESSION_PORT_R]++;          \
      expect_cnt++;                                \
      A##_assert_cnt(expect_cnt);                  \
    }                                              \
                                                   \
    for (int i = 0; i < repeat; i++) {             \
      A##_handle handle[step];                     \
                                                   \
      for (int j = 0; j < step; j++) {             \
        handle[j] = A##_create(m_handle, &ops);    \
        ASSERT_TRUE(handle[j] != NULL);            \
        ops.udp_port[MTL_SESSION_PORT_P]++;        \
        ops.udp_port[MTL_SESSION_PORT_R]++;        \
        expect_cnt++;                              \
        A##_assert_cnt(expect_cnt);                \
      }                                            \
                                                   \
      st_usleep(100 * 1000);                       \
      for (int j = 0; j < step; j++) {             \
        ret = A##_free(handle[j]);                 \
        ASSERT_TRUE(ret >= 0);                     \
        expect_cnt--;                              \
        A##_assert_cnt(expect_cnt);                \
      }                                            \
      st_usleep(100 * 1000);                       \
    }                                              \
                                                   \
    for (int i = 0; i < base; i++) {               \
      ret = A##_free(handle_base[i]);              \
      EXPECT_GE(ret, 0);                           \
      expect_cnt--;                                \
      A##_assert_cnt(expect_cnt);                  \
    }                                              \
                                                   \
    A##_assert_cnt(0);                             \
                                                   \
    delete test_ctx;                               \
  } while (0)

#define expect_fail_test(A)                \
  do {                                     \
    auto ctx = st_test_ctx();              \
    auto m_handle = ctx->handle;           \
    struct A##_ops ops;                    \
    auto test_ctx = new tests_context();   \
    EXPECT_TRUE(test_ctx != NULL);         \
    A##_handle handle;                     \
                                           \
    test_ctx->idx = 0;                     \
    test_ctx->ctx = ctx;                   \
    test_ctx->fb_cnt = 2;                  \
    test_ctx->fb_idx = 0;                  \
    A##_ops_init(test_ctx, &ops);          \
    /* test with 0 num_port */             \
    ops.num_port = 0;                      \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with crazy big num_port */     \
    ops.num_port = 100;                    \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with negative num_port */      \
    ops.num_port = -1;                     \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with 2 num_port */             \
    if (ctx->para.num_ports < 2) {         \
      ops.num_port = 2;                    \
      handle = A##_create(m_handle, &ops); \
      EXPECT_TRUE(handle == NULL);         \
    }                                      \
    delete test_ctx;                       \
  } while (0)

#define expect_fail_test_fb_cnt(A, fb_nb) \
  do {                                    \
    auto ctx = st_test_ctx();             \
    auto m_handle = ctx->handle;          \
    struct A##_ops ops;                   \
    auto test_ctx = new tests_context();  \
    ASSERT_TRUE(test_ctx != NULL);        \
    A##_handle handle;                    \
                                          \
    test_ctx->idx = 0;                    \
    test_ctx->ctx = ctx;                  \
    test_ctx->fb_cnt = fb_nb;             \
    test_ctx->fb_idx = 0;                 \
    A##_ops_init(test_ctx, &ops);         \
    /* test with 1 port */                \
    ops.num_port = 1;                     \
    handle = A##_create(m_handle, &ops);  \
    EXPECT_TRUE(handle == NULL);          \
    delete test_ctx;                      \
  } while (0)

#define test_get_framebuffer(A, fb_nb)            \
  do {                                            \
    auto ctx = st_test_ctx();                     \
    auto m_handle = ctx->handle;                  \
    struct A##_ops ops;                           \
    auto test_ctx = new tests_context();          \
    ASSERT_TRUE(test_ctx != NULL);                \
    A##_handle handle;                            \
                                                  \
    test_ctx->idx = 0;                            \
    test_ctx->ctx = ctx;                          \
    test_ctx->fb_cnt = fb_nb;                     \
    test_ctx->fb_idx = 0;                         \
    A##_ops_init(test_ctx, &ops);                 \
    /* test with 1 port */                        \
    ops.num_port = 1;                             \
    handle = A##_create(m_handle, &ops);          \
    ASSERT_TRUE(handle != NULL);                  \
    void* fb_buff;                                \
    for (uint16_t idx = 0; idx < fb_nb; idx++) {  \
      fb_buff = A##_get_framebuffer(handle, idx); \
      EXPECT_TRUE(fb_buff != NULL);               \
    }                                             \
    auto ret = A##_free(handle);                  \
    EXPECT_GE(ret, 0);                            \
    delete test_ctx;                              \
  } while (0)

#define expect_fail_test_get_framebuffer(A, fb_nb)    \
  do {                                                \
    auto ctx = st_test_ctx();                         \
    auto m_handle = ctx->handle;                      \
    struct A##_ops ops;                               \
    auto test_ctx = new tests_context();              \
    ASSERT_TRUE(test_ctx != NULL);                    \
    A##_handle handle;                                \
                                                      \
    test_ctx->idx = 0;                                \
    test_ctx->ctx = ctx;                              \
    test_ctx->fb_cnt = fb_nb;                         \
    test_ctx->fb_idx = 0;                             \
    A##_ops_init(test_ctx, &ops);                     \
    /* test with 1 port */                            \
    ops.num_port = 1;                                 \
    handle = A##_create(m_handle, &ops);              \
    ASSERT_TRUE(handle != NULL);                      \
    void* fb_buff;                                    \
    fb_buff = A##_get_framebuffer(handle, fb_nb);     \
    EXPECT_TRUE(fb_buff == NULL);                     \
    fb_buff = A##_get_framebuffer(handle, fb_nb * 2); \
    EXPECT_TRUE(fb_buff == NULL);                     \
    auto ret = A##_free(handle);                      \
    EXPECT_GE(ret, 0);                                \
    delete test_ctx;                                  \
  } while (0)

#define expect_fail_test_rtp_ring(A, s_type, ring_sz) \
  do {                                                \
    auto ctx = st_test_ctx();                         \
    auto m_handle = ctx->handle;                      \
    struct A##_ops ops;                               \
    auto test_ctx = new tests_context();              \
    ASSERT_TRUE(test_ctx != NULL);                    \
    A##_handle handle;                                \
    test_ctx->idx = 0;                                \
    test_ctx->ctx = ctx;                              \
    test_ctx->fb_idx = 0;                             \
    A##_ops_init(test_ctx, &ops);                     \
    /* test with 1 port */                            \
    ops.num_port = 1;                                 \
    ops.type = s_type;                                \
    ops.rtp_ring_size = ring_sz;                      \
    handle = A##_create(m_handle, &ops);              \
    EXPECT_TRUE(handle == NULL);                      \
    delete test_ctx;                                  \
  } while (0)

#define expect_fail_test_rtp_ring_2(A, ring_sz) \
  do {                                          \
    auto ctx = st_test_ctx();                   \
    auto m_handle = ctx->handle;                \
    struct A##_ops ops;                         \
    auto test_ctx = new tests_context();        \
    ASSERT_TRUE(test_ctx != NULL);              \
    A##_handle handle;                          \
    test_ctx->idx = 0;                          \
    test_ctx->ctx = ctx;                        \
    test_ctx->fb_idx = 0;                       \
    A##_ops_init(test_ctx, &ops);               \
    /* test with 1 port */                      \
    ops.num_port = 1;                           \
    ops.rtp_ring_size = ring_sz;                \
    handle = A##_create(m_handle, &ops);        \
    EXPECT_TRUE(handle == NULL);                \
    delete test_ctx;                            \
  } while (0)

#define expect_test_rtp_pkt_size(A, s_type, pkt_sz, expect) \
  do {                                                      \
    auto ctx = st_test_ctx();                               \
    auto m_handle = ctx->handle;                            \
    struct A##_ops ops;                                     \
    auto test_ctx = new tests_context();                    \
    ASSERT_TRUE(test_ctx != NULL);                          \
    A##_handle handle;                                      \
    test_ctx->idx = 0;                                      \
    test_ctx->ctx = ctx;                                    \
    test_ctx->fb_idx = 0;                                   \
    A##_ops_init(test_ctx, &ops);                           \
    /* test with 1 port */                                  \
    ops.num_port = 1;                                       \
    ops.type = s_type;                                      \
    ops.rtp_ring_size = 1024;                               \
    ops.rtp_frame_total_pkts = 1024;                        \
    ops.rtp_pkt_size = pkt_sz;                              \
    handle = A##_create(m_handle, &ops);                    \
    if (expect)                                             \
      EXPECT_TRUE(handle != NULL);                          \
    else                                                    \
      EXPECT_TRUE(handle == NULL);                          \
    if (handle) {                                           \
      auto ret = A##_free(handle);                          \
      EXPECT_GE(ret, 0);                                    \
    }                                                       \
    delete test_ctx;                                        \
  } while (0)

#define expect_test_rtp_pkt_size_2(A, s_type, pkt_sz, expect) \
  do {                                                        \
    auto ctx = st_test_ctx();                                 \
    auto m_handle = ctx->handle;                              \
    struct A##_ops ops;                                       \
    auto test_ctx = new tests_context();                      \
    ASSERT_TRUE(test_ctx != NULL);                            \
    A##_handle handle;                                        \
    test_ctx->idx = 0;                                        \
    test_ctx->ctx = ctx;                                      \
    test_ctx->fb_idx = 0;                                     \
    A##_ops_init(test_ctx, &ops);                             \
    /* test with 1 port */                                    \
    ops.num_port = 1;                                         \
    ops.rtp_ring_size = 1024;                                 \
    ops.rtp_pkt_size = pkt_sz;                                \
    ops.type = s_type;                                        \
    handle = A##_create(m_handle, &ops);                      \
    if (expect)                                               \
      EXPECT_TRUE(handle != NULL);                            \
    else                                                      \
      EXPECT_TRUE(handle == NULL);                            \
    if (handle) {                                             \
      auto ret = A##_free(handle);                            \
      EXPECT_GE(ret, 0);                                      \
    }                                                         \
    delete test_ctx;                                          \
  } while (0)

/* pipeline test marco */
#define pipeline_create_free_test(A, base, step, repeat) \
  do {                                                   \
    auto ctx = st_test_ctx();                            \
    auto m_handle = ctx->handle;                         \
    int ret, expect_cnt = 0;                             \
    struct A##_ops ops;                                  \
    auto test_ctx = new tests_context();                 \
    ASSERT_TRUE(test_ctx != NULL);                       \
                                                         \
    test_ctx->idx = 0;                                   \
    test_ctx->ctx = ctx;                                 \
    test_ctx->fb_cnt = 2;                                \
    test_ctx->fb_idx = 0;                                \
    A##_ops_init(test_ctx, &ops);                        \
                                                         \
    A##_handle handle_base[base];                        \
    for (int i = 0; i < base; i++) {                     \
      handle_base[i] = A##_create(m_handle, &ops);       \
      ASSERT_TRUE(handle_base[i]);                       \
      ops.port.udp_port[MTL_SESSION_PORT_P]++;           \
      ops.port.udp_port[MTL_SESSION_PORT_R]++;           \
      expect_cnt++;                                      \
      A##_assert_cnt(expect_cnt);                        \
    }                                                    \
                                                         \
    for (int i = 0; i < repeat; i++) {                   \
      A##_handle handle[step];                           \
                                                         \
      for (int j = 0; j < step; j++) {                   \
        handle[j] = A##_create(m_handle, &ops);          \
        ASSERT_TRUE(handle[j] != NULL);                  \
        ops.port.udp_port[MTL_SESSION_PORT_P]++;         \
        ops.port.udp_port[MTL_SESSION_PORT_R]++;         \
        expect_cnt++;                                    \
        A##_assert_cnt(expect_cnt);                      \
      }                                                  \
                                                         \
      for (int j = 0; j < step; j++) {                   \
        ret = A##_free(handle[j]);                       \
        ASSERT_TRUE(ret >= 0);                           \
        expect_cnt--;                                    \
        A##_assert_cnt(expect_cnt);                      \
      }                                                  \
    }                                                    \
                                                         \
    for (int i = 0; i < base; i++) {                     \
      ret = A##_free(handle_base[i]);                    \
      EXPECT_GE(ret, 0);                                 \
      expect_cnt--;                                      \
      A##_assert_cnt(expect_cnt);                        \
    }                                                    \
                                                         \
    A##_assert_cnt(0);                                   \
                                                         \
    delete test_ctx;                                     \
  } while (0)

#define pipeline_create_free_max(A, max)                    \
  do {                                                      \
    auto ctx = st_test_ctx();                               \
    auto m_handle = ctx->handle;                            \
    auto act = 0;                                           \
    int ret, expect_cnt = 0;                                \
    struct A##_ops ops;                                     \
    auto test_ctx = new tests_context();                    \
    ASSERT_TRUE(test_ctx != NULL);                          \
    auto sch_cnt = st_test_sch_cnt(ctx);                    \
    test_ctx->idx = 0;                                      \
    test_ctx->ctx = ctx;                                    \
    test_ctx->fb_cnt = 2;                                   \
    test_ctx->fb_idx = 0;                                   \
    A##_ops_init(test_ctx, &ops);                           \
                                                            \
    A##_handle handle[max];                                 \
    for (int i = 0; i < max; i++) {                         \
      handle[i] = A##_create(m_handle, &ops);               \
      if (!handle[i]) break;                                \
      ops.port.udp_port[MTL_SESSION_PORT_P]++;              \
      ops.port.udp_port[MTL_SESSION_PORT_R]++;              \
      expect_cnt++;                                         \
      A##_assert_cnt(expect_cnt);                           \
    }                                                       \
    info("%s, max session cnt %d\n", __func__, expect_cnt); \
    act = expect_cnt;                                       \
    for (int i = 0; i < act; i++) {                         \
      ret = A##_free(handle[i]);                            \
      EXPECT_GE(ret, 0);                                    \
      expect_cnt--;                                         \
      A##_assert_cnt(expect_cnt);                           \
    }                                                       \
                                                            \
    A##_assert_cnt(0);                                      \
    EXPECT_EQ(sch_cnt, st_test_sch_cnt(ctx));               \
                                                            \
    delete test_ctx;                                        \
  } while (0)

#define pipeline_expect_fail_test(A)       \
  do {                                     \
    auto ctx = st_test_ctx();              \
    auto m_handle = ctx->handle;           \
    struct A##_ops ops;                    \
    auto test_ctx = new tests_context();   \
    EXPECT_TRUE(test_ctx != NULL);         \
    A##_handle handle;                     \
                                           \
    test_ctx->idx = 0;                     \
    test_ctx->ctx = ctx;                   \
    test_ctx->fb_cnt = 2;                  \
    test_ctx->fb_idx = 0;                  \
    A##_ops_init(test_ctx, &ops);          \
    /* test with 0 num_port */             \
    ops.port.num_port = 0;                 \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with crazy big num_port */     \
    ops.port.num_port = 100;               \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with negative num_port */      \
    ops.port.num_port = -1;                \
    handle = A##_create(m_handle, &ops);   \
    EXPECT_TRUE(handle == NULL);           \
                                           \
    /* test with 2 num_port */             \
    if (ctx->para.num_ports < 2) {         \
      ops.port.num_port = 2;               \
      handle = A##_create(m_handle, &ops); \
      EXPECT_TRUE(handle == NULL);         \
    }                                      \
    delete test_ctx;                       \
  } while (0)

#define pipeline_expect_fail_test_fb_cnt(A, fb_nb) \
  do {                                             \
    auto ctx = st_test_ctx();                      \
    auto m_handle = ctx->handle;                   \
    struct A##_ops ops;                            \
    auto test_ctx = new tests_context();           \
    ASSERT_TRUE(test_ctx != NULL);                 \
    A##_handle handle;                             \
                                                   \
    test_ctx->idx = 0;                             \
    test_ctx->ctx = ctx;                           \
    test_ctx->fb_cnt = fb_nb;                      \
    test_ctx->fb_idx = 0;                          \
    A##_ops_init(test_ctx, &ops);                  \
    /* test with 1 port */                         \
    ops.port.num_port = 1;                         \
    handle = A##_create(m_handle, &ops);           \
    EXPECT_TRUE(handle == NULL);                   \
    delete test_ctx;                               \
  } while (0)
