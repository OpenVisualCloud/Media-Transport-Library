/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

class st30pDefaultTimestamp : public FrameTestStrategy {
 public:
  uint64_t lastTimestamp;

  st30pDefaultTimestamp(St30pHandler* parentHandler = nullptr) : lastTimestamp(0) {
    idx_tx = 0;
    idx_rx = 0;
    parent = parentHandler;
    enable_rx_modifier = true;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st30_frame* f = (st30_frame*)frame;
    St30pHandler* st30pParent = static_cast<St30pHandler*>(parent);
    uint64_t sampling = st30_get_sample_rate(st30pParent->sessionsOpsRx.sampling);
    uint64_t framebuffTime = st10_tai_to_media_clk(
        st30pParent->nsPacketTime, sampling);

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

class st30pUserTimestamp : public st30pDefaultTimestamp {
 protected:
  uint64_t startingTime = 10 * NS_PER_MS;
  uint64_t lastTimestamp = 0;

 public:
  st30pUserTimestamp(St30pHandler* parentHandler = nullptr)
      : st30pDefaultTimestamp(parentHandler) {
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

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pDefaultTimestamp* userData = new st30pDefaultTimestamp();
  St30pHandler* handler = new St30pHandler(ctx, userData);
  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

TEST_F(NoCtxTest, st30p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pUserTimestamp* userData = new st30pUserTimestamp();
  St30pHandler* handler = new St30pHandler(ctx);
  handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  handler->setModifiers(userData);
  handler->createSession(true);

  st30pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class st30pRedundantLatency : public st30pUserTimestamp {
  uint latencyInMs;

 public:
  st30pRedundantLatency(uint latency = 30, St30pHandler* parentHandler = nullptr)
      : st30pUserTimestamp(parentHandler), latencyInMs(latency) {
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
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);
  ASSERT_TRUE(ctx->para.num_ports >= 4)
      << " need at least 4 ports for redundant latency test";
  uint testedLatencyMs = 10;

  uint sessionRxSideId = 0;
  auto sessionUserData = new st30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st30pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  sessionUserData = new st30pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st30pHandlers.emplace_back(
      new St30pHandler(ctx, sessionUserData, {}, {}, 10, false, false));
  st30pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  st30pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st30pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  sessionUserData = new st30pRedundantLatency(testedLatencyMs);
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
