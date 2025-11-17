/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

TEST_F(NoCtxTest, st20p_default_timestamps) {
  initSt20pDefaultContext();

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new FrameTestStrategy(handler); });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultTimestamp(handler); });
  auto* frameTestStrategy = static_cast<St20pDefaultTimestamp*>(rxBundle.strategy);

  ASSERT_TRUE(startRxThenTx(rxBundle, txBundle));

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);
  sleepUntilFailure();

  stopTxThenRx(txBundle, rxBundle);

  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
}

TEST_F(NoCtxTest, st20p_user_pacing) {
  initSt20pDefaultContext();

  auto handlerTxBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });

  auto handlerRxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); });

  auto* frameTestStrategyTx = static_cast<St20pUserTimestamp*>(handlerTxBundle.strategy);
  auto* frameTestStrategyRx = static_cast<St20pUserTimestamp*>(handlerRxBundle.strategy);

  ASSERT_TRUE(startRxThenTx(handlerRxBundle, handlerTxBundle));

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(frameTestStrategyTx->getPacingParameters(), 0);
  EXPECT_GT(frameTestStrategyTx->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(frameTestStrategyTx->pacing_trs_ns, 0.0);
  EXPECT_GT(frameTestStrategyTx->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  stopTxThenRx(handlerTxBundle, handlerRxBundle);

  ASSERT_GT(frameTestStrategyTx->idx_tx, 0u)
      << "st20p_user_pacing did not transmit any frames";
  ASSERT_GT(frameTestStrategyRx->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
  ASSERT_EQ(frameTestStrategyTx->idx_tx, frameTestStrategyRx->idx_rx)
      << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  initSt20pDefaultContext();

  std::vector<double> jitterMultipliers = {-0.00005, 0.0,      0.0000875, -0.00003,
                                           0.00012,  -0.00001, 0.0,       0.00006};

  auto txBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/false,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });
  auto rxBundle = createSt20pHandlerBundle(
      /*createTx=*/false, /*createRx=*/true, [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      });
  auto* txStrategy = static_cast<St20pUserTimestamp*>(txBundle.strategy);
  auto* rxStrategy = static_cast<St20pUserTimestamp*>(rxBundle.strategy);

  ASSERT_TRUE(startRxThenTx(rxBundle, txBundle));

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(txStrategy->getPacingParameters(), 0);
  EXPECT_GT(txStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  stopTxThenRx(txBundle, rxBundle);

  ASSERT_GE(txStrategy->idx_tx, jitterMultipliers.size())
      << "TX frames below expectation";
  ASSERT_GE(rxStrategy->idx_rx, jitterMultipliers.size())
      << "RX frames below expectation";
  ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx) << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_spike) {
  initSt20pDefaultContext();

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
      /*createTx=*/false, /*createRx=*/true, [burstMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, burstMultipliers);
      });
  auto* txStrategy = static_cast<St20pUserTimestamp*>(txBundle.strategy);
  auto* rxStrategy = static_cast<St20pUserTimestamp*>(rxBundle.strategy);

  ASSERT_TRUE(startRxThenTx(rxBundle, txBundle));

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(txStrategy->getPacingParameters(), 0);
  EXPECT_GT(txStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  stopTxThenRx(txBundle, rxBundle);

  ASSERT_GE(txStrategy->idx_tx, burstMultipliers.size()) << "TX frames below expectation";
  ASSERT_GE(rxStrategy->idx_rx, burstMultipliers.size()) << "RX frames below expectation";
  ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx) << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_clamp_to_zero) {
  initSt20pDefaultContext();

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
      /*createTx=*/false, /*createRx=*/true, [clampMultipliers](St20pHandler* handler) {
        return new St20pUserTimestampCustomStart(handler, clampMultipliers, 0);
      });
  auto* txStrategy = static_cast<St20pUserTimestampCustomStart*>(txBundle.strategy);
  auto* rxStrategy = static_cast<St20pUserTimestampCustomStart*>(rxBundle.strategy);

  ASSERT_TRUE(startRxThenTx(rxBundle, txBundle));

  TestPtpSourceSinceEpoch(nullptr);
  mtl_start(ctx->handle);

  ASSERT_EQ(txStrategy->getPacingParameters(), 0);
  EXPECT_GE(txStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(txStrategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  stopTxThenRx(txBundle, rxBundle);

  ASSERT_GT(txStrategy->idx_tx, 0u) << "No frames transmitted under clamp test";
  ASSERT_GT(rxStrategy->idx_rx, 0u) << "No frames received under clamp test";
  ASSERT_EQ(txStrategy->idx_tx, rxStrategy->idx_rx) << "TX/RX frame count mismatch";
}