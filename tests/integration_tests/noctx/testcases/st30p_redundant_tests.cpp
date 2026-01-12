/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st30p_handler.hpp"
#include "strategies/st30p_strategies.hpp"

/* TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  initDefaultContext();

  uint testedLatencyMs = 10;

  auto rxBundle = createSt30pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(0, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        // handler->sessionsOpsRx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT, 1);
      });
  auto* rxStrategy = static_cast<St30pRedundantLatency*>(rxBundle.strategy);
  ASSERT_NE(rxBundle.handler, nullptr);
  ASSERT_NE(rxStrategy, nullptr);

  auto primaryBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(0, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(2, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto* primaryStrategy = static_cast<St30pRedundantLatency*>(primaryBundle.strategy);
  ASSERT_NE(primaryBundle.handler, nullptr);
  ASSERT_NE(primaryStrategy, nullptr);

  auto latencyBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [testedLatencyMs](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(testedLatencyMs, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [this, testedLatencyMs](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(3, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
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
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;
  uint64_t framesSend = primaryStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}

/* TODO: the tests fail with ST31_PTIME_80US */
TEST_F(NoCtxTest, st30p_redundant_latency2) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  initDefaultContext();

  uint testedLatencyMs = 10;

  auto rxBundle = createSt30pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(0, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        // handler->sessionsOpsRx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT, 1);
      });
  auto* rxStrategy = static_cast<St30pRedundantLatency*>(rxBundle.strategy);
  ASSERT_NE(rxBundle.handler, nullptr);
  ASSERT_NE(rxStrategy, nullptr);

  auto primaryBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(0, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->setSessionPorts(2, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  ASSERT_NE(primaryBundle.handler, nullptr);
  ASSERT_NE(primaryBundle.strategy, nullptr);

  auto latencyBundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [testedLatencyMs](St30pHandler* handler) {
        auto* strategy = new St30pRedundantLatency(testedLatencyMs, handler);
        strategy->initializeTiming(handler);
        return strategy;
      },
      [this, testedLatencyMs](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
        // handler->sessionsOpsTx.ptime = ST31_PTIME_80US;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(3, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
      });
  auto* latencyStrategy = static_cast<St30pRedundantLatency*>(latencyBundle.strategy);
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
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;
  uint64_t framesSend = latencyStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}
