/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

/* Strict ST20p pacing: default, user (nearest epoch) and exact user pacing,
 * measured with NIC RX timestamps against each packet-0 launch the library aims at
 * (0/+10 us) and against the instant its RTP timestamp names, and as elapsed time from
 * frame 0 within +-10 us, plus exact RTP values.
 * See README.md, "Timing model" and "Test catalogue".
 *
 * Common to every test here:
 * Config:    initStrictPacingContext(); one session TX TEST_PORT_1 -> RX TEST_PORT_2,
 *            1080p25 (T = 40 ms) YUV 4:2:2 10-bit BPM, 3 buffers (fillSt20Ops()),
 *            except where a test names another rate or resolution.
 * Expect:    every TX and RX frame also passes the St20pHandler thread checks.
 * Skip/Fail: TX and RX on one physical port, no RX PHC or no NIC RX timestamp on
 *            frame 0: SKIP, FAIL with NOCTX_REQUIRE_STRICT=1. A software RX time
 *            after frame 0: FAIL (RxPhcClock).
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

static_assert(kNoCtxPacingEarlyMaxNs == 0, "comments quote 0 us early");
static_assert(kNoCtxPacingLateMaxNs == 10 * NS_PER_US, "comments quote +10 us");
static_assert(kNoCtxPacingElapsedErrorMaxNs == 10 * NS_PER_US, "comments quote +-10 us");
static_assert(kSt20pUserPacingLeadNs == 800 * NS_PER_MS, "comments quote 800 ms");

/* st20p_default_timestamps
 * Config: default pacing, no TX flags.
 * Plan:   none; frame n goes out on epoch k0 + n, k0 the epoch of frame 0's RTP.
 * Expect: on every frame, St20pDefaultPacingOracle with expected launch = the 90 kHz
 *         tick of epoch + TR_offset - VRX*trs (st20pRoundedLaunchNs())
 *         1. COMPLETE, BPM packet count            expectCompletePrimaryFrame()
 *         2. timestamp == RTP header               expectRtpTimestamp()
 *         3. RTP == tick90k(expected launch)       expectRtpOnLaunch(), so RTP steps
 *            3600 at 25p, 1500 at 60p, 1501/1502 at 59.94p
 *         4. packet 0 at the expected launch, 0/+10 us   expectPacingLaunch(),
 *            kNoCtxPacingEarlyMaxNs, kNoCtxPacingLateMaxNs
 *         5. packet 0 at the instant its RTP names, 0/+10 us   expectNearRtpTimestamp()
 *         6. elapsed from frame 0 as the expected launches, +-10 us
 *            expectPacingElapsed(), kNoCtxPacingElapsedErrorMaxNs
 *         after stop, in the test:
 *         7. idx_rx > 0
 */
static void runDefaultTimestamps(NoCtxTest* test, st_tests_context* ctx, enum st_fps fps,
                                 const char* name, uint width = 1920,
                                 uint height = 1080) {
  auto bundle = test->createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pDefaultPacingOracle(handler); },
      [fps, width, height](St20pHandler* handler) {
        handler->sessionsOpsTx.fps = fps;
        handler->sessionsOpsRx.fps = fps;
        handler->sessionsOpsTx.width = width;
        handler->sessionsOpsRx.width = width;
        handler->sessionsOpsTx.height = height;
        handler->sessionsOpsRx.height = height;
      });
  auto* frameTestStrategy = static_cast<St20pDefaultPacingOracle*>(bundle.strategy);

  bundle.handler->startSession();
  mtl_start(ctx->handle);

  test->sleepUntilFailure();
  bundle.handler->stopSession();

  ASSERT_GT(frameTestStrategy->idx_rx, 0u) << name << " did not receive any frames";
}

TEST_F(NoCtxTest, st20p_default_timestamps) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P25, "st20p_default_timestamps");
}

/* st20p_default_timestamps_1080p59_94
 * As st20p_default_timestamps at 1080p59.94 (T = 1001/60 ms): the 90 kHz snap of the
 * launch moves with the format, and the epochs alternate 1501/1502 ticks. */
TEST_F(NoCtxTest, st20p_default_timestamps_1080p59_94) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_1080p59_94");
}

/* st20p_default_timestamps_1080p60
 * As st20p_default_timestamps at 1080p60 (T = 16.67 ms), whose snap moves the launch
 * later than the unsnapped epoch, not earlier as at 25p. */
TEST_F(NoCtxTest, st20p_default_timestamps_1080p60) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P60, "st20p_default_timestamps_1080p60");
}

/* st20p_default_timestamps_480p59_94, _576p50
 * As st20p_default_timestamps in SD (~54 MB/s), where an RL port warms packet 0 up with
 * 8 pads, fewer than the last pre-arm burst needs to trim a pad, and its shaper adds
 * credit in ~4 us timeslots: packet 0 still never early. */
TEST_F(NoCtxTest, st20p_default_timestamps_480p59_94) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_480p59_94",
                       720, 480);
}

TEST_F(NoCtxTest, st20p_default_timestamps_576p50) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P50, "st20p_default_timestamps_576p50", 720,
                       576);
}

/* st20p_default_timestamps_720p25, _720p50
 * As st20p_default_timestamps at 720p's ~60 and ~121 MB/s: a full 128-pad warm-up at
 * shaper timeslots of ~4 and ~2 us. */
TEST_F(NoCtxTest, st20p_default_timestamps_720p25) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P25, "st20p_default_timestamps_720p25", 1280,
                       720);
}

TEST_F(NoCtxTest, st20p_default_timestamps_720p50) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P50, "st20p_default_timestamps_720p50", 1280,
                       720);
}

/* st20p_default_timestamps_720p59_94, _2160p59_94
 * As st20p_default_timestamps where a pre-arm plan is kept: 720p59.94 (~145 MB/s), the
 * 720p rate past the timeslot gate (~1.7 us), and 2160p59.94 (~1.3 GB/s), the fastest
 * rate here (~0.2 us). */
TEST_F(NoCtxTest, st20p_default_timestamps_720p59_94) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_720p59_94",
                       1280, 720);
}

TEST_F(NoCtxTest, st20p_default_timestamps_2160p59_94) {
  initStrictPacingContext();
  if (IsSkipped() || HasFatalFailure()) return;
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_2160p59_94",
                       3840, 2160);
}

/* st20p_user_pacing
 * Config: TX kTxFlags = USER_PACING.
 * Plan:   t_user(n) = epoch n after start, start = the epoch at or after PTP now +
 *         800 ms (kSt20pUserPacingLeadNs); each request is on an epoch.
 * Expect: on every frame, St20pUserPacingOracle with expected launch = the 90 kHz tick
 *         of t_user(n) + TR_offset - VRX*trs
 *         1. COMPLETE, BPM packet count            expectCompletePrimaryFrame()
 *         2. packet 0 at the expected launch, 0/+10 us   verifyReceiveTiming(),
 *            kNoCtxPacingEarlyMaxNs, kNoCtxPacingLateMaxNs
 *         3. packet 0 at the instant its RTP names, 0/+10 us   verifyReceiveTiming()
 *         4. elapsed from frame 0 +-10 us          verifyReceiveTiming(),
 *            kNoCtxPacingElapsedErrorMaxNs
 *         5. timestamp == RTP header               expectRtpTimestamp()
 *         6. RTP == tick90k(expected launch), step 3600   verifyMediaClock()
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
 * Plan:   t_user(n) = epoch n after start + jitterMultipliers[n % 8]*T, start as in
 *         st20p_user_pacing; every offset is inside +-T/2, so each request snaps
 *         back to epoch n after start.
 * Expect: on every frame, checks 1-6 of st20p_user_pacing, with expected launch = the
 *         tick of (epoch nearest t_user(n)) + TR_offset - VRX*trs
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
 * Plan:   t_user(n) = epoch n after start + exactOffsets[n % 8]*T, start as in
 *         st20p_user_pacing; offsets -100 us .. +320 us, not snapped.
 * Expect: on every frame, St20pExactUserPacingOracle with expected launch = t_user(n)
 *         1, 2, 4, 5 as in st20p_user_pacing (launch 0/+10 us, kNoCtxPacingEarlyMaxNs,
 *            kNoCtxPacingLateMaxNs; elapsed +-10 us, kNoCtxPacingElapsedErrorMaxNs)
 *         3. packet 0 at the instant its RTP names, -T90k/2 .. +T90k/2 + 10 us, as
 *            the RTP is the tick nearest t_user(n)    kNoCtxRtpHalfTickNs
 *         6. RTP == tick90k(t_user(n))             verifyMediaClock()
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
