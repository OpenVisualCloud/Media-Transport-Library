/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <chrono>

#include "core/test_fixture.hpp"

/* Common to every test here:
 * Config:    mtl_init() with the SetUp() params (DEV_AUTO_START_STOP as on the command
 *            line), FakePtpClockNow, INFO log and the TX/RX queue counts below on
 *            both ports; no session.
 * Expect:    1. mtl_init() returns a handle
 *            2. mtl_start() == 0
 *            3. mtl_stop() == 0
 * Skip/Fail: none.
 * Except:    uninit_rx_only_64_queues_is_fast runs on P and R only with AUTO pacing,
 *            skips with fewer than 2 ports and also expects mtl_uninit() == 0 within
 *            its time budget.
 */

namespace {

void runQueueInitTest(st_tests_context* ctx, uint16_t tx_q, uint16_t rx_q,
                      const char* label) {
  ctx->para.tx_queues_cnt[MTL_PORT_P] = tx_q;
  ctx->para.rx_queues_cnt[MTL_PORT_P] = rx_q;
  ctx->para.tx_queues_cnt[MTL_PORT_R] = tx_q;
  ctx->para.rx_queues_cnt[MTL_PORT_R] = rx_q;

  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr) << "mtl_init failed with " << label;
  ASSERT_EQ(mtl_start(ctx->handle), 0) << "mtl_start failed with " << label;
  ASSERT_EQ(mtl_stop(ctx->handle), 0) << "mtl_stop failed with " << label;
}

}  // namespace

/* Test MTL initialization with more than 16 queues (32 TX + 32 RX). */
TEST_F(NoCtxTest, init_32_queues) {
  runQueueInitTest(ctx, 32, 32, "32 queues");
}

/* Test MTL initialization with 64 queues per direction. */
TEST_F(NoCtxTest, init_64_queues) {
  runQueueInitTest(ctx, 64, 64, "64 queues");
}

/* Test MTL initialization with 128 queues per direction. */
TEST_F(NoCtxTest, init_128_queues) {
  runQueueInitTest(ctx, 128, 128, "128 queues");
}

/* Test asymmetric queue counts: more TX than RX. */
TEST_F(NoCtxTest, init_asymmetric_queues_tx_heavy) {
  runQueueInitTest(ctx, 64, 32, "asymmetric queues (64tx/32rx)");
}

/* Test asymmetric queue counts: more RX than TX. */
TEST_F(NoCtxTest, init_asymmetric_queues_rx_heavy) {
  runQueueInitTest(ctx, 32, 64, "asymmetric queues (32tx/64rx)");
}

/* RX-only, 64 queues per port, AUTO pacing: mtl_uninit() alone stays fast. */
TEST_F(NoCtxTest, uninit_rx_only_64_queues_is_fast) {
  /* about 0.5 s on E810/E830 VFs; a TM commit per TX queue takes it past 2 s */
  constexpr int64_t kUninitBudgetMs = 1500;

  if (ctx->para.num_ports < 2) GTEST_SKIP() << "needs 2 ports, P and R";
  ctx->para.num_ports = 2; /* P and R only, so other ports of --port_list add no time */
  ctx->para.pacing = ST21_TX_PACING_WAY_AUTO;

  runQueueInitTest(ctx, 0, 64, "64 rx-only queues");
  if (HasFatalFailure()) return;

  const auto start = std::chrono::steady_clock::now();
  int ret = mtl_uninit(ctx->handle);
  const int64_t uninit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  ctx->handle = nullptr;

  EXPECT_EQ(ret, 0) << "mtl_uninit failed with 64 rx-only queues";
  EXPECT_LT(uninit_ms, kUninitBudgetMs) << "mtl_uninit took " << uninit_ms << " ms";
  RecordProperty("uninit_ms", uninit_ms);
}
