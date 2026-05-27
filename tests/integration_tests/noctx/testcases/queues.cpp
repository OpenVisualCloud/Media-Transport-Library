/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/test_fixture.hpp"

/* Test MTL initialization with more than 16 queues (32 TX + 32 RX). */
TEST_F(NoCtxTest, init_32_queues) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 32;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 32;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 32;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 32;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr) << "mtl_init failed with 32 queues";

  int ret = mtl_start(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_start failed with 32 queues";

  ret = mtl_stop(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_stop failed with 32 queues";
}

/* Test MTL initialization with 64 queues per direction. */
TEST_F(NoCtxTest, init_64_queues) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 64;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 64;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 64;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 64;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr) << "mtl_init failed with 64 queues";

  int ret = mtl_start(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_start failed with 64 queues";

  ret = mtl_stop(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_stop failed with 64 queues";
}

/* Test MTL initialization with 128 queues per direction. */
TEST_F(NoCtxTest, init_128_queues) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 128;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 128;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 128;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 128;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr) << "mtl_init failed with 128 queues";

  int ret = mtl_start(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_start failed with 128 queues";

  ret = mtl_stop(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_stop failed with 128 queues";
}

/* Test asymmetric queue counts: more TX than RX. */
TEST_F(NoCtxTest, init_asymmetric_queues_tx_heavy) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 64;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 32;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 64;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 32;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr)
      << "mtl_init failed with asymmetric queues (64tx/32rx)";

  int ret = mtl_start(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_start failed with asymmetric queues";

  ret = mtl_stop(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_stop failed with asymmetric queues";
}

/* Test asymmetric queue counts: more RX than TX. */
TEST_F(NoCtxTest, init_asymmetric_queues_rx_heavy) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = 32;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = 64;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = 32;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = 64;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr)
      << "mtl_init failed with asymmetric queues (32tx/64rx)";

  int ret = mtl_start(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_start failed with asymmetric queues";

  ret = mtl_stop(ctx->handle);
  ASSERT_EQ(ret, 0) << "mtl_stop failed with asymmetric queues";
}
