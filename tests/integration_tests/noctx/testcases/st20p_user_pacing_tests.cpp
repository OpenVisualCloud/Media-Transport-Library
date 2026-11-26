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

#include <algorithm>
#include <cerrno>
#include <vector>

#include "core/test_fixture.hpp"
#include "strategies/st20p_strategies.hpp"

static_assert(kNoCtxPacingEarlyMaxNs == 0, "comments quote 0 us early");
static_assert(kNoCtxPacingLateMaxNs == 10 * NS_PER_US, "comments quote +10 us");
static_assert(kNoCtxPacingElapsedErrorMaxNs == 10 * NS_PER_US, "comments quote +-10 us");
static_assert(kSt20pUserPacingLeadNs == 800 * NS_PER_MS, "comments quote 800 ms");

namespace {

/* Default pacing case: initStrictPacingContext() + St20pDefaultPacingOracle. */
void runDefaultTimestamps(NoCtxTest* test, st_tests_context* ctx, enum st_fps fps,
                          const char* name, uint width = 1920, uint height = 1080) {
  test->initStrictPacingContext();
  if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return;

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
  auto* strategy = static_cast<St20pDefaultPacingOracle*>(bundle.strategy);

  bundle.handler->startSession();
  ASSERT_EQ(mtl_start(ctx->handle), 0);

  test->sleepUntilFailure();
  bundle.handler->stopSession();

  ASSERT_GT(strategy->idx_rx, 0u) << name << " did not receive any frames";
}

/* User pacing (snapped or exact): initStrictPacingContext() + St20p*UserPacingOracle. */
void runUserPacingCase(NoCtxTest* test, st_tests_context* ctx, const char* name,
                       bool exact, std::vector<double> offsets = {}) {
  test->initStrictPacingContext();
  if (::testing::Test::IsSkipped() || ::testing::Test::HasFatalFailure()) return;

  uint32_t tx_flags = ST20P_TX_FLAG_USER_PACING;
  if (exact) tx_flags |= ST20P_TX_FLAG_EXACT_USER_PACING;
  auto bundle = test->createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [exact, offsets](St20pHandler* handler) -> FrameTestStrategy* {
        if (exact) return new St20pExactUserPacingOracle(handler, offsets);
        return new St20pUserPacingOracle(handler, offsets);
      },
      [tx_flags](St20pHandler* handler) { handler->sessionsOpsTx.flags |= tx_flags; });

  auto* handler = bundle.handler;
  auto* strategy = static_cast<St20pUserPacingOracle*>(bundle.strategy);

  handler->startSession();
  if (exact) {
    const int pacing_status = strategy->getPacingParameters();
    if (pacing_status == 0) {
      EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
      EXPECT_GT(strategy->pacing_trs_ns, 0.0);
      EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
    } else {
      EXPECT_EQ(pacing_status, -ENOTSUP) << "Unexpected pacing query result";
    }
    ASSERT_EQ(mtl_start(ctx->handle), 0);
  } else {
    ASSERT_EQ(mtl_start(ctx->handle), 0);
    ASSERT_EQ(strategy->getPacingParameters(), 0);
    EXPECT_GT(strategy->pacing_tr_offset_ns, 0.0);
    EXPECT_GT(strategy->pacing_trs_ns, 0.0);
    EXPECT_GT(strategy->pacing_vrx_pkts, 0u);
  }

  test->sleepUntilFailure();
  handler->stopSession();

  const size_t min_frames = std::max<size_t>(1, offsets.size());
  if (exact) {
    ASSERT_GE(handler->txFrames(), min_frames)
        << name << " transmitted too few frames for offset coverage";
    ASSERT_GE(handler->rxFrames(), min_frames)
        << name << " received too few frames for offset coverage";
    EXPECT_EQ(handler->txFrames(), handler->rxFrames())
        << name << " TX/RX frame count mismatch";
  }
  ASSERT_GE(strategy->idx_tx, min_frames) << name << " TX frames below expectation";
  ASSERT_GE(strategy->idx_rx, min_frames) << name << " RX frames below expectation";
  ASSERT_EQ(strategy->idx_tx, strategy->idx_rx) << name << " TX/RX frame count mismatch";
}

}  // namespace

/* st20p_default_timestamps: 1080p25, 1080p59.94, 1080p60, SD, 720p, 2160p59.94. */
TEST_F(NoCtxTest, st20p_default_timestamps) {
  runDefaultTimestamps(this, ctx, ST_FPS_P25, "st20p_default_timestamps");
}

TEST_F(NoCtxTest, st20p_default_timestamps_1080p59_94) {
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_1080p59_94");
}

TEST_F(NoCtxTest, st20p_default_timestamps_1080p60) {
  runDefaultTimestamps(this, ctx, ST_FPS_P60, "st20p_default_timestamps_1080p60");
}

TEST_F(NoCtxTest, st20p_default_timestamps_480p59_94) {
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_480p59_94",
                       720, 480);
}

TEST_F(NoCtxTest, st20p_default_timestamps_576p50) {
  runDefaultTimestamps(this, ctx, ST_FPS_P50, "st20p_default_timestamps_576p50", 720,
                       576);
}

TEST_F(NoCtxTest, st20p_default_timestamps_720p25) {
  runDefaultTimestamps(this, ctx, ST_FPS_P25, "st20p_default_timestamps_720p25", 1280,
                       720);
}

TEST_F(NoCtxTest, st20p_default_timestamps_720p50) {
  runDefaultTimestamps(this, ctx, ST_FPS_P50, "st20p_default_timestamps_720p50", 1280,
                       720);
}

TEST_F(NoCtxTest, st20p_default_timestamps_720p59_94) {
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_720p59_94",
                       1280, 720);
}

TEST_F(NoCtxTest, st20p_default_timestamps_2160p59_94) {
  runDefaultTimestamps(this, ctx, ST_FPS_P59_94, "st20p_default_timestamps_2160p59_94",
                       3840, 2160);
}

/* st20p_user_pacing: on-epoch requests (RTP step 3600). */
TEST_F(NoCtxTest, st20p_user_pacing) {
  runUserPacingCase(this, ctx, "st20p_user_pacing", /*exact=*/false);
}

/* st20p_user_pacing_offset_jitter: offsets inside +-T/2 snap back to each epoch. */
TEST_F(NoCtxTest, st20p_user_pacing_offset_jitter) {
  runUserPacingCase(this, ctx, "st20p_user_pacing_offset_jitter", /*exact=*/false,
                    {0, 0.3, 0.1, -0.49, 0.37, -0.14, 0.0, 0.44});
}

/* st20p_exact_user_pacing: unsnapped offsets (-100 us..+320 us), RTP = tick90k(t_user).
 * Offsets must stay smaller than in snapped user pacing (~(tr_offset - processing)). */
TEST_F(NoCtxTest, st20p_exact_user_pacing) {
  runUserPacingCase(this, ctx, "st20p_exact_user_pacing", /*exact=*/true,
                    {0.002, 0.007, -0.002, 0.008, -0.0005, 0.0033, -0.0025, 0.0051});
}
