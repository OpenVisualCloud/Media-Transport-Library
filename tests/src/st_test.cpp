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

#include "tests.h"

int st_test_sch_cnt(struct st_tests_context* ctx) {
  st_handle handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  return stats.sch_cnt;
}

static void init_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  st_handle handle;
  struct st_init_params para;

  memset(&para, 0, sizeof(para));
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  para.num_ports = 1;
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(st_p_sip_addr(&para), ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  snprintf(para.port[ST_PORT_P], sizeof(para.port[ST_PORT_P]), "0000:55:00.0");
  handle = st_init(&para);
  EXPECT_TRUE(handle == NULL);

  memcpy(st_r_sip_addr(&para), ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);

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
  st_handle handle;

  handle = st_init(&ctx->para);
  EXPECT_TRUE(handle == NULL);
}

TEST(Main, re_init_fail) { reinit_expect_fail_test(); }

static void start_stop_test(int repeat) {
  struct st_tests_context* ctx = st_test_ctx();
  st_handle handle = ctx->handle;
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
  st_handle handle = ctx->handle;
  int ret;

  ret = st_start(handle);
  EXPECT_GE(ret, 0);
  ret = st_start(handle);
  EXPECT_LT(ret, 0);
  ret = st_stop(handle);
  EXPECT_GE(ret, 0);
}

TEST(Main, start_expect_fail) { start_expect_fail_test(); }

static void stop_expect_fail_test(void) {
  struct st_tests_context* ctx = st_test_ctx();
  st_handle handle = ctx->handle;
  int ret;

  ret = st_stop(handle);
  EXPECT_LT(ret, 0);

  ret = st_start(handle);
  EXPECT_GE(ret, 0);
  ret = st_stop(handle);
  EXPECT_GE(ret, 0);

  ret = st_stop(handle);
  EXPECT_LT(ret, 0);
}

TEST(Main, stop_expect_fail) { stop_expect_fail_test(); }

TEST(Main, get_cap) {
  struct st_tests_context* ctx = st_test_ctx();
  st_handle handle = ctx->handle;
  struct st_cap cap;
  int ret;

  ret = st_get_cap(handle, &cap);
  EXPECT_GE(ret, 0);
  EXPECT_GT(cap.tx_sessions_cnt_max, 0);
  EXPECT_GT(cap.rx_sessions_cnt_max, 0);
}

TEST(Main, get_stats) {
  struct st_tests_context* ctx = st_test_ctx();
  st_handle handle = ctx->handle;
  struct st_stats stats;
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
  EXPECT_EQ(stats.lcore_cnt, 0);
}

static int test_lcore_cnt(struct st_tests_context* ctx) {
  st_handle handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  if (ret < 0) return ret;

  return stats.lcore_cnt;
}

static void test_lcore_one(struct st_tests_context* ctx) {
  st_handle handle = ctx->handle;
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
  st_handle handle = ctx->handle;
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
  st_handle handle = ctx->handle;

  int ret = st_put_lcore(handle, 10000);
  ASSERT_LT(ret, 0);
  test_lcore_one(ctx);
}
