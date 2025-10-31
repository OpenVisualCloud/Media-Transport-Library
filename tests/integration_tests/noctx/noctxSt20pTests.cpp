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

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new FrameTestStrategy(handler); });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultTimestamp(handler); });
  auto* frameTestStrategy =
      static_cast<St20pDefaultTimestamp*>(rxBundle.strategy);

  rxBundle.handler->startSessionRx();
  sleep(2);
  txBundle.handler->startSessionTx();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);
  sleepUntilFailure();

  txBundle.handler->session.stop();
  sleep(2);
  rxBundle.handler->session.stop();

  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
}

class St20pUserTimestamp : public FrameTestStrategy {
 protected:
  double frameTimeNs = 0;
  uint64_t startingTime = 0;
  uint64_t lastTimestamp = 0;
  std::vector<double> timestampOffsetMultipliers;

 public:
  double pacing_tr_offset_ns = 0.0;
  double pacing_trs_ns = 0.0;
  uint32_t pacing_vrx_pkts = 0;

  explicit St20pUserTimestamp(St20pHandler* parentHandler,
                              std::vector<double> offsetMultipliers = {})
      : FrameTestStrategy(parentHandler, true, true),
        timestampOffsetMultipliers(offsetMultipliers) {
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
    f->timestamp = plannedTimestampNs(idx_tx);
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
  uint64_t plannedTimestampNs(uint64_t frame_idx) const {
    double candidate = plannedTimestampBaseNs(frame_idx);
    if (candidate <= 0.0) {
      return 0;
    }
    return candidate;
  }

  double plannedTimestampBaseNs(uint64_t frame_idx) const {
    double base = startingTime + frame_idx * frameTimeNs;
    double offset = frameTimeNs * offsetMultiplierForFrame(frame_idx);
    double adjusted = base + offset;
    return adjusted < 0.0 ? 0.0 : adjusted;
  }

  double offsetMultiplierForFrame(uint64_t frame_idx) const {
    if (timestampOffsetMultipliers.empty()) {
      return 0;
    }

    size_t loop_idx = frame_idx % timestampOffsetMultipliers.size();
    return timestampOffsetMultipliers[loop_idx];
  }

  uint64_t expectedTransmitTimeNs(uint64_t frame_idx) const {
    double target_ns = plannedTimestampBaseNs(frame_idx);
    double pacing_adjustment = pacing_tr_offset_ns -
                               pacing_vrx_pkts * pacing_trs_ns;
    double expected = target_ns + pacing_adjustment;
    if (expected <= 0.0) {
      return 0;
    }
    return expected;
  }

  void verifyReceiveTiming(uint64_t frame_idx, uint64_t receive_time_ns,
                           uint64_t expected_transmit_time_ns) const {
    const int64_t delta_ns = (receive_time_ns) - (expected_transmit_time_ns);
    int64_t expected_delta_ns = 10 * NS_PER_US;
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

    double current_target = plannedTimestampBaseNs(frame_idx);
    double previous_target = plannedTimestampBaseNs(frame_idx ? frame_idx - 1 : 0);
    double expected_step_ns = current_target - previous_target;
    if (expected_step_ns < 0.0) {
      expected_step_ns = 0.0;
    }

    uint64_t expected_step_input = expected_step_ns;
    const uint64_t expected_step =
        st10_tai_to_media_clk(expected_step_input, VIDEO_CLOCK_HZ);
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

  auto handlerTxBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) { handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING; });

  auto handlerRxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); });

  auto* frameTestStrategyTx =
      static_cast<St20pUserTimestamp*>(handlerTxBundle.strategy);
  auto* frameTestStrategyRx =
      static_cast<St20pUserTimestamp*>(handlerRxBundle.strategy);

  handlerRxBundle.handler->startSessionRx();  // Rx must be up first
  sleep(2);
  handlerTxBundle.handler->startSessionTx();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(frameTestStrategyTx->getPacingParameters(), 0);
  EXPECT_GT(frameTestStrategyTx->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(frameTestStrategyTx->pacing_trs_ns, 0.0);
  EXPECT_GT(frameTestStrategyTx->pacing_vrx_pkts, 0u);

  frameTestStrategyRx->pacing_tr_offset_ns = frameTestStrategyTx->pacing_tr_offset_ns;
  frameTestStrategyRx->pacing_trs_ns = frameTestStrategyTx->pacing_trs_ns;
  frameTestStrategyRx->pacing_vrx_pkts = frameTestStrategyTx->pacing_vrx_pkts;

  sleepUntilFailure();

  handlerTxBundle.handler->session.stop();
  sleep(2);
  handlerRxBundle.handler->session.stop();

  ASSERT_GT(frameTestStrategyTx->idx_tx, 0u)
    << "st20p_user_pacing did not transmit any frames";
  ASSERT_GT(frameTestStrategyRx->idx_rx, 0u)
    << "st20p_user_pacing did not receive any frames";
  ASSERT_EQ(frameTestStrategyTx->idx_tx, frameTestStrategyRx->idx_rx)
    << "TX/RX frame count mismatch";
}

class St20pUserTimestampCustomStart : public St20pUserTimestamp {
 public:
  St20pUserTimestampCustomStart(St20pHandler* parentHandler,
                                std::vector<double> offsetsNs,
                                uint64_t customStartingTimeNs)
      : St20pUserTimestamp(parentHandler, offsetsNs) {
    startingTime = customStartingTimeNs;
  }
};

TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  std::vector<double> jitterMultipliers = {-0.00005, 0.0, 0.0000875, -0.00003,
                                           0.00012,  -0.00001, 0.0,      0.00006};

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      },
      [](St20pHandler* handler) { handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING; });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      });
  auto* txStrategy = static_cast<St20pUserTimestamp*>(txBundle.strategy);
  auto* rxStrategy = static_cast<St20pUserTimestamp*>(rxBundle.strategy);

  rxBundle.handler->startSessionRx();
  sleep(2);
  txBundle.handler->startSessionTx();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(txStrategy->getPacingParameters(), 0);
  EXPECT_GT(txStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

  rxStrategy->pacing_tr_offset_ns = txStrategy->pacing_tr_offset_ns;
  rxStrategy->pacing_trs_ns = txStrategy->pacing_trs_ns;
  rxStrategy->pacing_vrx_pkts = txStrategy->pacing_vrx_pkts;

  const int sleepDuration = defaultTestDuration > 1 ? defaultTestDuration / 2 : 1;
  sleepUntilFailure(sleepDuration);

    txBundle.handler->session.stop();
    sleep(2);
    rxBundle.handler->session.stop();

    ASSERT_GE(txStrategy->idx_tx, jitterMultipliers.size())
      << "TX frames below expectation";
    ASSERT_GE(rxStrategy->idx_rx, jitterMultipliers.size())
      << "RX frames below expectation";
    ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx)
      << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_spike) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  std::vector<double> burstMultipliers = {0.0, 0.2, 0.4, 0.6, 0.8, 0.0, 0.0, 0.0};

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [burstMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, burstMultipliers);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
      });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [burstMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, burstMultipliers);
      });
  auto* txStrategy = static_cast<St20pUserTimestamp*>(txBundle.strategy);
  auto* rxStrategy = static_cast<St20pUserTimestamp*>(rxBundle.strategy);

  rxBundle.handler->startSessionRx();
  sleep(2);
  txBundle.handler->startSessionTx();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

    ASSERT_EQ(txStrategy->getPacingParameters(), 0);
    EXPECT_GT(txStrategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
    EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

    rxStrategy->pacing_tr_offset_ns = txStrategy->pacing_tr_offset_ns;
    rxStrategy->pacing_trs_ns = txStrategy->pacing_trs_ns;
    rxStrategy->pacing_vrx_pkts = txStrategy->pacing_vrx_pkts;

  sleepUntilFailure(defaultTestDuration);

    txBundle.handler->session.stop();
    sleep(2);
    rxBundle.handler->session.stop();

    ASSERT_GE(txStrategy->idx_tx, burstMultipliers.size())
      << "TX frames below expectation";
    ASSERT_GE(rxStrategy->idx_rx, burstMultipliers.size())
      << "RX frames below expectation";
    ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx)
      << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_clamp_to_zero) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  std::vector<double> clampMultipliers = {-1.0, -0.5, 0.0, 0.25, 0.75, 0.0, -0.2};

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [clampMultipliers](St20pHandler* handler) {
        return new St20pUserTimestampCustomStart(handler, clampMultipliers, 0);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_TIMESTAMP;
      });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [clampMultipliers](St20pHandler* handler) {
        return new St20pUserTimestampCustomStart(handler, clampMultipliers, 0);
      });
  auto* txStrategy =
      static_cast<St20pUserTimestampCustomStart*>(txBundle.strategy);
  auto* rxStrategy =
      static_cast<St20pUserTimestampCustomStart*>(rxBundle.strategy);

  rxBundle.handler->startSessionRx();
  sleep(2);
  txBundle.handler->startSessionTx();

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(txStrategy->getPacingParameters(), 0);
  EXPECT_GE(txStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

  rxStrategy->pacing_tr_offset_ns = txStrategy->pacing_tr_offset_ns;
  rxStrategy->pacing_trs_ns = txStrategy->pacing_trs_ns;
  rxStrategy->pacing_vrx_pkts = txStrategy->pacing_vrx_pkts;

  const int sleepDuration = defaultTestDuration > 1 ? defaultTestDuration / 2 : 1;
  sleepUntilFailure(sleepDuration);

    txBundle.handler->session.stop();
    sleep(2);
    rxBundle.handler->session.stop();

    ASSERT_GT(txStrategy->idx_tx, 0u)
      << "No frames transmitted under clamp test";
    ASSERT_GT(rxStrategy->idx_rx, 0u)
      << "No frames received under clamp test";
    ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx)
      << "TX/RX frame count mismatch";
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

  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pRedundantLatency(0, handler); },
      [this](St20pHandler* handler) {
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
  sleep(2);
  primaryBundle.handler->startSessionTx();
  latencyBundle.handler->startSessionTx();

  /* we are creating 3 handlers
    - sessionRx handler will receive both primary and redundant streams
    - Primary handler will send the primary stream (no modifications)
    - Latency handler will send the redundant rx stream modified to simulate
      latency (100ms added to timestamp)

    [sessionTxPrimarySideId]:          Tx ---> Rx [sessionTxRedundantLatencySideId]
    [sessionTxRedundantLatencySideId]: Tx ---> Rx [sessionTxRedundantLatencySideId]
  */

  TestPtpSourceSinceEpoch(nullptr);  // reset ptp time to 0
  mtl_start(ctx->handle);
  sleepUntilFailure(30);

  mtl_stop(ctx->handle);

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

  primaryBundle.handler->session.stop();
  latencyBundle.handler->session.stop();
  sleep(2);
  rxBundle.handler->session.stop();
}
