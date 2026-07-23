/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Runs a progressive and an interlaced st20p session side by side, at matched
 * width/height/fmt (so both carry the same bitrate -- interlaced sends half
 * the pixels per call at double the call rate), and cross-checks their
 * pacing parameters directly against each other instead of only bounds-
 * checking each in isolation. Ratios below are derived from
 * tv_init_pacing() and the st21_vrx_narrow/wide setup in tv_attach()
 * (lib/src/st2110/st_tx_video_session.c):
 *
 * - trs_ns: reactive (1080.0/1125.0) is identical for both (height=1080, so
 *   neither hits the <=576 SD-interlaced override). frame_time halves and
 *   st20_total_pkts halves (height>>1 for interlaced in tv_init_pkt()) for
 *   the interlaced session, so trs = frame_time*reactive/total_pkts is
 *   unchanged -- expect a ratio of ~1.0.
 * - tr_offset_ns: progressive (height>=1080) uses frame_time*(43/1125);
 *   interlaced HD (height>576, the "else" branch) uses
 *   frame_time*(22/1125)*2 = frame_time*(44/1125). With frame_time halved,
 *   the ratio interlaced/progressive collapses to 22/43 (~0.5116), not 0.5.
 * - vrx_pkts: st21_vrx_narrow = max(8, total_pkts/(27000*frame_time_sec)).
 *   total_pkts/frame_time_sec is a bitrate-invariant quantity (both total_pkts
 *   and frame_time_sec halve together for interlaced), so st21_vrx_narrow --
 *   and therefore pacing->vrx, which only subtracts a pacing-way-dependent
 *   constant that does not depend on interlaced -- comes out equal for both,
 *   not halved as a naive tr_offset/trs argument would suggest (that
 *   quotient is only used for ST21_PACING_WIDE, not the default narrow
 *   pacing exercised here).
 */

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

namespace {
constexpr double kTrsRatioTolerance = 0.02;
constexpr double kTrOffsetRatioExpected = 22.0 / 43.0;
constexpr double kTrOffsetRatioTolerance = 0.02;
constexpr double kVrxPktsTolerance = 1.0;
}  // namespace

TEST_F(NoCtxTest, st20p_pacing_progressive_vs_interlaced) {
  initDefaultContext();

  auto progressiveBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
      });
  auto* progressive = static_cast<St20pUserTimestamp*>(progressiveBundle.strategy);

  auto interlacedBundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true,
      [](St20pHandler* handler) { return new St20pUserTimestamp(handler); },
      [](St20pHandler* handler) {
        handler->sessionsOpsTx.interlaced = true;
        handler->sessionsOpsRx.interlaced = true;
        handler->sessionsOpsTx.fps = ST_FPS_P50;
        handler->sessionsOpsRx.fps = ST_FPS_P50;
        handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_USER_PACING;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_P] = 20010;
        handler->sessionsOpsTx.port.udp_port[MTL_SESSION_PORT_R] = 20011;
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_P] = 20010;
        handler->sessionsOpsRx.port.udp_port[MTL_SESSION_PORT_R] = 20011;
      });
  auto* interlaced = static_cast<St20pUserTimestamp*>(interlacedBundle.strategy);

  StartFakePtpClock();
  progressiveBundle.handler->startSession();
  interlacedBundle.handler->startSession();
  mtl_start(ctx->handle);

  ASSERT_EQ(progressive->getPacingParameters(), 0);
  ASSERT_EQ(interlaced->getPacingParameters(), 0);
  EXPECT_GT(progressive->pacing_trs_ns, 0.0);
  EXPECT_GT(interlaced->pacing_trs_ns, 0.0);

  sleepUntilFailure();

  progressiveBundle.handler->stopSession();
  interlacedBundle.handler->stopSession();

  ASSERT_GT(progressive->idx_tx, 0u) << "progressive bundle did not transmit any frames";
  ASSERT_GT(progressive->idx_rx, 0u) << "progressive bundle did not receive any frames";
  ASSERT_EQ(progressive->idx_tx, progressive->idx_rx)
      << "progressive bundle TX/RX frame count mismatch";

  ASSERT_GT(interlaced->idx_tx, 0u) << "interlaced bundle did not transmit any fields";
  ASSERT_GT(interlaced->idx_rx, 0u) << "interlaced bundle did not receive any fields";
  ASSERT_EQ(interlaced->idx_tx, interlaced->idx_rx)
      << "interlaced bundle TX/RX field count mismatch";

  const double trs_ratio = interlaced->pacing_trs_ns / progressive->pacing_trs_ns;
  EXPECT_NEAR(trs_ratio, 1.0, kTrsRatioTolerance)
      << "trs_ns should be ~equal between matched-bitrate progressive ("
      << progressive->pacing_trs_ns << "ns) and interlaced (" << interlaced->pacing_trs_ns
      << "ns) sessions";

  const double tr_offset_ratio =
      interlaced->pacing_tr_offset_ns / progressive->pacing_tr_offset_ns;
  EXPECT_NEAR(tr_offset_ratio, kTrOffsetRatioExpected, kTrOffsetRatioTolerance)
      << "tr_offset_ns ratio (interlaced/progressive) should match the 22/43 HD "
         "interlaced-vs-progressive coefficient ratio; progressive="
      << progressive->pacing_tr_offset_ns
      << "ns interlaced=" << interlaced->pacing_tr_offset_ns << "ns";

  EXPECT_NEAR(static_cast<double>(interlaced->pacing_vrx_pkts),
              static_cast<double>(progressive->pacing_vrx_pkts), kVrxPktsTolerance)
      << "vrx_pkts should be ~equal between matched-bitrate progressive ("
      << progressive->pacing_vrx_pkts << ") and interlaced ("
      << interlaced->pacing_vrx_pkts
      << ") sessions -- it is derived from a bitrate-invariant quantity, not halved";

  progressive->assertTimingWithinBudget();
  progressive->assertRlLatencyWithinBounds();
  interlaced->assertTimingWithinBudget();
  interlaced->assertRlLatencyWithinBounds();
}
