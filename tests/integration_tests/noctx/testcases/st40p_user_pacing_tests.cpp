/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright(c) 2025 Intel Corporation */

#include <cerrno>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st40p_handler.hpp"
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
 *            in the test, after mtl_start():
 *            4. getPacingParameters() == 0 with TR_offset, trs, VRX > 0, or -ENOTSUP
 *               expectPacingQueryResult()
 *            Every TX and RX frame also passes the St40pHandler thread checks.
 * Skip/Fail: none.
 */

static_assert(kSt40pMaxLateNs == 1 * NS_PER_MS, "comments quote +1 ms");
static_assert(kSt40pUserPacingStartFrames == 70.0, "comments quote 70 frames");

namespace {
/* Check 4; EXPECT only, so the test goes on either way. */
void expectPacingQueryResult(St40pUserPacingOracle* strategy) {
  const int pacing_status = strategy->getPacingParameters();
  if (pacing_status == 0) {
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  } else {
    EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
  }
}
}  // namespace

/* st40p_user_pacing
 * Config: TX kTxFlags = USER_PACING.
 * Plan:   no offsets; each request is on an epoch, expected TX = t_user(n).
 * Expect: 1-4 above, RTP step 1500; after stop:
 *         5. txFrames() > 0, rxFrames() > 0, equal; idx_tx == idx_rx
 */
TEST_F(NoCtxTest, st40p_user_pacing) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_USER_PACING;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler* handler) { return new St40pUserPacingOracle(handler); },
      [](St40pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  expectPacingQueryResult(strategy);

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "st40p_user_pacing did not transmit any frames";
  ASSERT_GT(handler->rxFrames(), 0u) << "st40p_user_pacing did not receive any frames";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_user_pacing TX/RX frame count mismatch";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_user_pacing strategy TX/RX mismatch";
}

/* st40p_user_pacing_59fps
 * Config: TX kTxFlags = USER_PACING, kFps = 59.94p.
 * Plan:   no offsets; expected TX = epoch nearest t_user(n).
 * Expect: 1-4 above, RTP steps alternating 1501/1502; after stop:
 *         5. txFrames() > 0, rxFrames() > 0, equal; idx_tx == idx_rx
 */
TEST_F(NoCtxTest, st40p_user_pacing_59fps) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_USER_PACING;
  constexpr enum st_fps kFps = ST_FPS_P59_94;

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St40pHandler* handler) { return new St40pUserPacingOracle(handler); },
      [](St40pHandler* handler) {
        handler->sessionsOpsTx.flags |= kTxFlags;
        handler->sessionsOpsTx.fps = kFps;
      });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  expectPacingQueryResult(strategy);

  sleepUntilFailure();

  handler->stopSession();

  ASSERT_GT(handler->txFrames(), 0u) << "st40p_user_pacing did not transmit any frames";
  ASSERT_GT(handler->rxFrames(), 0u) << "st40p_user_pacing did not receive any frames";
  EXPECT_EQ(handler->txFrames(), handler->rxFrames())
      << "st40p_user_pacing TX/RX frame count mismatch";
  EXPECT_EQ(strategy->idx_tx, strategy->idx_rx)
      << "st40p_user_pacing strategy TX/RX mismatch";
}

/* st40p_user_pacing_offset_jitter
 * Config: TX kTxFlags = USER_PACING.
 * Plan:   offsets jitterMultipliers, all inside +-T/2; expected TX = epoch nearest
 *         t_user(n) = (70 + n) * T.
 * Expect: 1-4 above, RTP step 1500; after stop:
 *         5. txFrames() and rxFrames() >= 8 and equal; idx_tx and idx_rx >= 8 and equal
 */
TEST_F(NoCtxTest, st40p_user_pacing_offset_jitter) {
  initDefaultContext();

  constexpr uint32_t kTxFlags = ST40P_TX_FLAG_USER_PACING;
  /* everything that does not cross the half-frame boundary should be snapped to correct
   * epochs */
  std::vector<double> jitterMultipliers = {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44};

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [jitterMultipliers](St40pHandler* handler) {
        return new St40pUserPacingOracle(handler, jitterMultipliers);
      },
      [](St40pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pUserPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  expectPacingQueryResult(strategy);

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

/* st40p_exact_user_pacing
 * Config: TX kTxFlags = USER_PACING | EXACT_USER_PACING.
 * Plan:   offsets exactOffsets (-0.25 .. +0.8 T), not snapped.
 * Expect: St40pExactUserPacingOracle, expected TX = t_user(n): 1, 2 and 4 above,
 *         no step check; after stop:
 *         5. txFrames() and rxFrames() >= 8 and equal; idx_tx and idx_rx >= 8 and equal
 */
TEST_F(NoCtxTest, st40p_exact_user_pacing) {
  initDefaultContext();

  constexpr uint32_t kTxFlags =
      ST40P_TX_FLAG_USER_PACING | ST40P_TX_FLAG_EXACT_USER_PACING;
  /* Ancillary frame transmission time is minimal relative to the inter-frame interval at
  60 fps, allowing large offsets while maintaining successful transmission. */
  std::vector<double> exactOffsets = {0.2, 0.7, -0.1, 0.8, -0.05, 0.33, -0.25, 0.51};

  auto bundle = createSt40pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exactOffsets](St40pHandler* handler) {
        return new St40pExactUserPacingOracle(handler, exactOffsets);
      },
      [](St40pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St40pExactUserPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

  StartFakePtpClock();
  handler->startSession();
  mtl_start(ctx->handle);

  expectPacingQueryResult(strategy);

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
