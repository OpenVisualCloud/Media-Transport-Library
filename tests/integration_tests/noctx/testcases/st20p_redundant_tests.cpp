/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

TEST_F(NoCtxTest, st20p_redundant_latency) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st20p_redundant_latency test ctx needs at least 4 ports");
  }

  initDefaultContext();

  uint testedLatencyMs = 10;

  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pRedundantLatency(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
        handler->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT, 1);
      });
  auto* rxStrategy = static_cast<St20pRedundantLatency*>(rxBundle.strategy);

  auto primaryBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new St20pRedundantLatency(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
        handler->setSessionPorts(2, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto* primaryStrategy = static_cast<St20pRedundantLatency*>(primaryBundle.strategy);

  auto latencyBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [testedLatencyMs](St20pHandler* handler) {
        return new St20pRedundantLatency(testedLatencyMs, handler);
      },
      [this, testedLatencyMs](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(3, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
      });

  /* we are creating 3 handlers
    - rxBundle receives both primary and redundant streams
    - primaryBundle sends the primary stream (no modifications)
    - latencyBundle sends the redundant stream delayed by testedLatencyMs

    [primaryBundle]:          Tx ---> Rx [latencyBundle]
    [latencyBundle]:          Tx ---> Rx [latencyBundle]
  */

  rxBundle.handler->startSessionRx();
  ASSERT_TRUE(waitForSession(rxBundle.handler->session));
  primaryBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(primaryBundle.handler->session));
  latencyBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));

  StartFakePtpClock();  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure(30);

  latencyBundle.handler->session.stop();
  primaryBundle.handler->session.stop();
  rxBundle.handler->session.stop();

  st20_rx_user_stats stats;
  st20p_rx_get_session_stats(rxBundle.handler->sessionsHandleRx, &stats);
  st20_tx_user_stats statsTxPrimary;
  st20p_tx_get_session_stats(primaryBundle.handler->sessionsHandleTx, &statsTxPrimary);
  st20_tx_user_stats statsTxRedundant;
  st20p_tx_get_session_stats(latencyBundle.handler->sessionsHandleTx, &statsTxRedundant);

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

/**
 * This test requires MTL_SIMULATE_PAKET_DROPS to be enabled in the build.
 * so DEBUG mode is necessary for proper functionality.
 * The packet skip feature affects the critical path performance,
 */
#ifdef MTL_SIMULATE_PAKET_DROPS

#define TX_SESSION_PORT_0 0
#define TX_SESSION_PORT_1 1
#define RX_SESSION_PORT_0 2
#define RX_SESSION_PORT_1 3

TEST_F(NoCtxTest, st20p_redundant_latency_drops_even_odd) {
  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st20p_redundant_latency test ctx needs at least 4 ports");
  }

  ctx->para.flags |= MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS;
  ctx->para.port_packet_loss[TX_SESSION_PORT_0].tx_stream_loss_id =
      0; /* drop even packets */
  ctx->para.port_packet_loss[TX_SESSION_PORT_0].tx_stream_loss_divider =
      2; /* out of every 2 packets */
  ctx->para.port_packet_loss[TX_SESSION_PORT_1].tx_stream_loss_id =
      1; /* drop odd packets */
  ctx->para.port_packet_loss[TX_SESSION_PORT_1].tx_stream_loss_divider =
      2; /* out of every 2 packets */
  initDefaultContext();

  /* Class a */
  uint testedLatencyMs = 10;

  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pRedundantLatency(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = 20000;
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = 20001;
        handler->setSessionPorts(SESSION_SKIP_PORT, RX_SESSION_PORT_0, SESSION_SKIP_PORT,
                                 RX_SESSION_PORT_1);
      });
  auto* rxStrategy = static_cast<St20pRedundantLatency*>(rxBundle.strategy);

  auto primaryBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new St20pRedundantLatency(0, handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = 20000;
        handler->setSessionPorts(TX_SESSION_PORT_0, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
      });
  auto* primaryStrategy = static_cast<St20pRedundantLatency*>(primaryBundle.strategy);

  auto latencyBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [testedLatencyMs](St20pHandler* handler) {
        return new St20pRedundantLatency(testedLatencyMs, handler);
      },
      [this, testedLatencyMs](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = 20001;
        handler->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);
        handler->setSessionPorts(TX_SESSION_PORT_1, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                 SESSION_SKIP_PORT);
        memcpy(handler->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
               ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);
      });

  /* we are creating 3 handlers
    - rxBundle receives both primary and redundant streams
    - primaryBundle sends the primary stream (no modifications)
    - latencyBundle sends the redundant stream delayed by testedLatencyMs

    [primaryBundle]:          Tx ---> Rx [latencyBundle]
    [latencyBundle]:          Tx ---> Rx [latencyBundle]
  */

  rxBundle.handler->startSessionRx();
  ASSERT_TRUE(waitForSession(rxBundle.handler->session));
  primaryBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(primaryBundle.handler->session));
  latencyBundle.handler->startSessionTx();
  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));
  StartFakePtpClock(); /* reset ptp time to 0 */
  mtl_start(ctx->handle);

  ASSERT_TRUE(waitForSession(latencyBundle.handler->session));
  sleepUntilFailure(600);

  latencyBundle.handler->session.stop();
  primaryBundle.handler->session.stop();
  rxBundle.handler->session.stop();

  st20_rx_user_stats stats;
  st20p_rx_get_session_stats(rxBundle.handler->sessionsHandleRx, &stats);
  st20_tx_user_stats statsTxPrimary;
  st20p_tx_get_session_stats(primaryBundle.handler->sessionsHandleTx, &statsTxPrimary);
  st20_tx_user_stats statsTxRedundant;
  st20p_tx_get_session_stats(latencyBundle.handler->sessionsHandleTx, &statsTxRedundant);

  uint64_t framesSend = primaryStrategy->idx_tx;
  uint64_t framesRecieved = rxStrategy->idx_rx;

  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}
#endif
