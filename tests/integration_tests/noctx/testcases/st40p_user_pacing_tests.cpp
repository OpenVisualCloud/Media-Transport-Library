/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#include <cerrno>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st40p_handler.hpp"
#include "strategies/st40p_strategies.hpp"

TEST_F(NoCtxTest, st40p_user_pacing) {
  initDefaultContext();

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler* handler) { return new St40pUserTimestamp(handler); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST40P_TX_FLAG_USER_PACING;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "st40p_user_pacing did not transmit any frames";
  ASSERT_GT(handler->rxFrames(), 0u) << "st40p_user_pacing did not receive any frames";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_user_pacing TX/RX frame count mismatch";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_user_pacing strategy TX/RX mismatch";
}

TEST_F(NoCtxTest, st40p_user_pacing_59fps) {
  initDefaultContext();

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler* handler) { return new St40pUserTimestamp(handler); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST40P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.fps = ST_FPS_P59_94;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "st40p_user_pacing did not transmit any frames";
  ASSERT_GT(handler->rxFrames(), 0u) << "st40p_user_pacing did not receive any frames";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_user_pacing TX/RX frame count mismatch";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_user_pacing strategy TX/RX mismatch";
}

TEST_F(NoCtxTest, st40p_user_pacing_offset_jitter) {
  initDefaultContext();

  /* everything that does not cross the half-frame boundary should be snapped to correct
   * epochs */
  std::vector<double> jitterMultipliers = {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44};

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [jitterMultipliers](St40pHandler* handler) {
        return new St40pUserTimestamp(handler, jitterMultipliers);
      },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST40P_TX_FLAG_USER_PACING;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GE(handler->txFrames(), jitterMultipliers.size())
      << "st40p_user_pacing_offset_jitter TX frames below expectation";
  ASSERT_GE(handler->rxFrames(), jitterMultipliers.size())
      << "st40p_user_pacing_offset_jitter RX frames below expectation";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_user_pacing_offset_jitter TX/RX mismatch";
  ASSERT_GE(strategy->idx_tx, jitterMultipliers.size())
      << "st40p_user_pacing_offset_jitter strategy TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, jitterMultipliers.size())
      << "st40p_user_pacing_offset_jitter strategy RX frames below expectation";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_user_pacing_offset_jitter strategy TX/RX mismatch";
}

TEST_F(NoCtxTest, st40p_exact_user_pacing) {
  initDefaultContext();

  /* Ancillary frame transmission time is minimal relative to the inter-frame interval at
  60 fps, allowing large offsets while maintaining successful transmission. */
  std::vector<double> exactOffsets = {0.2, 0.7, -0.1, 0.8, -0.05, 0.33, -0.25, 0.51};

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exactOffsets](St40pHandler* handler) {
        return new St40pExactUserPacing(handler, exactOffsets);
      },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST40P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.flags |= ST40P_TX_FLAG_EXACT_USER_PACING;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pExactUserPacing*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GE(handler->txFrames(), exactOffsets.size())
      << "st40p_exact_user_pacing transmitted too few frames for offset coverage";
  ASSERT_GE(handler->rxFrames(), exactOffsets.size())
      << "st40p_exact_user_pacing received too few frames for offset coverage";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_exact_user_pacing TX/RX frame count mismatch";
  ASSERT_GE(strategy->idx_tx, exactOffsets.size())
      << "st40p_exact_user_pacing strategy TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, exactOffsets.size())
      << "st40p_exact_user_pacing strategy RX frames below expectation";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_exact_user_pacing strategy TX/RX mismatch";
}
