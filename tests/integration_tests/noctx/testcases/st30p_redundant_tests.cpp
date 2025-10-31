/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st30p_handler.hpp"
#include "strategies/st30p_strategies.hpp"

TEST_F(NoCtxTest, st30p_redundant_latency) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags |= MTL_FLAG_DEV_AUTO_START_STOP;

  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  uint testedLatencyMs = 10;

  uint sessionRxSideId = 0;
  auto rxStrategy = std::make_unique<St30pRedundantLatency>(0);
  auto* rxStrategyRaw = rxStrategy.get();
  frameTestStrategies.emplace_back(std::move(rxStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, rxStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  rxStrategyRaw->initializeTiming(st30pHandlers[sessionRxSideId].get());
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st30pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  auto primaryStrategy = std::make_unique<St30pRedundantLatency>(0);
  auto* primaryStrategyRaw = primaryStrategy.get();
  frameTestStrategies.emplace_back(std::move(primaryStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, primaryStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  primaryStrategyRaw->initializeTiming(st30pHandlers[sessionTxPrimarySideId].get());
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st30pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  auto redundantStrategy = std::make_unique<St30pRedundantLatency>(testedLatencyMs);
  auto* redundantStrategyRaw = redundantStrategy.get();
  frameTestStrategies.emplace_back(std::move(redundantStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, redundantStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  redundantStrategyRaw->initializeTiming(
      st30pHandlers[sessionTxRedundantLatencySideId].get());
  st30pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.flags |=
      ST30P_TX_FLAG_USER_PACING;
  /* the later we want to send the stream the more we need to shift the timestamps */
  st30pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.rtp_timestamp_delta_us =
      -1 * (testedLatencyMs * 1000);
  /* even though this session is sending Redundant stream we are setting primary port why
  ? The session has no idea it's actually redundant we are simulating a late session */
  st30pHandlers[sessionTxRedundantLatencySideId]->setSessionPorts(
      3, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  memcpy(st30pHandlers[sessionTxRedundantLatencySideId]
             ->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
         ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  st30pHandlers[sessionTxRedundantLatencySideId]
      ->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
  st30pHandlers[sessionTxRedundantLatencySideId]->createSessionTx();

  /* we are creating 3 handlers
    - sessionRx handler will receive both primary and redundant streams
    - Primary handler will send the primary stream (no modifications)
    - Latency handler will send the redundant rx stream modified to simulate
      latency (100ms added to timestamp)

    [sessionTxPrimarySideId]:          Tx ---> Rx [sessionTxRedundantLatencySideId]
    [sessionTxRedundantLatencySideId]: Tx ---> Rx [sessionTxRedundantLatencySideId]
  */

  st30pHandlers[sessionRxSideId]->startSessionRx();
  st30pHandlers[sessionTxPrimarySideId]->startSessionTx();
  st30pHandlers[sessionTxRedundantLatencySideId]->startSessionTx();
  TestPtpSourceSinceEpoch(nullptr);  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure();

  mtl_stop(ctx->handle);

  st30_rx_user_stats stats;
  st30p_rx_get_session_stats(st30pHandlers[sessionRxSideId]->sessionsHandleRx, &stats);
  st30_tx_user_stats statsTxPrimary;
  st30p_tx_get_session_stats(st30pHandlers[sessionTxPrimarySideId]->sessionsHandleTx,
                             &statsTxPrimary);
  st30_tx_user_stats statsTxRedundant;
  st30p_tx_get_session_stats(
      st30pHandlers[sessionTxRedundantLatencySideId]->sessionsHandleTx,
      &statsTxRedundant);

  uint64_t packetsSend = statsTxPrimary.common.port[0].packets;
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;
  uint64_t framesSend = frameTestStrategies[sessionTxPrimarySideId]->idx_tx;
  uint64_t framesRecieved = frameTestStrategies[sessionRxSideId]->idx_rx;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}

TEST_F(NoCtxTest, st30p_redundant_latency2) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags |= MTL_FLAG_DEV_AUTO_START_STOP;

  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st30p_redundant_latency test ctx needs at least 4 ports");
  }

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  uint testedLatencyMs = 10;

  uint sessionRxSideId = 0;
  auto rxStrategy = std::make_unique<St30pRedundantLatency>(0);
  auto* rxStrategyRaw = rxStrategy.get();
  frameTestStrategies.emplace_back(std::move(rxStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, rxStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  rxStrategyRaw->initializeTiming(st30pHandlers[sessionRxSideId].get());
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.ptime = ST31_PTIME_80US;
  st30pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st30pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  auto primaryStrategy = std::make_unique<St30pRedundantLatency>(0);
  auto* primaryStrategyRaw = primaryStrategy.get();
  frameTestStrategies.emplace_back(std::move(primaryStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, primaryStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  primaryStrategyRaw->initializeTiming(st30pHandlers[sessionTxPrimarySideId].get());
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.ptime = ST31_PTIME_80US;
  st30pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st30pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  auto redundantStrategy = std::make_unique<St30pRedundantLatency>(testedLatencyMs);
  auto* redundantStrategyRaw = redundantStrategy.get();
  frameTestStrategies.emplace_back(std::move(redundantStrategy));
  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(
      ctx, redundantStrategyRaw, st30p_tx_ops{}, st30p_rx_ops{}, 10, false, false));
  redundantStrategyRaw->initializeTiming(
      st30pHandlers[sessionTxRedundantLatencySideId].get());
  st30pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.flags |=
      ST30P_TX_FLAG_USER_PACING;
  /* the later we want to send the stream the more we need to shift the timestamps */
  st30pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.rtp_timestamp_delta_us =
      -1 * (testedLatencyMs * 1000);
  /* even though this session is sending Redundant stream we are setting primary port why
  ? The session has no idea it's actually redundant we are simulating a late session */
  st30pHandlers[sessionTxRedundantLatencySideId]->setSessionPorts(
      3, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  memcpy(st30pHandlers[sessionTxRedundantLatencySideId]
             ->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
         ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  st30pHandlers[sessionTxRedundantLatencySideId]
      ->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
  st30pHandlers[sessionTxRedundantLatencySideId]->createSessionTx();

  st30pHandlers[sessionRxSideId]->sessionsOpsTx.ptime = ST31_PTIME_80US;

  /* we are creating 3 handlers
    - sessionRx handler will receive both primary and redundant streams
    - Primary handler will send the primary stream (no modifications)
    - Latency handler will send the redundant rx stream modified to simulate
      latency (100ms added to timestamp)

    [sessionTxPrimarySideId]:          Tx ---> Rx [sessionTxRedundantLatencySideId]
    [sessionTxRedundantLatencySideId]: Tx ---> Rx [sessionTxRedundantLatencySideId]
  */

  st30pHandlers[sessionRxSideId]->startSessionRx();
  st30pHandlers[sessionTxPrimarySideId]->startSessionTx();
  st30pHandlers[sessionTxRedundantLatencySideId]->startSessionTx();
  TestPtpSourceSinceEpoch(nullptr);  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure(10);

  st30pHandlers[sessionTxPrimarySideId]->stopSession();
  sleepUntilFailure(20);

  mtl_stop(ctx->handle);

  st30_rx_user_stats stats;
  st30p_rx_get_session_stats(st30pHandlers[sessionRxSideId]->sessionsHandleRx, &stats);
  st30_tx_user_stats statsTxRedundant;
  st30p_tx_get_session_stats(
      st30pHandlers[sessionTxRedundantLatencySideId]->sessionsHandleTx,
      &statsTxRedundant);

  uint64_t packetsSend = statsTxRedundant.common.port[0].packets;
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;
  uint64_t framesSend = frameTestStrategies[sessionTxRedundantLatencySideId]->idx_tx;
  uint64_t framesRecieved = frameTestStrategies[sessionRxSideId]->idx_rx;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}
