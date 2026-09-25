/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Interlaced counterpart of st20p_user_pacing (st20p_user_pacing_tests.cpp).
 * MTL treats each field of an interlaced session as an individual frame (see
 * doc/design.md section 6.6): height stays the full frame height, fps is the
 * FIELD rate, and one st20p_tx_get_frame()/st20p_rx_get_frame() call carries
 * one field. pacing->frame_time (st_tx_video_session.c/st_rx_video_session.c)
 * is derived purely from ops.fps with no interlaced-specific halving, so with
 * fps set to the field rate it already represents one field period.
 *
 * Interlaced TX also pins the epoch parity to the ST 2110-21 6.2 frame grid. When
 * the first request lands on an odd field slot every field moves one slot later.
 * The strategy expects that shift in each field's RTP timestamp; it is uniform, so
 * elapsed time from field 0 and the RTP steps are unaffected.
 *
 * See README.md, "Test catalogue".
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

static_assert(kNoCtxPacingEarlyMaxNs == 1 * NS_PER_US, "comments quote -1 us");
static_assert(kNoCtxPacingLateMaxNs == 10 * NS_PER_US, "comments quote +10 us");
static_assert(kNoCtxPacingElapsedErrorMaxNs == 10 * NS_PER_US, "comments quote +-10 us");

/* st20p_user_pacing_interlaced
 * Config:    initStrictPacingContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            1080i50: kInterlaced, kFps = 50 fields/s (T = 20 ms), YUV 4:2:2 10-bit
 *            BPM, 3 buffers; TX kTxFlags = USER_PACING.
 * Plan:      t_user(n) = start + n*T per field, start = PTP now + 800 ms
 *            (kSt20pUserPacingLeadNs) rounded up to T.
 * Expect:    on every field, St20pInterlacedUserPacingOracle with expected TX =
 *            t_user(n) + TR_offset - VRX*trs, one field later if field 0 lands on
 *            an odd frame-grid slot
 *            0. interlaced, second_field == (n odd)   rxTestFrameModifier()
 *            1-6 as in st20p_user_pacing (launch -1/+10 us, kNoCtxPacingEarlyMaxNs,
 *               kNoCtxPacingLateMaxNs; elapsed +-10 us, kNoCtxPacingElapsedErrorMaxNs;
 *               RTP == tick90k(expected TX), step 1800)
 *            in the test, after mtl_start():
 *            7. getPacingParameters() == 0; TR_offset, trs, VRX > 0
 *            after stop:
 *            8. idx_tx > 0, idx_rx > 0, idx_tx == idx_rx
 *            Every TX and RX field also passes the St20pHandler thread checks.
 * Skip/Fail: as st20p_user_pacing (strict topology, NOCTX_REQUIRE_STRICT=1).
 */
TEST_F(NoCtxTest, st20p_user_pacing_interlaced) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  constexpr bool kInterlaced = true;
  constexpr enum st_fps kFps = ST_FPS_P50;
  constexpr uint32_t kTxFlags = ST20P_TX_FLAG_USER_PACING;

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pInterlacedUserPacingOracle(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.interlaced = kInterlaced;
        handler->sessionsOpsRx.interlaced = kInterlaced;
        handler->sessionsOpsTx.fps = kFps;
        handler->sessionsOpsRx.fps = kFps;
        handler->sessionsOpsTx.flags |= kTxFlags;
      });

  auto* frameTestStrategy = static_cast<St20pUserPacingOracle*>(bundle.strategy);

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
}
