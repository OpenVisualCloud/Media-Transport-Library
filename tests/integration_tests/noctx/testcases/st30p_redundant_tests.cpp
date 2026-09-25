/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Proves ST 2022-7 audio merge with the redundant copy 10 ms behind, and with the
 * primary stream stopping mid-run, from packet, loss and frame counts.
 * See README.md, "Test catalogue".
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st30p_handler.hpp"
#include "strategies/st30p_strategies.hpp"

/* 1 s of 10 ms buffers, so a stalled test consumer cannot drop merged frames */
static constexpr uint16_t kRxFramebuffCnt = 100;

/* Indexes into --port_list. */
constexpr int kRxPortP = 0;
constexpr int kRxPortR = 1;
constexpr int kPrimaryTxPort = 2;
constexpr int kRedundantTxPort = 3;

/* Common to both tests:
 * Config:    4 ports, else std::runtime_error; initDefaultContext(). PCM16 48 kHz
 *            stereo, 1 ms packets, 10 ms buffers (B), 3 TX buffers.
 *            RX kRxPortP + kRxPortR (P and R), kRxFramebuffCnt = 100 buffers;
 *            primary TX kPrimaryTxPort -> P mcast; redundant TX kRedundantTxPort
 *            -> R mcast, UDP port + 1, rtp_timestamp_delta_us = -testedLatencyMs *
 *            1000 (-10 ms); both TX kTxFlags = USER_PACING.
 * Plan:      both TX St30pRedundantStreamPlan, t_user(n) = (60 + n) * B from PTP
 *            zero (StartFakePtpClock() before mtl_start()). RX counts buffers.
 * Expect:    every TX and RX buffer also passes the St30pHandler thread checks.
 * Skip/Fail: none.
 */

/* st30p_redundant_latency
 * Plan:      all three sessions run 20 s; stats read after they stop.
 * Expect:    1. RX packets on P and on R == primary TX packets +- 10 %
 *            2. stat_lost_packets <= (P + R packets) / 1000
 *            3. RX buffers (idx_rx) == primary TX buffers (idx_tx) +- 1 %
 *
 * TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  initDefaultContext();

  uint testedLatencyMs = 10;
  constexpr uint32_t kTxFlags = ST30P_TX_FLAG_USER_PACING;

  auto rxBundle = createSt30pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        // handler->sessionsOpsRx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsRx.framebuff_cnt = kRxFramebuffCnt;
        handler->setSessionPorts(SESSION_SKIP_PORT, kRxPortP, SESSION_SKIP_PORT,
                                 kRxPortR);
      });
  auto* rxStrategy = static_cast<St30pRedundantStreamPlan*>(rxBundle.strategy);
  ASSERT_NE(rxBundle.handler, nullptr);
  ASSERT_NE(rxStrategy, nullptr);

  auto primaryBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(kPrimaryTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto* primaryStrategy = static_cast<St30pRedundantStreamPlan*>(primaryBundle.strategy);
  ASSERT_NE(primaryBundle.handler, nullptr);
  ASSERT_NE(primaryStrategy, nullptr);

  auto latencyBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [this, testedLatencyMs](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(kRedundantTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
      });
  ASSERT_NE(latencyBundle.handler, nullptr);
  ASSERT_NE(latencyBundle.strategy, nullptr);

  rxBundle.handler->startSessionRx();
  ASSERT_TRUE(waitForSession(rxBundle.handler->session));
  primaryBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(primaryBundle.handler->session));
  latencyBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));

  StartFakePtpClock();  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure(20);

  latencyBundle.handler->session.stop();
  primaryBundle.handler->session.stop();
  rxBundle.handler->session.stop();

  st30_rx_user_stats stats;
  st30p_rx_get_session_stats(rxBundle.handler->sessionsHandleRx, &stats);
  st30_tx_user_stats statsTxPrimary;
  st30p_tx_get_session_stats(primaryBundle.handler->sessionsHandleTx, &statsTxPrimary);

  uint64_t packetsSend = statsTxPrimary.common.port[0].packets;
  uint64_t packetsRecievedPort0 = stats.common.port[0].packets;
  uint64_t packetsRecievedPort1 = stats.common.port[1].packets;
  uint64_t framesSend = primaryStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

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
 *            5. RX buffers (idx_rx) == redundant TX buffers (idx_tx) +- 1 %
 *
 * TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency2) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  initDefaultContext();

  uint testedLatencyMs = 10;
  constexpr uint32_t kTxFlags = ST30P_TX_FLAG_USER_PACING;

  auto rxBundle = createSt30pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        // handler->sessionsOpsRx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsRx.framebuff_cnt = kRxFramebuffCnt;
        handler->setSessionPorts(SESSION_SKIP_PORT, kRxPortP, SESSION_SKIP_PORT,
                                 kRxPortR);
      });
  auto* rxStrategy = static_cast<St30pRedundantStreamPlan*>(rxBundle.strategy);
  ASSERT_NE(rxBundle.handler, nullptr);
  ASSERT_NE(rxStrategy, nullptr);

  auto primaryBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(kPrimaryTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  ASSERT_NE(primaryBundle.handler, nullptr);
  ASSERT_NE(primaryBundle.strategy, nullptr);

  auto latencyBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantStreamPlan(handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [this, testedLatencyMs](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(kRedundantTxPort, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
      });
  auto* latencyStrategy = static_cast<St30pRedundantStreamPlan*>(latencyBundle.strategy);
  ASSERT_NE(latencyBundle.handler, nullptr);
  ASSERT_NE(latencyStrategy, nullptr);

  rxBundle.handler->startSessionRx();
  ASSERT_TRUE(waitForSession(rxBundle.handler->session));
  primaryBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(primaryBundle.handler->session));
  latencyBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));

  StartFakePtpClock();
  mtl_start(ctx->handle);

  sleepUntilFailure(10);
  primaryBundle.handler->session.stop();
  sleepUntilFailure(10);

  latencyBundle.handler->session.stop();
  rxBundle.handler->session.stop();

  st30_rx_user_stats stats;
  st30p_rx_get_session_stats(rxBundle.handler->sessionsHandleRx, &stats);
  st30_tx_user_stats statsTxRedundant;
  st30p_tx_get_session_stats(latencyBundle.handler->sessionsHandleTx, &statsTxRedundant);

  uint64_t packetsSend = statsTxRedundant.common.port[0].packets;
  uint64_t packetsRecievedPort0 = stats.common.port[0].packets;
  uint64_t packetsRecievedPort1 = stats.common.port[1].packets;
  uint64_t framesSend = latencyStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

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
