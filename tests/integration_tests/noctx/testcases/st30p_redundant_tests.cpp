/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Proves ST 2022-7 audio merge with the redundant copy 10 ms behind, and with the
 * primary stream stopping mid-run, from packet, loss and frame counts.
 * See README.md, "Test catalogue".
 */

#include <chrono>
#include <cstring>
#include <thread>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "strategies/st30p_strategies.hpp"

namespace {

/* 1 s of 10 ms buffers, so a stalled test consumer cannot drop merged frames */
constexpr uint16_t kRxFramebuffCnt = 100;
constexpr uint kTestedLatencyMs = 10;

/* Indexes into --port_list. */
constexpr int kRxPortP = 0;
constexpr int kRxPortR = 1;
constexpr int kPrimaryTxPort = 2;
constexpr int kRedundantTxPort = 3;

struct RedundantAudioSetup {
  St30pHandler* rx = nullptr;
  St30pHandler* primary = nullptr;
  St30pHandler* latency = nullptr;
  St30pUserPacingOracle* primaryStrategy = nullptr;
  St30pUserPacingOracle* latencyStrategy = nullptr;
};

/* Creates and starts RX (P+R, no strategy) + primary TX + redundant TX (-10 ms). */
RedundantAudioSetup setupRedundantAudio(NoCtxTest* test, st_tests_context* ctx) {
  RedundantAudioSetup s;
  auto makeTxPlan = [](St30pHandler* handler) {
    auto* strategy = new St30pUserPacingOracle(handler, /*enableRx=*/false);
    strategy->initializeTiming(handler);
    return strategy;
  };

  auto rxBundle = test->createSt30pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true, nullptr, [](St30pHandler* handler) {
        handler->sessionsOpsRx.framebuff_cnt = kRxFramebuffCnt;
        handler->setSessionPorts(SESSION_SKIP_PORT, kRxPortP, SESSION_SKIP_PORT,
                                 kRxPortR);
      });
  auto primaryBundle = test->createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false, makeTxPlan, [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        handler->setSessionPorts(kPrimaryTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto latencyBundle = test->createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false, makeTxPlan, [ctx](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.rtp_timestamp_delta_us =
            -static_cast<int>(kTestedLatencyMs * 1000);
        handler->setSessionPorts(kRedundantTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
      });

  s.rx = rxBundle.handler;
  s.primary = primaryBundle.handler;
  s.latency = latencyBundle.handler;
  s.primaryStrategy = static_cast<St30pUserPacingOracle*>(primaryBundle.strategy);
  s.latencyStrategy = static_cast<St30pUserPacingOracle*>(latencyBundle.strategy);

  s.rx->startSessionRx();
  s.primary->startSessionTx();
  s.latency->startSessionTx();
  NoCtxTest::StartFakePtpClock();
  return s;
}

}  // namespace

/* Common to both tests:
 * Config:    4 ports, else requireStrictTopology(); initDefaultContext(). PCM16 48 kHz
 *            stereo, 1 ms packets, 10 ms buffers (B), 3 TX buffers.
 *            RX kRxPortP + kRxPortR (P and R), kRxFramebuffCnt = 100 buffers;
 *            primary TX kPrimaryTxPort -> P mcast; redundant TX kRedundantTxPort
 *            -> R mcast, UDP port + 1, rtp_timestamp_delta_us = -kTestedLatencyMs *
 *            1000 (-10 ms); both TX ST30P_TX_FLAG_USER_PACING.
 * Plan:      both TX St30pUserPacingOracle, t_user(n) = (60 + n) * B from PTP
 *            zero (StartFakePtpClock() before mtl_start()). RX counts via rxFrames().
 * Expect:    every TX and RX buffer also passes the St30pHandler thread checks.
 * Skip/Fail: none.
 */

/* st30p_redundant_latency
 * Plan:      all three sessions run 20 s; stats read after they stop.
 * Expect:    1. RX packets on P and on R == primary TX packets +- 10 %
 *            2. stat_lost_packets <= (P + R packets) / 1000
 *            3. RX buffers == primary TX buffers (idx_tx) +- 1 %
 *
 * TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency) {
  if (ctx->para.num_ports < 4) {
    requireStrictTopology("st30p_redundant_latency needs at least 4 ports");
    return;
  }

  initDefaultContext();
  if (HasFatalFailure()) return;

  auto s = setupRedundantAudio(this, ctx);
  ASSERT_EQ(mtl_start(ctx->handle), 0);
  sleepUntilFailure(20);

  s.latency->session.stop();
  s.primary->session.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  s.rx->session.stop();

  st30_rx_user_stats stats = {};
  ASSERT_EQ(st30p_rx_get_session_stats(s.rx->sessionsHandleRx, &stats), 0);
  st30_tx_user_stats statsTxPrimary = {};
  ASSERT_EQ(st30p_tx_get_session_stats(s.primary->sessionsHandleTx, &statsTxPrimary), 0);

  uint64_t packetsSend = statsTxPrimary.common.port[0].packets;
  uint64_t packetsRecievedPort0 = stats.common.port[0].packets;
  uint64_t packetsRecievedPort1 = stats.common.port[1].packets;
  uint64_t framesSend = s.primaryStrategy->idx_tx;
  uint64_t framesRecieved = s.rx->rxFrames();

  ASSERT_NEAR(packetsSend, packetsRecievedPort0, packetsSend / 10)
      << "Comparison against primary stream (port 0)";
  ASSERT_NEAR(packetsSend, packetsRecievedPort1, packetsSend / 10)
      << "Comparison against primary stream (port 1)";
  ASSERT_LE(stats.common.stat_lost_packets,
            (packetsRecievedPort0 + packetsRecievedPort1) / 1000)
      << "Lost packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream; packets refused for lack of an RX buffer: "
      << stats.stat_slot_get_frame_fail;
}

/* st30p_redundant_latency2
 * Plan:      all run 10 s, the primary TX stops, the redundant TX runs 10 s more;
 *            stats read after all stop.
 * Expect:    1. RX packets on R == redundant TX packets +- 10 %
 *            2. RX packets on P > 0
 *            3. stat_pkts_received == redundant TX packets +- 1 %
 *            4. stat_lost_packets <= (P + R packets) / 1000
 *            5. RX buffers == redundant TX buffers (idx_tx) +- 1 %
 *
 * TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency2) {
  if (ctx->para.num_ports < 4) {
    requireStrictTopology("st30p_redundant_latency2 needs at least 4 ports");
    return;
  }

  initDefaultContext();
  if (HasFatalFailure()) return;

  auto s = setupRedundantAudio(this, ctx);
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  sleepUntilFailure(10);
  s.primary->session.stop();
  sleepUntilFailure(10);

  s.latency->session.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  s.rx->session.stop();

  st30_rx_user_stats stats = {};
  ASSERT_EQ(st30p_rx_get_session_stats(s.rx->sessionsHandleRx, &stats), 0);
  st30_tx_user_stats statsTxRedundant = {};
  ASSERT_EQ(st30p_tx_get_session_stats(s.latency->sessionsHandleTx, &statsTxRedundant),
            0);

  uint64_t packetsSend = statsTxRedundant.common.port[0].packets;
  uint64_t packetsRecievedPort0 = stats.common.port[0].packets;
  uint64_t packetsRecievedPort1 = stats.common.port[1].packets;
  uint64_t framesSend = s.latencyStrategy->idx_tx;
  uint64_t framesRecieved = s.rx->rxFrames();

  /* In this test the primary TX stops after 10s while the redundant TX runs for 20s.
   * Port 0 (primary) only receives ~half the packets, port 1 (redundant) receives all.
   * Compare the redundant port against TX, and verify accepted packets match. */
  ASSERT_NEAR(packetsSend, packetsRecievedPort1, packetsSend / 10)
      << "Comparison against redundant stream (port 1)";
  ASSERT_GT(packetsRecievedPort0, 0u) << "Primary port must have received packets";
  ASSERT_NEAR(packetsSend, stats.common.stat_pkts_received, packetsSend / 100)
      << "Accepted packets should match TX";
  ASSERT_LE(stats.common.stat_lost_packets,
            (packetsRecievedPort0 + packetsRecievedPort1) / 1000)
      << "Lost packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream; packets refused for lack of an RX buffer: "
      << stats.stat_slot_get_frame_fail;
}
