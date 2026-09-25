/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Strict ST20p pacing: default, user (nearest epoch) and exact user pacing,
 * measured with NIC RX timestamps against each planned packet-0 launch (-1/+10 us)
 * and as elapsed time from frame 0 within +-10 us, plus exact RTP values.
 * See README.md, "Timing model" and "Test catalogue".
 *
 * Common to every test here:
 * Config:    initStrictPacingContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            1080p25 (T = 40 ms) YUV 4:2:2 10-bit BPM, 3 buffers (fillSt20Ops()).
 * Expect:    every TX and RX frame also passes the St20pHandler thread checks.
 * Skip/Fail: TX and RX on one physical port, no RX PHC or no NIC RX timestamp on
 *            frame 0: SKIP, FAIL with NOCTX_REQUIRE_STRICT=1. A software RX time
 *            after frame 0: FAIL (RxPhcClock).
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

static_assert(kNoCtxPacingEarlyMaxNs == 1 * NS_PER_US, "comments quote -1 us");
static_assert(kNoCtxPacingLateMaxNs == 10 * NS_PER_US, "comments quote +10 us");
static_assert(kNoCtxPacingElapsedErrorMaxNs == 10 * NS_PER_US, "comments quote +-10 us");
static_assert(kSt20pUserPacingLeadNs == 800 * NS_PER_MS, "comments quote 800 ms");

/* st20p_default_timestamps
 * Config: default pacing, no TX flags.
 * Plan:   none; each frame goes out on its epoch.
 * Expect: on every frame, St20pDefaultPacingOracle
 *         1. COMPLETE, BPM packet count            expectCompletePrimaryFrame()
 *         2. timestamp == RTP header               expectRtpTimestamp()
 *         3. frame 0 RTP on k*T + TR_offset - VRX*trs  expectRtpOnEpoch()
 *         4. RTP step 3600                         rxTestFrameModifier()
 *         5. packet 0 at frame 0 + n*T, -1/+10 us  expectPacingLaunch(),
 *            kNoCtxPacingEarlyMaxNs, kNoCtxPacingLateMaxNs
 *         6. elapsed n*T +-10 us                   expectPacingElapsed(),
 *            kNoCtxPacingElapsedErrorMaxNs
 *         after stop, in the test:
 *         7. idx_rx > 0
 */
TEST_F(NoCtxTest, st20p_default_timestamps) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultPacingOracle(handler); });
  auto* frameTestStrategy = static_cast<St20pDefaultPacingOracle*>(bundle.strategy);

  bundle.handler->startSession();
  mtl_start(ctx->handle);

  sleepUntilFailure();
  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_default_timestamps did not receive any frames";
}

/* st20p_user_pacing
 * Config: TX kTxFlags = USER_PACING.
 * Plan:   t_user(n) = start + n*T, start = PTP now + 800 ms (kSt20pUserPacingLeadNs)
 *         rounded up to T; each request is on an epoch.
 * Expect: on every frame, St20pUserPacingOracle with expected TX = t_user(n) +
 *         TR_offset - VRX*trs
 *         1. COMPLETE, BPM packet count            expectCompletePrimaryFrame()
 *         2. packet 0 at expected TX, -1/+10 us    verifyReceiveTiming(),
 *            kNoCtxPacingEarlyMaxNs, kNoCtxPacingLateMaxNs
 *         3. elapsed from frame 0 +-10 us          verifyReceiveTiming(),
 *            kNoCtxPacingElapsedErrorMaxNs
 *         4. timestamp == RTP header               expectRtpTimestamp()
 *         5. RTP == tick90k(expected TX)           verifyMediaClock()
 *         6. RTP step 3600                         verifyTimestampStep()
 *         in the test, after mtl_start():
 *         7. getPacingParameters() == 0; TR_offset, trs, VRX > 0
 *         after stop:
 *         8. idx_tx > 0, idx_rx > 0, idx_tx == idx_rx
 */
TEST_F(NoCtxTest, st20p_user_pacing) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  constexpr uint32_t kTxFlags = ST20P_TX_FLAG_USER_PACING;
  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserPacingOracle(handler); },
      [](St20pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });

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
      << "st20p_user_pacing did not transmit any frames";
  ASSERT_GT(frameTestStrategy->idx_rx, 0u)
      << "st20p_user_pacing did not receive any frames";
  ASSERT_EQ(frameTestStrategy->idx_tx, frameTestStrategy->idx_rx)
      << "TX/RX frame count mismatch";
}

/* st20p_user_pacing_offset_jitter
 * Config: TX kTxFlags = USER_PACING.
 * Plan:   t_user(n) = start + (n + jitterMultipliers[n % 8])*T, start as in
 *         st20p_user_pacing; every offset is inside +-T/2, so each request snaps
 *         back to epoch start + n*T.
 * Expect: on every frame, checks 1-6 of st20p_user_pacing, with expected TX = (epoch
 *         nearest t_user(n)) + TR_offset - VRX*trs
 *         in the test, after mtl_start():
 *         7. getPacingParameters() == 0; TR_offset, trs, VRX > 0
 *         after stop:
 *         8. idx_tx >= 8, idx_rx >= 8, idx_tx == idx_rx
 */
TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  constexpr uint32_t kTxFlags = ST20P_TX_FLAG_USER_PACING;
  /* everything that does not cross the half-frame boundary should be snapped to correct
   * epochs */
  std::vector<double> jitterMultipliers = {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44};
  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [jitterMultipliers](St20pHandler* handler) {
        return new St20pUserPacingOracle(handler, jitterMultipliers);
      },
      [](St20pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });
  auto* strategy = static_cast<St20pUserPacingOracle*>(bundle.strategy);

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

/* st20p_exact_user_pacing
 * Config: TX kTxFlags = USER_PACING | EXACT_USER_PACING.
 * Plan:   t_user(n) = start + (n + exactOffsets[n % 8])*T, start as in
 *         st20p_user_pacing; offsets -100 us .. +320 us, not snapped.
 * Expect: on every frame, St20pExactUserPacingOracle with expected TX = t_user(n)
 *         1-4 as in st20p_user_pacing (launch -1/+10 us, kNoCtxPacingEarlyMaxNs,
 *            kNoCtxPacingLateMaxNs; elapsed +-10 us, kNoCtxPacingElapsedErrorMaxNs)
 *         5. RTP == tick90k(t_user(n))             verifyMediaClock()
 *         in the test, before mtl_start():
 *         7. getPacingParameters() == 0 with TR_offset, trs, VRX > 0, or -ENOTSUP
 *         after stop:
 *         8. txFrames() and rxFrames() >= 8 and equal; idx_tx and idx_rx >= 8 and equal
 */
TEST_F(NoCtxTest, st20p_exact_user_pacing) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;

  constexpr uint32_t kTxFlags =
      ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_EXACT_USER_PACING;

  /* Offset values must remain smaller than in standard user pacing, since exact mode
     lacks epoch snapping and only minimal timing slack exists between consecutive frames.
     ~(tr_offset - processing time) */
  std::vector<double> exactOffsets = {0.002,   0.007,  -0.002,  0.008,
                                      -0.0005, 0.0033, -0.0025, 0.0051};

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exactOffsets](St20pHandler* handler) {
        return new St20pExactUserPacingOracle(handler, exactOffsets);
      },
      [](St20pHandler* handler) { handler->sessionsOpsTx.flags |= kTxFlags; });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St20pExactUserPacingOracle*>(bundle.strategy);
  ASSERT_NE(handler, nullptr);
  ASSERT_NE(strategy, nullptr);

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
