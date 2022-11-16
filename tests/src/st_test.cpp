/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "log.h"
#include "tests.h"

int st_test_sch_cnt(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  return stats.sch_cnt;
}

bool st_test_dma_available(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_stats stats;
  struct mtl_cap cap;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  ret = st_get_cap(handle, &cap);
  if (ret < 0) return ret;

  if (stats.dma_dev_cnt < cap.dma_dev_cnt_max)
    return true;
  else
    return false;
}

static void init_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle;
  struct mtl_init_params para;

  memset(&para, 0, sizeof(para));
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  para.num_ports = 1;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(st_p_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  snprintf(para.port[MTL_PORT_P], sizeof(para.port[MTL_PORT_P]), "0000:55:00.0");
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(st_r_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  /* test with 0 num_ports */
  para.num_ports = 0;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  /* test with crazy big num_ports */
  para.num_ports = 100;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  /* test with negative big num_ports */
  para.num_ports = -1;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  para.num_ports = 1;
  para.tx_sessions_cnt_max = -1;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  para.tx_sessions_cnt_max = 1;
  para.rx_sessions_cnt_max = -1;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);
}

TEST(Main, init_expect_fail) { init_expect_fail_test(); }

static void reinit_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle;

  handle = st_init(&ctx->para);
  EXPECT_TRUE(handle == NULL);
}

TEST(Main, re_init_fail) { reinit_expect_fail_test(); }

static void start_stop_test(int repeat) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  for (int i = 0; i < repeat; i++) {
    ret = st_start(handle);
    EXPECT_GE(ret, 0);
    ret = st_stop(handle);
    EXPECT_GE(ret, 0);
  }
}

TEST(Main, start_stop_single) { start_stop_test(1); }

TEST(Main, start_stop_multi) { start_stop_test(5); }

static void start_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  ret = st_start(handle);
  EXPECT_GE(ret, 0);
  ret = st_start(handle);
  EXPECT_GE(ret, 0);
  ret = st_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, start_expect_fail) { start_expect_fail_test(); }

static void stop_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  ret = st_stop(handle);
  EXPECT_GE(ret, 0);

  ret = st_start(handle);
  EXPECT_GE(ret, 0);
  ret = st_stop(handle);
  EXPECT_GE(ret, 0);

  ret = st_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, stop_expect_fail) { stop_expect_fail_test(); }

TEST(Main, get_cap) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  struct mtl_cap cap;
  int ret;

  ret = st_get_cap(handle, &cap);
  EXPECT_GE(ret, 0);
  EXPECT_GT(cap.tx_sessions_cnt_max, 0);
  EXPECT_GT(cap.rx_sessions_cnt_max, 0);
  info("dma dev count %u\n", cap.dma_dev_cnt_max);
  info("init_flags 0x%lx\n", cap.init_flags);
}

TEST(Main, get_stats) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  struct mtl_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st20_tx_sessions_cnt, 0);
  EXPECT_EQ(stats.st30_tx_sessions_cnt, 0);
  EXPECT_EQ(stats.st40_tx_sessions_cnt, 0);
  EXPECT_EQ(stats.st20_rx_sessions_cnt, 0);
  EXPECT_EQ(stats.st30_rx_sessions_cnt, 0);
  EXPECT_EQ(stats.st40_rx_sessions_cnt, 0);
  EXPECT_EQ(stats.sch_cnt, 1);
}

static int test_lcore_cnt(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  return stats.lcore_cnt;
}

static void test_lcore_one(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  int base_cnt = test_lcore_cnt(ctx);
  int ret;
  unsigned int lcore;

  ret = st_get_lcore(handle, &lcore);
  ASSERT_TRUE(ret >= 0);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + 1);
  ret = st_put_lcore(handle, lcore);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);
}

TEST(Main, lcore) {
  struct st_tests_context* ctx = st_test_ctx();

  test_lcore_one(ctx);
}

TEST(Main, lcore_max) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int base_cnt = test_lcore_cnt(ctx), max = 100;
  int ret, i;
  unsigned int lcore[100];

  for (i = 0; i < max; i++) {
    ret = st_get_lcore(handle, &lcore[i]);
    if (ret < 0) break;
  }
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + i);
  max = i;
  for (i = 0; i < max; i++) st_put_lcore(handle, lcore[i]);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);

  test_lcore_one(ctx);
}

TEST(Main, lcore_expect_fail) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  int ret = st_put_lcore(handle, 10000);
  ASSERT_LT(ret, 0);
  test_lcore_one(ctx);
}

static bool test_dev_started(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  if (stats.dev_started)
    return true;
  else
    return false;
}

TEST(Main, dev_started) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  int ret = st_start(handle);
  EXPECT_GE(ret, 0);
  EXPECT_TRUE(test_dev_started(ctx));
  ret = st_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, bandwidth) {
  uint64_t bandwidth_1080p_mps = st20_1080p59_yuv422_10bit_bandwidth_mps();
  uint64_t bandwidth_1080p = 0;
  int ret = st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                                   &bandwidth_1080p);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(bandwidth_1080p / 1000 / 1000, bandwidth_1080p_mps);

  uint64_t bandwidth_720p = 0;
  ret = st20_get_bandwidth_bps(1280, 720, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                               &bandwidth_720p);
  EXPECT_GE(ret, 0);
  EXPECT_GT(bandwidth_1080p, bandwidth_720p);
}

static void st20_frame_size_test() {
  uint32_t w = 1920;
  uint32_t h = 1080;
  size_t size, expect_size;

  size = st20_frame_size(ST20_FMT_YUV_422_10BIT, w, h);
  expect_size = w * h * 5 / 2;
  EXPECT_EQ(size, expect_size);
}

TEST(Main, frame_size) { st20_frame_size_test(); }

static void fmt_frame_equal_transport_test() {
  bool equal;

  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                       ST20_FMT_YUV_422_10BIT);
  EXPECT_TRUE(equal);
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PACKED8, ST20_FMT_YUV_422_8BIT);
  EXPECT_TRUE(equal);
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_RGB8, ST20_FMT_RGB_8BIT);
  EXPECT_TRUE(equal);

  equal =
      st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR10LE, ST20_FMT_YUV_422_10BIT);
  EXPECT_FALSE(equal);
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_V210, ST20_FMT_YUV_422_10BIT);
  EXPECT_FALSE(equal);
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PLANAR8, ST20_FMT_YUV_422_8BIT);
  EXPECT_FALSE(equal);
  equal =
      st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422PACKED8, ST20_FMT_YUV_422_12BIT);
  EXPECT_FALSE(equal);
}

TEST(Main, fmt_equal_transport) { fmt_frame_equal_transport_test(); }

static void fmt_frame_fom_transport_test() {
  enum st_frame_fmt to_fmt;

  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_422_10BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_422_8BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_YUV422PACKED8);
  to_fmt = st_frame_fmt_from_transport(ST20_FMT_RGB_8BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_RGB8);

  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_444_16BIT); /* not support now */
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_MAX);
}

TEST(Main, fmt_frame_transport) { fmt_frame_fom_transport_test(); }

static void fmt_frame_to_transport_test() {
  enum st20_fmt to_fmt;

  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  EXPECT_TRUE(to_fmt == ST20_FMT_YUV_422_10BIT);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422PACKED8);
  EXPECT_TRUE(to_fmt == ST20_FMT_YUV_422_8BIT);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_RGB8);
  EXPECT_TRUE(to_fmt == ST20_FMT_RGB_8BIT);

  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422PLANAR10LE);
  EXPECT_TRUE(to_fmt == ST20_FMT_MAX);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_V210);
  EXPECT_TRUE(to_fmt == ST20_FMT_MAX);
}

TEST(Main, fmt_to_transport) { fmt_frame_to_transport_test(); }

static void size_page_align_test() {
  size_t pg_sz = 4096;
  size_t sz, expect_sz;

  sz = pg_sz * 1;
  expect_sz = pg_sz * 1;
  sz = st_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 1 + 100;
  expect_sz = pg_sz * 2;
  sz = st_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4;
  expect_sz = pg_sz * 4;
  sz = st_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4 - 1;
  expect_sz = pg_sz * 4;
  sz = st_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4 + 1;
  expect_sz = pg_sz * 5;
  sz = st_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);
}

TEST(Main, size_page_align) { size_page_align_test(); }
