/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

TEST_F(NoCtxTest, st20p_default_timestamps) {
  initDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultTimestamp(handler); });
  auto* frameTestStrategy = static_cast<St20pDefaultTimestamp*>(bundle.strategy);

  StartFakePtpClock();
  bundle.handler->startSession();
  mtl_start(ctx->handle);

  sleepUntilFailure();
  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
}

TEST_F(NoCtxTest, st20p_user_pacing) {
  initDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });

  auto* frameTestStrategy = static_cast<St20pUserTimestamp*>(bundle.strategy);

  StartFakePtpClock();
  bundle.handler->startSession();
  mtl_start(ctx->handle);

  ASSERT_EQ(frameTestStrategy->getPacingParameters(), 0);
  EXPECT_GT(frameTestStrategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(frameTestStrategy->pacing_trs_ns, 0.0);
  EXPECT_GT(frameTestStrategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_tx, 0u)
      << "st20p_user_pacing did not transmit any frames";
  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
  ASSERT_EQ(frameTestStrategy->idx_tx, frameTestStrategy->idx_rx)
      << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  initDefaultContext();

  /* everything that does not cross the half-frame boundary should be snapped to correct
   * epochs */
  std::vector<double> jitterMultipliers = {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44};
  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserTimestamp(handler, jitterMultipliers);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });
  auto* strategy = static_cast<St20pUserTimestamp*>(bundle.strategy);

  StartFakePtpClock();
  bundle.handler->startSession();
  mtl_start(ctx->handle);

  ASSERT_EQ(strategy->getPacingParameters(), 0);
  EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
  EXPECT_GT(strategy->pacing_trs_ns, 0.0);
  EXPECT_GT(strategy->pacing_vrx_pkts, 0u);

  sleepUntilFailure();

  bundle.handler->stopSession();

  ASSERT_GE(strategy->idx_tx, jitterMultipliers.size()) << "TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, jitterMultipliers.size()) << "RX frames below expectation";
  ASSERT_EQ(strategy->idx_tx, strategy->idx_rx) << "TX/RX frame count mismatch";
}

TEST_F(NoCtxTest, st20p_exact_user_pacing) {
  initDefaultContext();

  /* Offset values must remain smaller than in standard user pacing, since exact mode
     lacks epoch snapping and only minimal timing slack exists between consecutive frames.
     ~(tr_offset - processing time) */
  std::vector<double> exactOffsets = {0.002,   0.007,  -0.002,  0.008,
                                      -0.0005, 0.0033, -0.0025, 0.0051};

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exactOffsets](St20pHandler* handler) {
        return new St20pExactUserPacing(handler, exactOffsets);
      },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_EXACT_USER_PACING;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St20pExactUserPacing*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();

  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);

  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }

  mtl_start(ctx->handle);

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GE(handler->txFrames(), exactOffsets.size())
      << "st20p_exact_user_pacing transmitted too few frames for offset coverage";
  ASSERT_GE(handler->rxFrames(), exactOffsets.size())
      << "st20p_exact_user_pacing received too few frames for offset coverage";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st20p_exact_user_pacing TX/RX frame count mismatch";
  ASSERT_GE(strategy->idx_tx, exactOffsets.size())
      << "st20p_exact_user_pacing strategy TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, exactOffsets.size())
      << "st20p_exact_user_pacing strategy RX frames below expectation";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st20p_exact_user_pacing strategy TX/RX mismatch";
}
