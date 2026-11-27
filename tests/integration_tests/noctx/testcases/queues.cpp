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

  /* Verify configured RX queue count is honored by probing dedicated RX queues. */
  if (rx_q > 0 && mtl_pmd_is_dpdk_based(ctx->handle, MTL_PORT_P) &&
      mtl_rss_mode_get(ctx->handle) == MTL_RSS_MODE_NONE &&
      !(ctx->para.flags & MTL_FLAG_SHARED_RX_QUEUE)) {
    const uint16_t probe_max = std::min<uint16_t>(rx_q, 64);
    struct st40_rx_ops rx_ops = {};
    rx_ops.name = "rx_queue_probe";
    rx_ops.num_port = 1;
    memcpy(rx_ops.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
           MTL_IP_ADDR_LEN);
    snprintf(rx_ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
             ctx->para.port[MTL_PORT_P]);
    rx_ops.udp_port[MTL_SESSION_PORT_P] = 20000;
    rx_ops.payload_type = 113;
    rx_ops.type = ST40_TYPE_RTP_LEVEL;
    rx_ops.rtp_ring_size = 128;
    rx_ops.flags = ST40_RX_FLAG_DATA_PATH_ONLY;
    rx_ops.notify_rtp_ready = [](void*) { return 0; };

    std::vector<st40_rx_handle> rx_handles;
    rx_handles.reserve(probe_max);
    for (uint16_t i = 0; i < probe_max; i++) {
      st40_rx_handle h = st40_rx_create(ctx->handle, &rx_ops);
      if (!h) break;
      rx_handles.push_back(h);
      rx_ops.udp_port[MTL_SESSION_PORT_P]++;
    }
    const uint16_t created = static_cast<uint16_t>(rx_handles.size());
    for (st40_rx_handle h : rx_handles) EXPECT_EQ(st40_rx_free(h), 0);
    EXPECT_GT(created, 0u) << "no dedicated RX queues usable for " << label;
    if (rx_q > tx_q && tx_q == 0) {
      EXPECT_GE(created, 2u) << "rx-only rx_queues_cnt=" << rx_q
                             << " did not allocate multiple RX queues";
    }
  }

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
