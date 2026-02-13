/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "log.h"
#include "tests.hpp"

int st_test_sch_cnt(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_var_info var;
  int ret;

  ret = mtl_get_var_info(handle, &var);
  if (ret < 0) return ret;

  return var.sch_cnt;
}

bool st_test_dma_available(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_var_info var;
  struct mtl_fix_info fix;
  int ret;

  if (ctx->iova == MTL_IOVA_MODE_PA) {
    info("%s, DMA not full supported under IOVA PA mode\n", __func__);
    return false;
  }

  ret = mtl_get_var_info(handle, &var);
  if (ret < 0) return ret;

  ret = mtl_get_fix_info(handle, &fix);
  if (ret < 0) return ret;

  if (var.dma_dev_cnt < fix.dma_dev_cnt_max)
    return true;
  else
    return false;
}

static void init_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle;
  struct mtl_init_params para;

  memset(&para, 0, sizeof(para));
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  para.num_ports = 1;
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(mtl_p_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_P], MTL_IP_ADDR_LEN);
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  snprintf(para.port[MTL_PORT_P], sizeof(para.port[MTL_PORT_P]), "0000:55:00.0");
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(mtl_r_sip_addr(&para), ctx->para.sip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  /* test with 0 num_ports */
  para.num_ports = 0;
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  /* test with crazy big num_ports */
  para.num_ports = 100;
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);

  /* test with negative big num_ports */
  para.num_ports = -1;
  handle = mtl_init(&para);
  EXPECT_TRUE(handle == NULL);
}

TEST(Main, init_expect_fail) {
  init_expect_fail_test();
}

static void reinit_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle;

  handle = mtl_init(&ctx->para);
  EXPECT_TRUE(handle == NULL);
}

TEST(Main, re_init_fail) {
  reinit_expect_fail_test();
}

static void start_stop_test(int repeat) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  for (int i = 0; i < repeat; i++) {
    ret = mtl_start(handle);
    EXPECT_GE(ret, 0);
    ret = mtl_stop(handle);
    EXPECT_GE(ret, 0);
  }
}

TEST(Main, start_stop_single) {
  start_stop_test(1);
}

TEST(Main, start_stop_multi) {
  start_stop_test(5);
}

static void start_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  ret = mtl_start(handle);
  EXPECT_GE(ret, 0);
  ret = mtl_start(handle);
  EXPECT_GE(ret, 0);
  ret = mtl_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, start_expect_fail) {
  start_expect_fail_test();
}

static void stop_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  int ret;

  ret = mtl_stop(handle);
  EXPECT_GE(ret, 0);

  ret = mtl_start(handle);
  EXPECT_GE(ret, 0);
  ret = mtl_stop(handle);
  EXPECT_GE(ret, 0);

  ret = mtl_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, stop_expect_fail) {
  stop_expect_fail_test();
}

TEST(Main, get_fix) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  struct mtl_fix_info fix;
  int ret;

  ret = mtl_get_fix_info(handle, &fix);
  EXPECT_GE(ret, 0);
  info("dma dev count %u\n", fix.dma_dev_cnt_max);
  info("init_flags 0x%" PRIx64 "\n", fix.init_flags);
}

TEST(Main, get_var) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;
  struct mtl_var_info var;
  int ret;

  ret = mtl_get_var_info(handle, &var);
  EXPECT_GE(ret, 0);
}

static int test_lcore_cnt(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_var_info var;
  int ret;

  ret = mtl_get_var_info(handle, &var);
  if (ret < 0) return ret;

  return var.lcore_cnt;
}

static void test_lcore_one(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  int base_cnt = test_lcore_cnt(ctx);
  int ret;
  unsigned int lcore;

  ret = mtl_get_lcore(handle, &lcore);
  ASSERT_TRUE(ret >= 0);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + 1);
  ret = mtl_put_lcore(handle, lcore);
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
    ret = mtl_get_lcore(handle, &lcore[i]);
    if (ret < 0) break;
  }
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt + i);
  max = i;
  for (i = 0; i < max; i++) mtl_put_lcore(handle, lcore[i]);
  EXPECT_EQ(test_lcore_cnt(ctx), base_cnt);

  test_lcore_one(ctx);
}

TEST(Main, lcore_expect_fail) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  int ret = mtl_put_lcore(handle, 10000);
  ASSERT_LT(ret, 0);
  test_lcore_one(ctx);
}

static bool test_dev_started(struct st_tests_context* ctx) {
  mtl_handle handle = ctx->handle;
  struct mtl_var_info var;
  int ret;

  ret = mtl_get_var_info(handle, &var);
  if (ret < 0) return false;

  return var.dev_started;
}

TEST(Main, dev_started) {
  struct st_tests_context* ctx = st_test_ctx();
  mtl_handle handle = ctx->handle;

  int ret = mtl_start(handle);
  EXPECT_GE(ret, 0);
  EXPECT_TRUE(test_dev_started(ctx));
  ret = mtl_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, bandwidth) {
  uint64_t bandwidth_1080p_mps = st20_1080p59_yuv422_10bit_bandwidth_mps();
  uint64_t bandwidth_1080p = 0;
  int ret = st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94,
                                   false, &bandwidth_1080p);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(bandwidth_1080p / 1000 / 1000, bandwidth_1080p_mps);

  uint64_t bandwidth_720p = 0;
  ret = st20_get_bandwidth_bps(1280, 720, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94, false,
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

TEST(Main, st20_frame_size) {
  st20_frame_size_test();
}

static void fmt_frame_equal_transport_test() {
  bool equal;

  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10,
                                       ST20_FMT_YUV_422_10BIT);
  EXPECT_TRUE(equal);
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_8BIT);
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
  equal = st_frame_fmt_equal_transport(ST_FRAME_FMT_UYVY, ST20_FMT_YUV_422_12BIT);
  EXPECT_FALSE(equal);
}

TEST(Main, fmt_equal_transport) {
  fmt_frame_equal_transport_test();
}

static void fmt_frame_fom_transport_test() {
  enum st_frame_fmt to_fmt;

  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_422_10BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_422_8BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_UYVY);
  to_fmt = st_frame_fmt_from_transport(ST20_FMT_RGB_8BIT);
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_RGB8);

  to_fmt = st_frame_fmt_from_transport(ST20_FMT_YUV_444_16BIT); /* not support now */
  EXPECT_TRUE(to_fmt == ST_FRAME_FMT_MAX);
}

TEST(Main, fmt_frame_transport) {
  fmt_frame_fom_transport_test();
}

static void fmt_frame_to_transport_test() {
  enum st20_fmt to_fmt;

  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422RFC4175PG2BE10);
  EXPECT_TRUE(to_fmt == ST20_FMT_YUV_422_10BIT);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_UYVY);
  EXPECT_TRUE(to_fmt == ST20_FMT_YUV_422_8BIT);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_RGB8);
  EXPECT_TRUE(to_fmt == ST20_FMT_RGB_8BIT);

  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_YUV422PLANAR10LE);
  EXPECT_TRUE(to_fmt == ST20_FMT_MAX);
  to_fmt = st_frame_fmt_to_transport(ST_FRAME_FMT_V210);
  EXPECT_TRUE(to_fmt == ST20_FMT_MAX);
}

TEST(Main, fmt_to_transport) {
  fmt_frame_to_transport_test();
}

static void frame_api_test() {
  uint32_t w = 1920;
  uint32_t h = 1080;
  size_t size;
  enum st_frame_fmt fmt;
  size_t szero = 0;

  /* yuv */
  for (int i = ST_FRAME_FMT_YUV_START; i < ST_FRAME_FMT_YUV_END; i++) {
    fmt = (enum st_frame_fmt)i;
    size = st_frame_size(fmt, w, h, false);
    EXPECT_GT(size, szero);
    EXPECT_GT(st_frame_fmt_planes(fmt), 0);
    EXPECT_GT(st_frame_least_linesize(fmt, w, 0), szero);
  }
  /* rgb */
  for (int i = ST_FRAME_FMT_RGB_START; i < ST_FRAME_FMT_RGB_END; i++) {
    fmt = (enum st_frame_fmt)i;
    size = st_frame_size(fmt, w, h, false);
    EXPECT_GT(size, szero);
    EXPECT_GT(st_frame_fmt_planes(fmt), 0);
    EXPECT_GT(st_frame_least_linesize(fmt, w, 0), szero);
  }
  /* codestream */
  for (int i = ST_FRAME_FMT_CODESTREAM_START; i < ST_FRAME_FMT_CODESTREAM_END; i++) {
    fmt = (enum st_frame_fmt)i;
    size = st_frame_size(fmt, w, h, false);
    EXPECT_EQ(size, szero);
    EXPECT_EQ(st_frame_fmt_planes(fmt), 1);
    EXPECT_EQ(st_frame_least_linesize(fmt, w, 0), szero);
  }

  /* invalid fmt */
  size = st_frame_size(ST_FRAME_FMT_YUV_END, w, h, false);
  EXPECT_EQ(size, szero);
  size = st_frame_size(ST_FRAME_FMT_RGB_END, w, h, false);
  EXPECT_EQ(size, szero);
  size = st_frame_size(ST_FRAME_FMT_CODESTREAM_END, w, h, false);
  EXPECT_EQ(size, szero);
  size = st_frame_size(ST_FRAME_FMT_MAX, w, h, false);
  EXPECT_EQ(size, szero);
}

static void frame_name_test() {
  int result;
  const char* fail = "unknown";
  const char* name;
  enum st_frame_fmt fmt;

  /* yuv */
  for (int i = ST_FRAME_FMT_YUV_START; i < ST_FRAME_FMT_YUV_END; i++) {
    fmt = (enum st_frame_fmt)i;
    name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }
  /* rgb */
  for (int i = ST_FRAME_FMT_RGB_START; i < ST_FRAME_FMT_RGB_END; i++) {
    fmt = (enum st_frame_fmt)i;
    name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }
  /* codestream */
  for (int i = ST_FRAME_FMT_CODESTREAM_START; i < ST_FRAME_FMT_CODESTREAM_END; i++) {
    fmt = (enum st_frame_fmt)i;
    name = st_frame_fmt_name(fmt);
    EXPECT_NE(strcmp(fail, name), 0);
    EXPECT_EQ(st_frame_name_to_fmt(name), fmt);
  }

  /* invalid fmt */
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_YUV_END));
  EXPECT_EQ(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_RGB_END));
  EXPECT_EQ(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_CODESTREAM_END));
  EXPECT_EQ(result, 0);
  result = strcmp(fail, st_frame_fmt_name(ST_FRAME_FMT_MAX));
  EXPECT_EQ(result, 0);
  /* invalid name */
  EXPECT_EQ(st_frame_name_to_fmt(fail), ST_FRAME_FMT_MAX);
}

TEST(Main, frame_api) {
  frame_api_test();
}
TEST(Main, frame_name) {
  frame_name_test();
}

static void size_page_align_test() {
  size_t pg_sz = 4096;
  size_t sz, expect_sz;

  sz = pg_sz * 1;
  expect_sz = pg_sz * 1;
  sz = mtl_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 1 + 100;
  expect_sz = pg_sz * 2;
  sz = mtl_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4;
  expect_sz = pg_sz * 4;
  sz = mtl_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4 - 1;
  expect_sz = pg_sz * 4;
  sz = mtl_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);

  sz = pg_sz * 4 + 1;
  expect_sz = pg_sz * 5;
  sz = mtl_size_page_align(sz, pg_sz);
  EXPECT_EQ(sz, expect_sz);
}

TEST(Main, size_page_align) {
  size_page_align_test();
}

class fps_23_98 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_23_98, conv_fps_to_st_fps_23_98_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_23_98,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 22.00),
                                     std::make_tuple(ST_FPS_MAX, 22.97),
                                     std::make_tuple(ST_FPS_P23_98, 22.98),
                                     std::make_tuple(ST_FPS_P23_98, 23.98),
                                     std::make_tuple(ST_FPS_P23_98, 23.99),
                                     std::make_tuple(ST_FPS_P24, 24.00)));

class fps_24 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_24, conv_fps_to_st_fps_24_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_24,
                   ::testing::Values(std::make_tuple(ST_FPS_P23_98, 23.00),
                                     std::make_tuple(ST_FPS_P24, 24.00),
                                     std::make_tuple(ST_FPS_P24, 24.99),
                                     std::make_tuple(ST_FPS_P25, 25.00)));

class fps_25 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_25, conv_fps_to_st_fps_25_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_25,
                   ::testing::Values(std::make_tuple(ST_FPS_P25, 25.00),
                                     std::make_tuple(ST_FPS_P25, 26.00),
                                     std::make_tuple(ST_FPS_MAX, 27.00)));

class fps_29_97 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_29_97, conv_fps_to_st_fps_29_97_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_29_97,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 28.00),
                                     std::make_tuple(ST_FPS_MAX, 28.50),
                                     std::make_tuple(ST_FPS_P29_97, 29.97),
                                     std::make_tuple(ST_FPS_P29_97, 29.99),
                                     std::make_tuple(ST_FPS_P30, 30.00)));

class fps_30 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_30, conv_fps_to_st_fps_30_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_30,
                   ::testing::Values(std::make_tuple(ST_FPS_P30, 30.00),
                                     std::make_tuple(ST_FPS_P30, 31.00),
                                     std::make_tuple(ST_FPS_MAX, 31.01),
                                     std::make_tuple(ST_FPS_MAX, 32.00)));

class fps_50 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

TEST_P(fps_50, conv_fps_to_st_fps_50_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

PARAMETERIZED_TEST(Main, fps_50,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 48.00),
                                     std::make_tuple(ST_FPS_P50, 49.00),
                                     std::make_tuple(ST_FPS_P50, 49.50),
                                     std::make_tuple(ST_FPS_P50, 50.00),
                                     std::make_tuple(ST_FPS_P50, 50.50),
                                     std::make_tuple(ST_FPS_P50, 51.00),
                                     std::make_tuple(ST_FPS_MAX, 52.00)));

class fps_59_94 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

PARAMETERIZED_TEST(Main, fps_59_94,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 58.93),
                                     std::make_tuple(ST_FPS_P59_94, 58.94),
                                     std::make_tuple(ST_FPS_P59_94, 59.94),
                                     std::make_tuple(ST_FPS_P59_94, 59.99),
                                     std::make_tuple(ST_FPS_P60, 60.00)));

TEST_P(fps_59_94, conv_fps_to_st_fps_50_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_60 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

PARAMETERIZED_TEST(Main, fps_60,
                   ::testing::Values(std::make_tuple(ST_FPS_P60, 60.00),
                                     std::make_tuple(ST_FPS_P60, 61.00),
                                     std::make_tuple(ST_FPS_MAX, 61.01),
                                     std::make_tuple(ST_FPS_MAX, 62.00)));

TEST_P(fps_60, conv_fps_to_st_fps_60_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_100 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

PARAMETERIZED_TEST(Main, fps_100,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 98.99),
                                     std::make_tuple(ST_FPS_P100, 99.00),
                                     std::make_tuple(ST_FPS_P100, 100.00),
                                     std::make_tuple(ST_FPS_P100, 101.00),
                                     std::make_tuple(ST_FPS_MAX, 101.01)));

TEST_P(fps_100, conv_fps_to_st_fps_100_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_119_98 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

PARAMETERIZED_TEST(Main, fps_119_98,
                   ::testing::Values(std::make_tuple(ST_FPS_MAX, 118.87),
                                     std::make_tuple(ST_FPS_P119_88, 118.88),
                                     std::make_tuple(ST_FPS_P119_88, 119.88),
                                     std::make_tuple(ST_FPS_P119_88, 119.99),
                                     std::make_tuple(ST_FPS_P120, 120.00)));

TEST_P(fps_119_98, conv_fps_to_st_fps_119_98_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}

class fps_120 : public ::testing::TestWithParam<std::tuple<enum st_fps, double>> {};

PARAMETERIZED_TEST(Main, fps_120,
                   ::testing::Values(std::make_tuple(ST_FPS_P120, 120.00),
                                     std::make_tuple(ST_FPS_P120, 120.01),
                                     std::make_tuple(ST_FPS_P120, 121.00),
                                     std::make_tuple(ST_FPS_MAX, 121.01),
                                     std::make_tuple(ST_FPS_MAX, 122.00)));

TEST_P(fps_120, conv_fps_to_st_fps_120_test) {
  enum st_fps expect = st_frame_rate_to_st_fps(std::get<1>(GetParam()));
  EXPECT_EQ(expect, std::get<0>(GetParam()));
}
