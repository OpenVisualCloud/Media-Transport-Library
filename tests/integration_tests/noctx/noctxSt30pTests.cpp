/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

class St30pDefaultTimestamp : public FrameTestStrategy {
 public:
  uint64_t lastTimestamp;

  St30pDefaultTimestamp(St30pHandler* parentHandler = nullptr) : lastTimestamp(0) {
    idx_tx = 0;
    idx_rx = 0;
    parent = parentHandler;
    enable_rx_modifier = true;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st30_frame* f = (st30_frame*)frame;
    St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);
    uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
    uint64_t framebuffTime = st10_tai_to_media_clk(st30pParent->nsPacketTime, sampling);

    EXPECT_NEAR(f->timestamp,
                st10_tai_to_media_clk((idx_rx)*st30pParent->nsPacketTime, sampling),
                framebuffTime)
        << " idx_rx: " << idx_rx;
    if (lastTimestamp != 0) {
      uint64_t diff = f->timestamp - lastTimestamp;
      EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
    }

    lastTimestamp = f->timestamp;
    idx_rx++;
  }
};

class St30pUserTimestamp : public St30pDefaultTimestamp {
 protected:
  uint64_t startingTime = 10 * NS_PER_MS;
  uint64_t lastTimestamp = 0;

 public:
  St30pUserTimestamp(St30pHandler* parentHandler = nullptr)
      : St30pDefaultTimestamp(parentHandler) {
    enable_tx_modifier = true;
    enable_rx_modifier = true;
  }

  void txTestFrameModifier(void* frame, size_t frame_size) {
    st30_frame* f = (st30_frame*)frame;
    St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);

    f->tfmt = ST10_TIMESTAMP_FMT_TAI;
    f->timestamp = startingTime + (st30pParent->nsPacketTime * (idx_tx));
    idx_tx++;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st30_frame* f = (st30_frame*)frame;
    St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);
    uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
    idx_rx++;

    uint64_t expectedTimestamp =
        startingTime + (st30pParent->nsPacketTime * (idx_rx - 1));
    uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, sampling);

    EXPECT_EQ(f->timestamp, expected_media_clk)
        << " idx_rx: " << idx_rx << " tai difference: "
        << (int64_t)(st10_media_clk_to_ns(f->timestamp, sampling) - expectedTimestamp);

    if (lastTimestamp != 0) {
      uint64_t diff = f->timestamp - lastTimestamp;
      EXPECT_TRUE(diff == st10_tai_to_media_clk(st30pParent->nsPacketTime, sampling))
          << " idx_rx " << idx_rx << " diff: " << diff;
    }

    lastTimestamp = f->timestamp;
  }
};

TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  St30pDefaultTimestamp* userData = new St30pDefaultTimestamp();
  St30pHandler* handler = new St30pHandler(ctx, userData);
  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

TEST_F(NoCtxTest, st30p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  St30pUserTimestamp* userData = new St30pUserTimestamp();
  St30pHandler* handler = new St30pHandler(ctx);
  handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  handler->setModifiers(userData);
  handler->createSession(true);

  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class St30pRedundantLatency : public St30pUserTimestamp {
  uint latencyInMs;
  uint startingTimeInMs;

 public:
  St30pRedundantLatency(uint latency = 30, St30pHandler* parentHandler = nullptr, int startingTime = 100)
      : St30pUserTimestamp(parentHandler), latencyInMs(latency), startingTimeInMs(startingTime) {
    enable_tx_modifier = true;
    enable_rx_modifier = true;

    this->startingTime = (50 + latencyInMs) * NS_PER_MS;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    idx_rx++;
  }
};

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
  auto sessionUserData = new St30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st30pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  sessionUserData = new St30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st30pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  sessionUserData = new St30pRedundantLatency(testedLatencyMs);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
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
  uint64_t framesSend = sessionUserDatas[sessionTxPrimarySideId]->idx_tx;
  uint64_t framesRecieved = sessionUserDatas[sessionRxSideId]->idx_rx;

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
  auto sessionUserData = new St30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.ptime = ST31_PTIME_80US;
  st30pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st30pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  sessionUserData = new St30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.ptime = ST31_PTIME_80US;
  st30pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st30pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  sessionUserData = new St30pRedundantLatency(testedLatencyMs);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
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
  st30p_tx_get_session_stats(st30pHandlers[sessionTxRedundantLatencySideId]->sessionsHandleTx,
                             &statsTxRedundant);

  uint64_t packetsSend = statsTxRedundant.common.port[0].packets;
  uint64_t packetsRecieved = stats.common.port[0].packets + stats.common.port[1].packets;
  uint64_t framesSend = sessionUserDatas[sessionTxRedundantLatencySideId]->idx_tx;
  uint64_t framesRecieved = sessionUserDatas[sessionRxSideId]->idx_rx;

  ASSERT_NEAR(packetsSend, packetsRecieved, packetsSend / 100)
      << "Comparison against primary stream";
  ASSERT_LE(stats.common.stat_pkts_out_of_order, packetsRecieved / 1000)
      << "Out of order packets";
  ASSERT_NEAR(framesSend, framesRecieved, framesSend / 100)
      << "Comparison against primary stream";
}
