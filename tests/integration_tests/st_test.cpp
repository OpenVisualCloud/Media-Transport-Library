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
