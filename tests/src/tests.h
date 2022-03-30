/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#pragma once

#include <gtest/gtest.h>
#include <math.h>
#include <openssl/md5.h>
#include <st_convert_api.h>
#include <st_dpdk_api.h>

#include <condition_variable>
#include <mutex>
#include <queue>

#include "test_platform.h"

#define TEST_LCORE_LIST_MAX_LEN (128)
#define TEST_MD5_HIST_NUM (2)

#define TEST_DATA_FIXED_PATTER (0)

#ifndef NS_PER_S
#define NS_PER_S (1000000000)
#endif

enum st_test_level {
  ST_TEST_LEVEL_ALL = 0,
  ST_TEST_LEVEL_MANDATORY,
  ST_TEST_LEVEL_MAX, /* max value of this enum */
};

struct st_tests_context {
  struct st_init_params para;
  st_handle handle;
  char lcores_list[TEST_LCORE_LIST_MAX_LEN];
  uint8_t mcast_ip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  uint64_t ptp_time;
  enum st_test_level level;
};

struct st_tests_context* st_test_ctx(void);

static inline int st_test_num_port(struct st_tests_context* ctx) {
  return ctx->para.num_ports;
}

static inline void* st_test_zmalloc(size_t sz) {
  void* p = malloc(sz);
  if (p) memset(p, 0x0, sz);
  return p;
}

static inline void st_test_free(void* p) { free(p); }

static inline void st_test_rand_data(uint8_t* p, size_t sz, uint8_t base) {
  for (size_t i = 0; i < sz; i++) {
#if TEST_DATA_FIXED_PATTER
    p[i] = base + i;
#else
    p[i] = rand();
#endif
  }
}

int st_test_check_patter(uint8_t* p, size_t sz, uint8_t base);

int st_test_sch_cnt(struct st_tests_context* ctx);

bool st_test_dma_available(struct st_tests_context* ctx);

/* Monotonic time (in nanoseconds) since some unspecified starting point. */
static inline uint64_t st_test_get_monotonic_time() {
  struct timespec ts;

  clock_gettime(ST_CLOCK_MONOTONIC_ID, &ts);
  return ((uint64_t)ts.tv_sec * NS_PER_S) + ts.tv_nsec;
}
void test_md5_dump(const char* tag, unsigned char* md5);
void md5_frame_check(void* args);
class tests_context {
 public:
  struct st_tests_context* ctx;
  int idx;
  int fb_cnt;
  uint16_t fb_idx;
  int fb_send;
  int fb_rec;
  int packet_rec;
  uint64_t start_time;
  void* handle;
  void* priv; /* private data for the test */
  bool stop;
  std::mutex mtx;
  std::condition_variable cv;
  struct st20_pgroup st20_pg;
  int total_pkts_in_frame;
  int seq_id;
  int frame_base_seq_id; /* for ooo_mapping */
  int pkt_idx;
  int rtp_tmstamp;
  int rtp_delta;
  int pkt_data_len;
  int pkts_in_line;
  int bytes_in_line;
  int width;
  int height;
  int stride;
  bool single_line;
  bool slice;
  std::queue<void*> buf_q;
  std::queue<enum st_field> flag_q;
  int lines_per_slice;

  size_t frame_size;
  size_t uframe_size;
  uint8_t md5s[TEST_MD5_HIST_NUM][MD5_DIGEST_LENGTH];
  uint8_t* frame_buf[TEST_MD5_HIST_NUM];
  uint16_t lines_ready[TEST_MD5_HIST_NUM];
  bool check_md5 = false;
  int fail_cnt; /* fail as md5 or wrong status, etc. */
  int incomplete_frame_cnt;
  int incomplete_slice_cnt;
  int check_md5_frame_cnt;
  bool out_of_order_pkt = false; /* out of order pkt index */
  int* ooo_mapping;
  int slice_cnt;
  uint32_t slice_recv_lines;
  uint64_t slice_recv_timestamp;
};

int tx_next_frame(void* priv, uint16_t* next_frame_idx);
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
      ops.udp_port[ST_PORT_P]++;                            \
      ops.udp_port[ST_PORT_R]++;                            \
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
      ops.udp_port[ST_PORT_P]++;                   \
      ops.udp_port[ST_PORT_R]++;                   \
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
        ops.udp_port[ST_PORT_P]++;                 \
        ops.udp_port[ST_PORT_R]++;                 \
        expect_cnt++;                              \
        A##_assert_cnt(expect_cnt);                \
      }                                            \
                                                   \
      for (int j = 0; j < step; j++) {             \
        ret = A##_free(handle[j]);                 \
        ASSERT_TRUE(ret >= 0);                     \
        expect_cnt--;                              \
        A##_assert_cnt(expect_cnt);                \
      }                                            \
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
    if (ctx->para.num_ports != 2) {        \
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

#define expect_test_rtp_pkt_size_2(A, pkt_sz, expect) \
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
    ops.rtp_ring_size = 1024;                         \
    ops.rtp_pkt_size = pkt_sz;                        \
    handle = A##_create(m_handle, &ops);              \
    if (expect)                                       \
      EXPECT_TRUE(handle != NULL);                    \
    else                                              \
      EXPECT_TRUE(handle == NULL);                    \
    if (handle) {                                     \
      auto ret = A##_free(handle);                    \
      EXPECT_GE(ret, 0);                              \
    }                                                 \
    delete test_ctx;                                  \
  } while (0)
