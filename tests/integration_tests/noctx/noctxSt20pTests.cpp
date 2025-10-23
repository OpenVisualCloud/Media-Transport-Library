/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

class St20pDefaultTimestamp : public FrameTestStrategy {
 public:
  uint64_t lastTimestamp;

  St20pDefaultTimestamp(St20pHandler* parentHandler = nullptr) : lastTimestamp(0) {
    idx_tx = 0;
    idx_rx = 0;
    parent = parentHandler;
    enable_rx_modifier = true;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st_frame* f = (st_frame*)frame;  // Changed from st30_frame to st_frame
    St20pHandler* st20pParent = static_cast<St20pHandler*>(parent);
    uint64_t framebuffTime = st10_tai_to_media_clk(st20pParent->nsFrameTime, VIDEO_CLOCK_HZ);
    uint64_t diff;

    EXPECT_NEAR(f->timestamp,
                framebuffTime * (idx_rx + 1),
                framebuffTime / 20)
        << " idx_rx: " << idx_rx;

    if (lastTimestamp != 0) {
      diff = f->timestamp - lastTimestamp;
      EXPECT_TRUE(diff == framebuffTime) << " idx_rx: " << idx_rx << " diff: " << diff;
    }

    lastTimestamp = f->timestamp;
    idx_rx++;
  }
};

TEST_F(NoCtxTest, st20p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  St20pDefaultTimestamp* userData = new St20pDefaultTimestamp();
  St20pHandler* handler = new St20pHandler(ctx, userData);

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);
  st20pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class St20pUserTimestamp : public St20pDefaultTimestamp {
 protected:
  uint64_t startingTime = 20 * NS_PER_MS;
  uint64_t lastTimestamp = 0;

 public:
  St20pUserTimestamp(St20pHandler* parentHandler = nullptr)
      : St20pDefaultTimestamp(parentHandler) {
    enable_tx_modifier = true;
    enable_rx_modifier = true;
  }

  void txTestFrameModifier(void* frame, size_t frame_size) {
    st_frame* f = (st_frame*)frame;
    St20pHandler* st20pParent = static_cast<St20pHandler*>(parent);

    f->tfmt = ST10_TIMESTAMP_FMT_TAI;
    f->timestamp = startingTime + (st20pParent->nsFrameTime * (idx_tx));
    idx_tx++;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st_frame* f = (st_frame*)frame;
    St20pHandler* st20pParent = static_cast<St20pHandler*>(parent);
    idx_rx++;

    uint64_t expectedTimestamp =
        startingTime + (st20pParent->nsFrameTime * (idx_rx - 1));
    uint64_t expected_media_clk = st10_tai_to_media_clk(expectedTimestamp, VIDEO_CLOCK_HZ);

    EXPECT_EQ(f->timestamp, expected_media_clk)
        << " idx_rx: " << idx_rx << " tai difference: "
        << (int64_t)(st10_media_clk_to_ns(f->timestamp, VIDEO_CLOCK_HZ) - expectedTimestamp);

    if (lastTimestamp != 0) {
      uint64_t diff = f->timestamp - lastTimestamp;
      EXPECT_TRUE(diff == st10_tai_to_media_clk(st20pParent->nsFrameTime, VIDEO_CLOCK_HZ))
          << " idx_rx " << idx_rx << " diff: " << diff;
    }

    lastTimestamp = f->timestamp;
  }
};

TEST_F(NoCtxTest, st20p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  St20pUserTimestamp* userData = new St20pUserTimestamp();
  St20pHandler* handler = new St20pHandler(ctx);
  handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;  // Changed from ST30P to ST20P
  handler->setModifiers(userData);
  handler->createSession(true);

  st20pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class St20pRedundantLatency : public St20pUserTimestamp {
  uint latencyInMs;

 public:
  St20pRedundantLatency(uint latency = 30, St20pHandler* parentHandler = nullptr)
      : St20pUserTimestamp(parentHandler), latencyInMs(latency) {
    enable_tx_modifier = true;
    enable_rx_modifier = true;

    this->startingTime = (50 + latencyInMs) * NS_PER_MS;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    idx_rx++;
  }
};

TEST_F(NoCtxTest, st20p_redundant_latency) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  if (ctx->para.num_ports < 4) {
    throw std::runtime_error("st20p_redundant_latency test ctx needs at least 4 ports");
  }

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  uint testedLatencyMs = 10;

  uint sessionRxSideId = 0;
  auto sessionUserData = new St20pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st20pHandlers.emplace_back(
      new St20pHandler(ctx, sessionUserData, {}, {}, false, false));

  st20pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
  st20pHandlers[sessionRxSideId]->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
  st20pHandlers[sessionRxSideId]->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT,
                                                  1);
  st20pHandlers[sessionRxSideId]->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  sessionUserData = new St20pRedundantLatency(0);
  sessionUserDatas.emplace_back(sessionUserData);
  st20pHandlers.emplace_back(
      new St20pHandler(ctx, sessionUserData, {}, {}, false, false));

  st20pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
  st20pHandlers[sessionTxPrimarySideId]->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
  st20pHandlers[sessionTxPrimarySideId]->setSessionPorts(
      2, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  st20pHandlers[sessionTxPrimarySideId]->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  sessionUserData = new St20pRedundantLatency(testedLatencyMs);
  sessionUserDatas.emplace_back(sessionUserData);
  st20pHandlers.emplace_back(
      new St20pHandler(ctx, sessionUserData, {}, {}, false, false));
  st20pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.flags |=
      ST20P_TX_FLAG_USER_PACING;

  /* the later we want to send the stream the more we need to shift the timestamps */
  st20pHandlers[sessionTxRedundantLatencySideId]->sessionsOpsTx.rtp_timestamp_delta_us =
      -1 * (testedLatencyMs * 1000);

  /* even though this session is sending Redundant stream we are setting primary port why
  ? The session has no idea it's actually redundant we are simulating a late session */
  st20pHandlers[sessionTxRedundantLatencySideId]->setSessionPorts(
      3, SESSION_SKIP_PORT, SESSION_SKIP_PORT, SESSION_SKIP_PORT);
  memcpy(st20pHandlers[sessionTxRedundantLatencySideId]
             ->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
         ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  st20pHandlers[sessionTxRedundantLatencySideId]
      ->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
  st20pHandlers[sessionTxRedundantLatencySideId]->createSessionTx();

  /* we are creating 3 handlers
    - sessionRx handler will receive both primary and redundant streams
    - Primary handler will send the primary stream (no modifications)
    - Latency handler will send the redundant rx stream modified to simulate
      latency (100ms added to timestamp)

    [sessionTxPrimarySideId]:          Tx ---> Rx [sessionTxRedundantLatencySideId]
    [sessionTxRedundantLatencySideId]: Tx ---> Rx [sessionTxRedundantLatencySideId]
  */

  st20pHandlers[sessionRxSideId]->startSessionRx();
  st20pHandlers[sessionTxPrimarySideId]->startSessionTx();
  st20pHandlers[sessionTxRedundantLatencySideId]->startSessionTx();
  TestPtpSourceSinceEpoch(nullptr);  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure(30);

  mtl_stop(ctx->handle);

  st20_rx_user_stats stats;
  st20p_rx_get_session_stats(st20pHandlers[sessionRxSideId]->sessionsHandleRx, &stats);
  st20_tx_user_stats statsTxPrimary;
  st20p_tx_get_session_stats(st20pHandlers[sessionTxPrimarySideId]->sessionsHandleTx,
                             &statsTxPrimary);
  st20_tx_user_stats statsTxRedundant;
  st20p_tx_get_session_stats(
      st20pHandlers[sessionTxRedundantLatencySideId]->sessionsHandleTx,
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
