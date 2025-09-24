/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "noctx.hpp"

TEST_F(NoCtxTest, st30p_user_pacing) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(ctx));
  auto& handler = st30pHandlers[0];

  handler->sessionsOpsTx.flags |= ST30P_TX_FLAG_USER_PACING;
  handler->txTestFrameModifier = std::bind(&St30pHandler::txSt30DefaultUserPacing, handler.get(), std::placeholders::_1, std::placeholders::_2);
  handler->rxTestFrameModifier = std::bind(&St30pHandler::rxSt30DefaultUserPacingCheck, handler.get(), std::placeholders::_1, std::placeholders::_2);
  handler->createSession();
  sleepUntilFailure();
}

TEST_F(NoCtxTest, st30p_default_timestamps) {
  ctx->para.ptp_get_time_fn = NoCtxTest::TestPtpSourceSinceEpoch;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;

  ASSERT_TRUE(ctx && ctx->handle == nullptr);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  st30pHandlers.emplace_back(std::make_unique<St30pHandler>(ctx));
  auto& handler = st30pHandlers[0];
  handler->rxTestFrameModifier = std::bind(&St30pHandler::rxSt30DefaultTimestampsCheck, handler.get(), std::placeholders::_1, std::placeholders::_2);
  handler->createSession();
  sleepUntilFailure();
}

