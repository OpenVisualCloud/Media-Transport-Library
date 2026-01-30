/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st30p_handler.hpp"
#include "strategies/st30p_strategies.hpp"

TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pDefaultTimestamp(handler); });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pDefaultTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  handler->startSession();
  sleepUntilFailure();
  handler->stopSession();
}

TEST_F(NoCtxTest, st30p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::FakePtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  auto bundle = createSt30pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St30pHandler* handler) { return new St30pUserTimestamp(handler); },
      [](St30pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
      });
  auto* handler = bundle.handler;
  auto* strategy = static_cast<St30pUserTimestamp*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  strategy->initializeTiming(handler);
  sleep(1);

  StartFakePtpClock();
  mtl_start(ctx->handle);
  handler->startSession();

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(strategy->idx_tx, 0u) << "st30p_user_pacing did not transmit any frames";
  ASSERT_GT(strategy->idx_rx, 0u) << "st30p_user_pacing did not receive any frames";
  ASSERT_EQ(strategy->idx_tx, strategy->idx_rx) << "TX/RX frame count mismatch";
}
