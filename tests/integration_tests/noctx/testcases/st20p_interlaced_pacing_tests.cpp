/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Interlaced counterpart of st20p_user_pacing (st20p_user_pacing_tests.cpp).
 * MTL treats each field of an interlaced session as an individual frame (see
 * doc/design.md section 6.6): height stays the full frame height, fps is the
 * FIELD rate, and one st20p_tx_get_frame()/st20p_rx_get_frame() call carries
 * one field. pacing->frame_time (st_tx_video_session.c/st_rx_video_session.c)
 * is derived purely from ops.fps with no interlaced-specific halving, so with
 * fps set to the field rate it already represents one field period -- the
 * same St20pUserTimestamp math used for progressive sessions applies to
 * fields unmodified; no new strategy class is needed.
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

TEST_F(NoCtxTest, st20p_user_pacing_interlaced) {
  initDefaultContext();

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.interlaced = true;
        handler->sessionsOpsRx.interlaced = true;
        handler->sessionsOpsTx.fps = ST_FPS_P50;
        handler->sessionsOpsRx.fps = ST_FPS_P50;
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
      << "st20p_user_pacing_interlaced did not transmit any fields";
  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing_interlaced did not receive any fields";
  ASSERT_EQ(frameTestStrategy->idx_tx, frameTestStrategy->idx_rx)
      << "TX/RX field count mismatch";
  frameTestStrategy->assertTimingWithinBudget();
  frameTestStrategy->assertRlLatencyWithinBounds();
}
