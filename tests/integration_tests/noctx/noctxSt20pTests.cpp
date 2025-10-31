/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

class St20pDefaultTimestamp : public FrameTestStrategy {
 protected:
  uint64_t lastTimestamp = 0;

 public:
  St20pDefaultTimestamp(St20pHandler* parentHandler = nullptr)
      : FrameTestStrategy(parentHandler, false, true) {
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) {
    st_frame* f = (st_frame*)frame;  // Changed from st30_frame to st_frame
    St20pHandler* st20pParent = static_cast<St20pHandler*>(parent);
    uint64_t framebuffTime =
        st10_tai_to_media_clk(st20pParent->nsFrameTime, VIDEO_CLOCK_HZ);
    uint64_t diff;

    EXPECT_NEAR(f->timestamp, framebuffTime * (idx_rx + 1), framebuffTime / 20)
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

  St20pHandler* handler = new St20pHandler(ctx);
  St20pDefaultTimestamp* userData = new St20pDefaultTimestamp(handler);
  handler->setModifiers(userData);
  handler->createSession(true);

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);
  st20pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class St20pUserTimestamp : public FrameTestStrategy {
 protected:
  double frameTimeNs = 0;
  uint64_t startingTime = 0;
  uint64_t lastTimestamp = 0;

 public:
  double pacing_tr_offset_ns = 0.0;
  double pacing_trs_ns = 0.0;
  uint32_t pacing_vrx_pkts = 0;

  explicit St20pUserTimestamp(St20pHandler* parentHandler)
      : FrameTestStrategy(parentHandler, true, true) {
    initializeTiming(parentHandler);
  }

  int getPacingParameters() {
    St20pHandler* parentHandler = static_cast<St20pHandler*>(parent);
    if (parentHandler && parentHandler->sessionsHandleTx) {
      int ret = st20p_tx_get_pacing_params(parentHandler->sessionsHandleTx,
                                           &pacing_tr_offset_ns, &pacing_trs_ns,
                                           &pacing_vrx_pkts);
      return ret;
    }
    return -1;
  }

  void txTestFrameModifier(void* frame, size_t frame_size) {
    st_frame* f = static_cast<st_frame*>(frame);

    f->tfmt = ST10_TIMESTAMP_FMT_TAI;
    f->timestamp = startingTime + (frameTimeNs * idx_tx);
    idx_tx++;
  }

  void rxTestFrameModifier(void* frame, size_t frame_size) override {
    st_frame* f = static_cast<st_frame*>(frame);
    const uint64_t frame_idx = idx_rx++;

    const uint64_t expected_transmit_time_ns = expectedTransmitTimeNs(frame_idx);
    const uint64_t expected_media_clk =
        st10_tai_to_media_clk(expected_transmit_time_ns, VIDEO_CLOCK_HZ);

    verifyReceiveTiming(frame_idx, f->receive_timestamp, expected_transmit_time_ns);
    verifyMediaClock(frame_idx, f->timestamp, expected_media_clk);
    verifyTimestampStep(frame_idx, f->timestamp);

    lastTimestamp = f->timestamp;
  }

 private:
  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const {
    const double base = (startingTime) + (frame_idx) * (frameTimeNs);
    const double pacing_adjustment =
        (pacing_tr_offset_ns) - (pacing_vrx_pkts) * (pacing_trs_ns);
    return base + pacing_adjustment;
  }

  void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                           uint64_t expected_transmit_time_ns) const {
    const int64_t delta_ns = (receive_time_ns) - (expected_transmit_time_ns);
    int64_t expected_delta_ns = 5 * NS_PER_US;
    if (frame_idx == 0) {
      expected_delta_ns = 20 * NS_PER_US;
    }

    EXPECT_LE(delta_ns, expected_delta_ns)
        << " idx_rx: " << frame_idx << " delta(ns): " << delta_ns
        << " receive timestamp(ns): " << receive_time_ns
        << " expected timestamp(ns): " << expected_transmit_time_ns;
  }

  void verifyMediaClock(uint64_t frame_idx, uint64_t timestamp_media_clk,
                        uint64_t expected_media_clk) const {
    EXPECT_EQ(timestamp_media_clk, expected_media_clk)
        << " idx_rx: " << frame_idx << "expected media clk: " << expected_media_clk
        << " received timestamp: " << timestamp_media_clk;
  }

  void verifyTimestampStep(uint64_t frame_idx, uint64_t current_timestamp) {
    if (!lastTimestamp) {
      return;
    }

    const uint64_t expected_step = st10_tai_to_media_clk(frameTimeNs, VIDEO_CLOCK_HZ);
    const uint64_t diff = current_timestamp - lastTimestamp;
    EXPECT_EQ(diff, expected_step) << " idx_rx: " << frame_idx << " diff: " << diff;
  }

 protected:
  void initializeTiming(St20pHandler* handler) {
    if (!handler) {
      throw std::invalid_argument("St20pUserTimestamp expects a valid handler");
    }

    frameTimeNs = handler->nsFrameTime;

    if (!frameTimeNs) {
      double framerate = st_frame_rate(handler->sessionsOpsTx.fps);
      if (framerate > 0.0) {
        long double frame_time = (long double)NS_PER_S / framerate;
        frameTimeNs = (uint64_t)(frame_time + 0.5L);
      }
    }

    if (!frameTimeNs) {
      frameTimeNs = NS_PER_S / 25;
    }

    startingTime = frameTimeNs * 20;
  }
};

TEST_F(NoCtxTest, st20p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  St20pHandler* handler = new St20pHandler(ctx);
  St20pUserTimestamp* userData = new St20pUserTimestamp(handler);
  handler->sessionsOpsTx.flags |=
      ST20P_TX_FLAG_USER_PACING;  // Changed from ST30P to ST20P
  handler->setModifiers(userData);
  handler->createSession(true);
  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  userData->getPacingParameters();

  EXPECT_GT(userData->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(userData->pacing_trs_ns, 0.0);
  EXPECT_GT(userData->pacing_vrx_pkts, 0u);

  st20pHandlers.emplace_back(handler);
  sessionUserDatas.emplace_back(userData);
  sleepUntilFailure();
}

class St20pRedundantLatency : public St20pUserTimestamp {
  uint latencyInMs;

 public:
  St20pRedundantLatency(uint latency, St20pHandler* parentHandler)
      : St20pUserTimestamp(parentHandler), latencyInMs(latency) {
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
  auto handlerRx = new St20pHandler(ctx);
  auto sessionUserData = new St20pRedundantLatency(0, handlerRx);
  sessionUserDatas.emplace_back(sessionUserData);
  handlerRx->setModifiers(sessionUserData);
  st20pHandlers.emplace_back(handlerRx);

  handlerRx->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
  handlerRx->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
  handlerRx->setSessionPorts(SESSION_SKIP_PORT, 0, SESSION_SKIP_PORT, 1);
  handlerRx->createSessionRx();

  uint sessionTxPrimarySideId = 1;
  auto handlerPrimary = new St20pHandler(ctx);
  sessionUserData = new St20pRedundantLatency(0, handlerPrimary);
  sessionUserDatas.emplace_back(sessionUserData);
  handlerPrimary->setModifiers(sessionUserData);
  st20pHandlers.emplace_back(handlerPrimary);

  handlerPrimary->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
  handlerPrimary->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
  handlerPrimary->setSessionPorts(2, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                  SESSION_SKIP_PORT);
  handlerPrimary->createSessionTx();

  uint sessionTxRedundantLatencySideId = 2;
  auto handlerLatency = new St20pHandler(ctx);
  sessionUserData = new St20pRedundantLatency(testedLatencyMs, handlerLatency);
  sessionUserDatas.emplace_back(sessionUserData);
  handlerLatency->setModifiers(sessionUserData);
  st20pHandlers.emplace_back(handlerLatency);
  handlerLatency->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;

  /* the later we want to send the stream the more we need to shift the timestamps */
  handlerLatency->sessionsOpsTx.rtp_timestamp_delta_us = -1 * (testedLatencyMs * 1000);

  /* even though this session is sending Redundant stream we are setting primary port why
  ? The session has no idea it's actually redundant we are simulating a late session */
  handlerLatency->setSessionPorts(3, SESSION_SKIP_PORT, SESSION_SKIP_PORT,
                                  SESSION_SKIP_PORT);
  memcpy(handlerLatency->sessionsOpsTx.port.dip_addr[MTL_SESSION_PORT_P],
         ctx->mcast_ip_addr[MTL_PORT_R], MTL_IP_ADDR_LEN);

  handlerLatency->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P]++;
  handlerLatency->createSessionTx();

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
