/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#include <algorithm>
#include <vector>

#include "core/test_fixture.hpp"
#include "strategies/st40p_strategies.hpp"

/* Common to every test here:
 * Config:    initDefaultContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            60p (T = 16.67 ms) unless noted, one 255-byte ANC packet per frame,
 *            4 buffers, TX and RX BLOCK_GET (fillSt40pOps()).
 * Plan:      St40pUserPacingOracle, t_user(n) = (70 + n + offset[n % size]) * T
 *            from PTP zero (kSt40pUserPacingStartFrames; StartFakePtpClock() before
 *            the sessions start).
 * Expect:    on every frame, St40pUserPacingOracle
 *            1. software RX time - expected TX in [0, +1 ms]  verifyReceiveTiming(),
 *               kSt40pMaxLateNs
 *            2. RTP == tick90k(expected TX)                   verifyMediaClock()
 *            3. RTP step from the planned grid                verifyTimestampStep()
 *            Every TX and RX frame also passes the St40pHandler thread checks.
 * Skip/Fail: none.
 */

static_assert(kSt40pMaxLateNs == 1 * NS_PER_MS, "comments quote +1 ms");
static_assert(kSt40pUserPacingStartFrames == 70.0, "comments quote 70 frames");

namespace {

void runSt40pPacingCase(NoCtxTest* test, st_tests_context* ctx, const char* name,
                        enum st_fps fps, bool exact, std::vector<double> offsets = {}) {
  test->initDefaultContext();
  if (::testing::Test::HasFatalFailure()) return;

  uint32_t tx_flags = ST40P_TX_FLAG_USER_PACING;
  if (exact) tx_flags |= ST40P_TX_FLAG_EXACT_USER_PACING;
  const size_t min_frames = std::max<size_t>(1, offsets.size());

  auto bundle = test->createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exact, offsets](St40pHandler* handler) -> FrameTestStrategy* {
        if (exact) return new St40pExactUserPacingOracle(handler, offsets);
        return new St40pUserPacingOracle(handler, offsets);
      },
      [tx_flags, fps](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= tx_flags;
        handler->sessionsOpsTx.fps = fps;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserPacingOracle*>(bundle.strategy);

  NoCtxTest::StartFakePtpClock();
  handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  test->sleepUntilFailure();
  handler->stopSession();

  ASSERT_GE(handler->txFrames(), min_frames) << name << " TX frames below expectation";
  ASSERT_GE(handler->rxFrames(), min_frames) << name << " RX frames below expectation";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << name << " TX/RX frame count mismatch";
  ASSERT_GE(strategy->idx_tx, min_frames)
      << name << " strategy TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, min_frames)
      << name << " strategy RX frames below expectation";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx) << name << " strategy TX/RX mismatch";
}

}  // namespace

/* st40p_user_pacing: on-epoch requests at 60p (RTP step 1500). */
TEST_F(NoCtxTest, st40p_user_pacing) {
  runSt40pPacingCase(this, ctx, "st40p_user_pacing", ST_FPS_P60, /*exact=*/false);
}

/* st40p_user_pacing_59fps: on-epoch requests at 59.94p (RTP steps 1501/1502). */
TEST_F(NoCtxTest, st40p_user_pacing_59fps) {
  runSt40pPacingCase(this, ctx, "st40p_user_pacing_59fps", ST_FPS_P59_94,
                     /*exact=*/false);
}

/* st40p_user_pacing_offset_jitter: offsets inside +-T/2 snap back to each epoch. */
TEST_F(NoCtxTest, st40p_user_pacing_offset_jitter) {
  runSt40pPacingCase(this, ctx, "st40p_user_pacing_offset_jitter", ST_FPS_P60,
                     /*exact=*/false, {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44});
}

/* st40p_exact_user_pacing: unsnapped offsets (-0.25..+0.8 T), no step check. */
TEST_F(NoCtxTest, st40p_exact_user_pacing) {
  runSt40pPacingCase(this, ctx, "st40p_exact_user_pacing", ST_FPS_P60, /*exact=*/true,
                     {0.2, 0.7, -0.1, 0.8, -0.05, 0.33, -0.25, 0.51});
}
