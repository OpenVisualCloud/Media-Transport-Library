/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* ST20p RL pacing that does not depend on the TX scheduler: every ST 2022-7 session
 * keeps the VRX margin of the best one, on both legs, measured by the library's RX
 * timing parser on NIC RX timestamps. See README.md, "Equal VRX margin oracle".
 */

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"
#include "strategies/st20p_strategies.hpp"

namespace {
/* Indexes into --port_list, interleaved as in CI: TX on PF A, RX on PF B. */
constexpr int kTxPortP = 0;
constexpr int kRxPortP = 1;
constexpr int kTxPortR = 2;
constexpr int kRxPortR = 3;
constexpr int kSessions = 8;
constexpr uint16_t kUdpPortBase = 20000;
/* The 2 s tail keeps the TX stop out of the window. */
constexpr int kTestDurationS = kSt20pVrxWarmupS + kSt20pVrxWindowS + 2;
constexpr double kVrxMarginMaxDiffPkts = 1.0;
/* The RTP wrap every ~13.25 h is not handled: ~1 s of frames then falls off band. */
constexpr uint64_t kRtpWrapOffBandFrames = 61;
/* MTL's PTP time - the RX PHC: 60 1080p59.94 frames exactly, so the TX epochs keep
 * their phase on the PHC, and far above the delivery latency of a software RX time. */
constexpr int64_t kPtpMinusPhcNs = 1001 * NS_PER_MS;
static_assert(kPtpMinusPhcNs * 60 % (1001 * NS_PER_MS) == 0, "a whole number of frames");

struct LegVrx {
  size_t frames = 0;
  double mean = 0;
  int32_t p1 = 0, p5 = 0, p50 = 0;
  size_t underflows = 0;
};

/* Mean of vrx_min less its lowest 1 %, and its percentiles. */
LegVrx summarize(std::vector<int32_t> vrx) {
  LegVrx leg;
  leg.frames = vrx.size();
  if (vrx.empty()) return leg;
  leg.underflows = std::count_if(vrx.begin(), vrx.end(), [](int32_t v) { return v < 0; });
  std::sort(vrx.begin(), vrx.end());
  const size_t trim = vrx.size() / 100;
  for (size_t k = trim; k < vrx.size(); k++) leg.mean += vrx[k];
  leg.mean /= vrx.size() - trim;
  leg.p1 = vrx[vrx.size() / 100];
  leg.p5 = vrx[vrx.size() / 20];
  leg.p50 = vrx[vrx.size() / 2];
  return leg;
}

/* Every session and port is logged before any check, and every check is non-fatal. */
void expectEqualVrxMargin(const std::vector<St20pVrxRecorder*>& rx,
                          const std::vector<int>& tx_sch, double frame_time_ns) {
  const size_t min_frames = (double)kSt20pVrxWindowS * NS_PER_S / frame_time_ns * 0.8;

  std::vector<int64_t> delivery;
  for (const auto* r : rx)
    delivery.insert(delivery.end(), r->delivery_ns.begin(), r->delivery_ns.end());
  if (delivery.empty()) {
    ADD_FAILURE() << "no frames in the measurement window";
    return;
  }
  std::nth_element(delivery.begin(), delivery.begin() + delivery.size() / 2,
                   delivery.end());
  const int64_t delivery_p50 = delivery[delivery.size() / 2];
  fprintf(stderr, "NoCtx vrx: median PTP now - receive_timestamp %" PRId64 " ns\n",
          delivery_p50);

  LegVrx legs[MTL_SESSION_PORT_MAX][kSessions];
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    for (size_t i = 0; i < rx.size(); i++) {
      const LegVrx& leg = legs[port][i] = summarize(rx[i]->vrx_min[port]);
      fprintf(stderr,
              "NoCtx vrx: session %zu port %d tx sch %d: frames %zu (sw %" PRIu64
              ", off band %" PRIu64 "), missing max %u pkts, %" PRIu64
              " frames > %u, vrx_min trimmed mean %.2f p1 %d p5 %d p50 %d, underflows "
              "%zu\n",
              i, port, tx_sch[i], leg.frames, rx[i]->sw_rx_frames[port],
              rx[i]->off_band_frames[port], rx[i]->max_missing_pkts[port],
              rx[i]->partial_frames[port], kSt20pVrxMaxLegLagPkts, leg.mean, leg.p1,
              leg.p5, leg.p50, leg.underflows);
    }
  }

  EXPECT_TRUE(delivery_p50 > kPtpMinusPhcNs &&
              delivery_p50 < kPtpMinusPhcNs + 2 * frame_time_ns)
      << "median PTP now - receive_timestamp " << delivery_p50 << " ns is not in ("
      << kPtpMinusPhcNs << ", +2T): a NIC RX timestamp puts it at " << kPtpMinusPhcNs
      << " ns plus the delivery latency, a software one at the latency alone; otherwise "
      << "the PHC follower failed";
  for (int port = 0; port < MTL_SESSION_PORT_MAX; port++) {
    double best = -INFINITY;
    for (size_t i = 0; i < rx.size(); i++) {
      const LegVrx& leg = legs[port][i];
      EXPECT_EQ(rx[i]->sw_rx_frames[port], 0u)
          << "session " << i << " port " << port
          << ": frames timed with software RX times, so the port has no NIC RX "
             "timestamps";
      EXPECT_GE(leg.frames, min_frames)
          << "session " << i << " port " << port << ": too few NIC-timed frames";
      EXPECT_LE(rx[i]->off_band_frames[port], leg.frames / 100 + kRtpWrapOffBandFrames)
          << "session " << i << " port " << port << ": " << rx[i]->off_band_frames[port]
          << " frames with rtp_offset more than half a frame from the K shift, so not "
             "timed against their own epoch";
      EXPECT_LE(rx[i]->partial_frames[port], leg.frames / 100)
          << "session " << i << " port " << port << ": in " << rx[i]->partial_frames[port]
          << " frames this leg finished more than " << kSt20pVrxMaxLegLagPkts
          << " packets (max " << rx[i]->max_missing_pkts[port]
          << ") behind the other: TX paces the two legs apart, the leg lost packets or "
             "the RX tasklet stalled; the parser does not time a lagging leg's tail, so "
             "its vrx_min covers only its head";
      if (leg.frames) best = std::max(best, leg.mean);
    }
    for (size_t i = 0; i < rx.size(); i++) {
      if (!legs[port][i].frames) continue;
      EXPECT_GE(legs[port][i].mean, best - kVrxMarginMaxDiffPkts)
          << "session " << i << " port " << port << " on tx sch " << tx_sch[i]
          << ": trimmed mean vrx_min " << legs[port][i].mean << " vs best " << best;
    }
  }
}
}  // namespace

/* st20p_redundant_1080p59_s8_equal_vrx_margin
 * Config:    4 ports. MTL's PTP time is the kRxPortP PHC + kPtpMinusPhcNs
 *            (PhcFollowingClock, read and followed from before
 *            initStrictPacingContext()): the RX timing parser places the epochs of
 *            vrx_min on raw NIC timestamps. 8 sessions 1080p59.94 YUV 4:2:2 10-bit
 *            BPM, RFC 4175 frames (no conversion), 3 buffers, UDP kUdpPortBase + 2i;
 *            TX kTxPortP + kTxPortR -> RX kRxPortP + kRxPortR (ST 2022-7); default RL
 *            pacing and scheduler quota; TX and RX BLOCK_GET, RX TIMING_PARSER_META.
 *            Every TX session is created before any RX one, so RX cannot take a TX
 *            scheduler.
 * Plan:      default pacing for kTestDurationS = 52 s.
 * Expect:    after TX creation:
 *            1. one TX scheduler holds >= 3 sessions and >= 2 are in use
 *            St20pVrxRecorder, 30 s from 20 s after each session's first frame;
 *            after stop, expectEqualVrxMargin(), all non-fatal, after logging every
 *            session and port:
 *            2. median (PTP now - receive_timestamp) in (kPtpMinusPhcNs,
 *               kPtpMinusPhcNs + 2T), so the frames carry NIC RX timestamps
 *            3. per session and port, no frame whose rtp_offset shows software RX
 *               times, >= 80 % of the window's frames NIC timed, and <= 1 % of them
 *               + kRtpWrapOffBandFrames with rtp_offset off the K shift by >= T/2
 *            4. per session and port, <= 1 % of those frames finish more than
 *               kSt20pVrxMaxLegLagPkts behind the other leg
 *            5. per port, each session's vrx_min mean less its lowest 1 % >= the
 *               best session's - 1 packet (kVrxMarginMaxDiffPkts)
 *            and PhcFollowingClock::healthError() is empty:
 *            6. lag <= kPhcFollowMaxLagNs, read gaps <= kPhcFollowMaxGapNs, >= 90 %
 *               of the 10 ms read slots read (kPhcFollowMinReadShare)
 * Skip/Fail: strict topology of (kTxPortP, kRxPortP) and (kTxPortR, kRxPortR), or RX
 *            ports on different PHCs: SKIP, FAIL with NOCTX_REQUIRE_STRICT=1; then
 *            FAIL, never SKIP, without an exclusive CPU partition.
 */
TEST_F(NoCtxTest, st20p_redundant_1080p59_s8_equal_vrx_margin) {
  ASSERT_GE((int)ctx->para.num_ports, 4) << "needs 4 ports";
  /* Before the PHC follower reads kRxPortP; initStrictPacingContext() repeats leg P. */
  for (const std::string& why :
       {strictPacingTopologyError(ctx->para.port[kTxPortP], ctx->para.port[kRxPortP]),
        strictPacingTopologyError(ctx->para.port[kTxPortR], ctx->para.port[kRxPortR]),
        sharedPhcError(ctx->para.port[kRxPortP], ctx->para.port[kRxPortR])}) {
    requireStrictTopology(why);
    if (IsSkipped() || HasFatalFailure()) return;
  }
  const std::string isolation_error = exclusivePartitionError(
      "st20p_redundant_1080p59_s8_equal_vrx_margin (8 RL paced sessions, vrx_min to 1 "
      "packet)");
  if (!isolation_error.empty()) FAIL() << isolation_error;

  PhcFollowingClock ptp;
  const std::string ptp_error = ptp.open(ctx->para.port[kRxPortP], kPtpMinusPhcNs);
  ASSERT_TRUE(ptp_error.empty()) << ptp_error;
  /* Following through mtl_init() keeps the first read's lag to 10 ms of drift. The pin
   * assumes, unchecked as KahawaiTest does not link DPDK, that the test starts on the
   * partition's lowest CPU (isolate.sh), which EAL takes as its main lcore without
   * --lcores and which runs no MTL scheduler. */
  const std::string follow_error = ptp.follow();
  ASSERT_TRUE(follow_error.empty()) << follow_error;
  initStrictPacingContext(PhcFollowingClock::now);
  if (IsSkipped() || HasFatalFailure()) return;

  std::vector<St20pHandler*> handlers;
  std::vector<St20pVrxRecorder*> recorders;
  std::vector<int> tx_sch;
  for (int i = 0; i < kSessions; i++) {
    auto bundle = createSt20pHandlerBundle(
        /*createTx=*/true, /*createRx=*/false,
        [](St20pHandler* handler) {
          return new St20pVrxRecorder(handler, kPtpMinusPhcNs);
        },
        [i](St20pHandler* handler) {
          handler->fillSt20Ops(kUdpPortBase + 2 * i, 3, ST20_FMT_YUV_422_10BIT, 1920,
                               1080, 112, ST_FPS_P59_94);
          handler->sessionsOpsTx.input_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
          handler->sessionsOpsTx.flags |= ST20P_TX_FLAG_BLOCK_GET;
          handler->sessionsOpsRx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
          handler->sessionsOpsRx.flags |=
              ST20P_RX_FLAG_BLOCK_GET | ST20P_RX_FLAG_TIMING_PARSER_META;
          handler->setSessionPorts(kTxPortP, kRxPortP, kTxPortR, kRxPortR);
        });
    ASSERT_NE(bundle.handler->sessionsHandleTx, nullptr);
    handlers.push_back(bundle.handler);
    recorders.push_back(static_cast<St20pVrxRecorder*>(bundle.strategy));
    tx_sch.push_back(st20p_tx_get_sch_idx(bundle.handler->sessionsHandleTx));
  }

  std::map<int, int> sessions_per_sch;
  for (int sch : tx_sch) sessions_per_sch[sch]++;
  int most = 0;
  std::string packing;
  for (const auto& sch : sessions_per_sch) {
    most = std::max(most, sch.second);
    packing += " sch " + std::to_string(sch.first) + ": " + std::to_string(sch.second);
  }
  ASSERT_TRUE(sessions_per_sch.size() >= 2 && most >= 3)
      << "TX sessions per scheduler:" << packing
      << "; the check needs one scheduler with >= 3 sessions and >= 2 schedulers";

  for (auto* handler : handlers) {
    handler->createSessionRx();
    ASSERT_NE(handler->sessionsHandleRx, nullptr);
  }

  ASSERT_EQ(mtl_start(ctx->handle), 0);
  for (auto* handler : handlers) handler->startSession();
  sleepUntilFailure(kTestDurationS);
  for (auto* handler : handlers) handler->stopSession();
  ptp.stop();

  const std::string follower_error = ptp.healthError();
  EXPECT_TRUE(follower_error.empty()) << follower_error;
  expectEqualVrxMargin(recorders, tx_sch, NS_PER_S / st_frame_rate(ST_FPS_P59_94));
}
