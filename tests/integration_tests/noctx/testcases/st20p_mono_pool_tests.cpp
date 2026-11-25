/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <chrono>
#include <deque>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"

namespace {

constexpr int kSessions = 4;
constexpr int kWarmupS = 3;
/* 5 s x 59.94 fps ~= 300 frames per session */
constexpr int kMeasureS = 5;

void runMonoPoolSessions(NoCtxTest* test, st_tests_context* ctx, uint64_t monoPoolFlag) {
  if (ctx->para.num_ports < 2)
    throw std::runtime_error("mono pool test needs 2 ports, TX and RX");

  ctx->para.num_ports = 2; /* P and R only, so other ports of --port_list add no pools */
  ctx->para.flags |= monoPoolFlag;
  /* one 1080p59 session's quota per scheduler: each session gets its own */
  ctx->para.data_quota_mbs_per_sch = st20_1080p59_yuv422_10bit_bandwidth_mps();
  test->initDefaultContext();
  if (::testing::Test::HasFatalFailure()) return;

  static std::deque<std::string> names;
  std::vector<St20pHandler*> handlers;
  std::set<int> schs;
  for (int i = 0; i < kSessions; i++) {
    auto bundle = test->createSt20pHandlerBundle(
        /*createTx=*/true, /*createRx=*/true, nullptr, [i](St20pHandler* handler) {
          handler->fillSt20Ops(20000 + 2 * i, 3, ST20_FMT_YUV_422_10BIT, 1920, 1080, 112,
                               ST_FPS_P59_94);
          names.push_back("mono_tx_" + std::to_string(i));
          handler->sessionsOpsTx.name = names.back().c_str();
          names.push_back("mono_rx_" + std::to_string(i));
          handler->sessionsOpsRx.name = names.back().c_str();
          handler->sessionsOpsTx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
          handler->sessionsOpsRx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
        });
    St20pHandler* h = bundle.handler;
    ASSERT_NE(h->sessionsHandleTx, nullptr) << "session " << i;
    ASSERT_NE(h->sessionsHandleRx, nullptr) << "session " << i;
    schs.insert(st20p_tx_get_sch_idx(h->sessionsHandleTx));
    schs.insert(st20p_rx_get_sch_idx(h->sessionsHandleRx));
    handlers.push_back(h);
  }
  ASSERT_EQ(schs.size(), 2u * kSessions) << "sessions share a scheduler";

  NoCtxTest::StartFakePtpClock();
  for (St20pHandler* h : handlers) h->startSession();
  ASSERT_GE(mtl_start(ctx->handle), 0);

  std::this_thread::sleep_for(std::chrono::seconds(kWarmupS));
  std::vector<uint32_t> tx0, rx0;
  for (St20pHandler* h : handlers) {
    tx0.push_back(h->txFrames());
    rx0.push_back(h->rxFrames());
  }
  std::this_thread::sleep_for(std::chrono::seconds(kMeasureS));
  std::vector<double> txFps, rxFps;
  for (int i = 0; i < kSessions; i++) {
    txFps.push_back((double)(handlers[i]->txFrames() - tx0[i]) / kMeasureS);
    rxFps.push_back((double)(handlers[i]->rxFrames() - rx0[i]) / kMeasureS);
  }
  for (St20pHandler* h : handlers) h->stopSession();

  const double minFps = st_frame_rate(ST_FPS_P59_94) * 0.9;
  for (int i = 0; i < kSessions; i++) {
    ::testing::Test::RecordProperty("tx_fps_" + std::to_string(i),
                                    std::to_string(txFps[i]));
    ::testing::Test::RecordProperty("rx_fps_" + std::to_string(i),
                                    std::to_string(rxFps[i]));
    EXPECT_GE(txFps[i], minFps) << "session " << i << " TX";
    EXPECT_GE(rxFps[i], minFps) << "session " << i << " RX";
  }
}

} /* namespace */

/* st20p_rx_mono_pool_sessions_full_fps, st20p_tx_mono_pool_sessions_full_fps
 * Config:    runMonoPoolSessions(): MTL_FLAG_RX_MONO_POOL or MTL_FLAG_TX_MONO_POOL,
 *            ports P and R only, one 1080p59 session's quota per scheduler,
 *            initDefaultContext(); kSessions sessions TX
 *            TEST_PORT_1 -> RX TEST_PORT_2, 1080p59.94 YUV 4:2:2 10-bit BPM, RFC 4175
 *            in and out (no conversion), UDP 20000 + 2i, mono_tx_<i> / mono_rx_<i>.
 * Plan:      default pacing; kWarmupS after mtl_start(), then kMeasureS.
 * Expect:    1. every TX and RX session on a scheduler of its own
 *            2. per session, TX and RX fps over the window >= 90 % of 59.94
 *            Every TX and RX frame also passes the St20pHandler thread checks.
 * Skip/Fail: < 2 ports: std::runtime_error. No lcore for a session's scheduler: FAIL.
 */
TEST_F(NoCtxTest, st20p_rx_mono_pool_sessions_full_fps) {
  runMonoPoolSessions(this, ctx, MTL_FLAG_RX_MONO_POOL);
}

TEST_F(NoCtxTest, st20p_tx_mono_pool_sessions_full_fps) {
  runMonoPoolSessions(this, ctx, MTL_FLAG_TX_MONO_POOL);
}
