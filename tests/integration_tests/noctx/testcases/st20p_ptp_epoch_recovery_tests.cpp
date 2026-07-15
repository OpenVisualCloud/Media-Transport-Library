/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/constants.hpp"
#include "core/test_fixture.hpp"
#include "handlers/st20p_handler.hpp"

namespace {

std::atomic<uint64_t> g_epoch_recovery_start_ns{0};
std::atomic<int64_t> g_epoch_recovery_future_offset_ns{0};

uint64_t EpochRecoveryMonotonicNowNs() {
  struct timespec spec;
  clock_gettime(CLOCK_MONOTONIC, &spec);
  return (uint64_t)spec.tv_sec * NS_PER_S + spec.tv_nsec;
}

/* Simulates a PHC seeded from the host system clock (ahead of the true PTP
 * grandmaster) that later gets corrected by the PTP servo. Returns
 * elapsed-since-first-call plus a controllable offset the test steps to 0
 * mid-run to emulate that correction. */
uint64_t EpochRecoveryPtpClockNow(void* priv) {
  (void)priv;
  uint64_t start = g_epoch_recovery_start_ns.load(std::memory_order_acquire);
  if (!start) {
    start = EpochRecoveryMonotonicNowNs();
    g_epoch_recovery_start_ns.store(start, std::memory_order_release);
  }
  int64_t offset = g_epoch_recovery_future_offset_ns.load(std::memory_order_acquire);
  return (EpochRecoveryMonotonicNowNs() - start) + offset;
}

/* Shared repro body: calc_frame_count_since_epoch() (the epoch/onward-gap
 * math under test) runs identically for every st21_tx_pacing_way -- the
 * pacing way only changes how packets are scheduled onto the wire, not how
 * the epoch catch-up gap is computed. Parametrizing over pacing_way confirms
 * (or disproves) that the bug is pacing-way-specific rather than TSN-only. */
void RunEpochOnwardRecoveryCase(NoCtxTest* self, struct st_tests_context* ctx,
                                enum st21_tx_pacing_way pacing_way) {
  /* Offset must exceed pacing->max_onward_epochs (~1s of frames) so the
   * post-step gap actually trips stat_epoch_onward instead of being
   * absorbed as ordinary jitter. */
  constexpr int64_t kFutureOffsetNs = 3 * (int64_t)NS_PER_S;
  g_epoch_recovery_start_ns.store(0, std::memory_order_release);
  g_epoch_recovery_future_offset_ns.store(kFutureOffsetNs, std::memory_order_release);

  ctx->para.pacing = pacing_way;
  ctx->para.ptp_get_time_fn = EpochRecoveryPtpClockNow;
  ctx->para.log_level = MTL_LOG_LEVEL_INFO;
  ctx->para.flags &= ~MTL_FLAG_DEV_AUTO_START_STOP;
  /* TSN launch-time pacing compares against the phc, which only the
   * built-in ptp service (fed by ptp_get_time_fn above) disciplines. */
  if (pacing_way == ST21_TX_PACING_WAY_TSN) ctx->para.flags |= MTL_FLAG_PTP_ENABLE;
  /* Shorten the periodic "M T DEV STATE" stat dump (default 10s) so the run
   * below comfortably covers several dumps without needing a long sleep. */
  ctx->para.dump_period_s = 2;
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  /* No frame-content/timestamp strategy: St20pDefaultTimestamp assumes a
   * steady real-time-correlated clock, which this test deliberately violates
   * to reproduce the bug. Only stat_epoch_onward is asserted on below. */
  auto bundle =
      self->createSt20pHandlerBundle(/*createTx=*/true, /*createRx=*/true, nullptr);

  bundle.handler->startSession();
  mtl_start(ctx->handle);

  /* let the session settle and transmit for a while against the inflated clock */
  std::this_thread::sleep_for(std::chrono::seconds(6));

  /* simulate the PTP servo correcting the clock back to the true grandmaster time */
  g_epoch_recovery_future_offset_ns.store(0, std::memory_order_release);

  struct st20_tx_user_stats stats = {};
  uint64_t last_onward = 0;
  bool recovered = false;

  /* a correct fix resyncs on the first frame after the step, so the delta
   * should hit 0 by the poll right after the one that observes the resync */
  for (int i = 0; i < 3; i++) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(st20p_tx_get_session_stats(bundle.handler->sessionsHandleTx, &stats), 0);
    uint64_t delta = stats.common.stat_epoch_onward - last_onward;
    last_onward = stats.common.stat_epoch_onward;
    if (delta == 0) {
      recovered = true;
      break;
    }
  }

  bundle.handler->stopSession();

  EXPECT_TRUE(recovered)
      << "stat_epoch_onward kept incrementing after the simulated PTP correction "
         "(cumulative value "
      << last_onward
      << ") -- TX pacing never recovered from an epoch initialized ahead of real time";
}

/* Waits for the first complete RX frame with a generous bound, since real PF
 * hardware RX flow/ARP establishment latency (not sampling-window size) is
 * what actually varies run to run -- confirmed via RX_VIDEO_SESSION logging
 * 0 pkts for the whole run when a fixed pre-sleep was used instead. Once
 * flowing, discards kWarmupFrames more complete frames, then samples up to
 * kMaxSampleFrames within a separate, steady-state-only deadline. Returns
 * the total complete-frame count observed; span_ratios gets one
 * observed/expected packet-train-span ratio per sampled frame with adequate
 * st20_rx_tp_meta coverage. */
int CollectPacingSpanRatios(NoCtxTest::St20pHandlerBundle& bundle, double trs_ns,
                            std::vector<double>& span_ratios) {
  constexpr int kWarmupFrames = 5;
  constexpr int kMaxSampleFrames = 20;
  st20p_rx_handle rx = bundle.handler->sessionsHandleRx;
  int complete_frames = 0;
  int sampled_frames = 0;

  auto first_frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (complete_frames == 0 &&
         std::chrono::steady_clock::now() < first_frame_deadline) {
    struct st_frame* frame = st20p_rx_get_frame(rx);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (frame->status == ST_FRAME_STATUS_COMPLETE) complete_frames++;
    st20p_rx_put_frame(rx, frame);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (sampled_frames < kMaxSampleFrames &&
         std::chrono::steady_clock::now() < deadline) {
    struct st_frame* frame = st20p_rx_get_frame(rx);
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (frame->status != ST_FRAME_STATUS_COMPLETE) {
      st20p_rx_put_frame(rx, frame);
      continue;
    }

    complete_frames++;
    if (complete_frames > kWarmupFrames) {
      sampled_frames++;
      struct st20_rx_tp_meta* tp = frame->tp[MTL_SESSION_PORT_P];
      if (tp && tp->pkts_cnt >= 2 && tp->pkts_cnt >= frame->pkts_total / 2) {
        double observed_span_ns = (double)tp->ipt_avg * (tp->pkts_cnt - 1);
        double expected_span_ns = trs_ns * (tp->pkts_cnt - 1);
        span_ratios.push_back(observed_span_ns / expected_span_ns);
      }
    }
    st20p_rx_put_frame(rx, frame);
  }
  return complete_frames;
}

} /* namespace */

/* TSN pacing (launch-time offload) is only advertised by PF drivers; this
 * test requires the noctx port pair to be bound as PF, not VF. The "_pf_"
 * infix lets run.sh/run_pf.sh select/exclude it with a plain gtest_filter
 * wildcard instead of parsing the test source.
 *
 * TSN launch-time-pacing offload has only been validated on Intel E830 (PCI
 * device 0x12d2); other NICs may not advertise it or may behave differently.
 * Run this only on E830 PF ports. */
TEST_F(NoCtxTest, st20p_tx_epoch_onward_recovers_after_ptp_step_pf_tsn_pacing) {
  RunEpochOnwardRecoveryCase(this, ctx, ST21_TX_PACING_WAY_TSN);
}

/* Same repro with software TSC-based pacing (no NIC launch-time offload, so
 * this pacing way also runs on VF ports) -- confirms the epoch-onward gap
 * never recovers regardless of pacing way, i.e. this is not a TSN-only bug. */
TEST_F(NoCtxTest, st20p_tx_epoch_onward_recovers_after_ptp_step_tsc_pacing) {
  RunEpochOnwardRecoveryCase(this, ctx, ST21_TX_PACING_WAY_TSC);
}

/* Smoke-test: does TSN launch-time pacing actually spread packets across the
 * frame interval on the wire, instead of merely completing session setup?
 * Uses E830 PF RX hardware timestamps (st20_rx_tp_meta::ipt_avg, populated by
 * ST20P_RX_FLAG_TIMING_PARSER_META with MTL_FLAG_ENABLE_HW_TIMESTAMP) so
 * scheduler/poll jitter on the RX side cannot manufacture apparent spread. */
TEST_F(NoCtxTest, st20p_tx_packets_are_spread_over_frame_pf_tsn_pacing) {
  ctx->para.pacing = ST21_TX_PACING_WAY_TSN;
  ctx->para.flags |= MTL_FLAG_PTP_ENABLE | MTL_FLAG_ENABLE_HW_TIMESTAMP;
  ctx->para.flags &= ~(MTL_FLAG_PTP_SOURCE_TSC | MTL_FLAG_DEV_AUTO_START_STOP);
  ctx->handle = mtl_init(&ctx->para);
  ASSERT_TRUE(ctx->handle != nullptr);

  auto bundle = createSt20pHandlerBundle(
      /*createTx=*/true, /*createRx=*/true, nullptr, [](St20pHandler* handler) {
        handler->sessionsOpsRx.flags |= ST20P_RX_FLAG_TIMING_PARSER_META;
      });
  ASSERT_NE(bundle.handler, nullptr);

  bundle.handler->startSessionTx();
  mtl_start(ctx->handle);

  double tr_offset_ns = 0, trs_ns = 0;
  uint32_t vrx_pkts = 0;
  ASSERT_EQ(st20p_tx_get_pacing_params(bundle.handler->sessionsHandleTx, &tr_offset_ns,
                                       &trs_ns, &vrx_pkts),
            0);
  ASSERT_GT(trs_ns, 0.0);

  std::vector<double> span_ratios;
  int complete_frames = CollectPacingSpanRatios(bundle, trs_ns, span_ratios);

  bundle.handler->stopSession();

  constexpr int kMinUsableFrames = 15;
  ASSERT_GE((int)span_ratios.size(), kMinUsableFrames)
      << "only " << span_ratios.size() << " of " << complete_frames
      << " complete RX frames had adequate timing-parser packet coverage";

  int in_range = 0;
  for (double ratio : span_ratios) {
    if (ratio >= 0.5 && ratio <= 1.5) in_range++;
  }
  EXPECT_GE(in_range, (int)(span_ratios.size() * 0.8))
      << "fewer than 80% of sampled frames show a packet-train span within "
         "[0.5, 1.5] of the TSN-paced expectation (trs_ns="
      << trs_ns << ")";

  std::vector<double> sorted_ratios(span_ratios);
  std::sort(sorted_ratios.begin(), sorted_ratios.end());
  size_t mid = sorted_ratios.size() / 2;
  double median = (sorted_ratios.size() % 2)
                      ? sorted_ratios[mid]
                      : (sorted_ratios[mid - 1] + sorted_ratios[mid]) / 2.0;
  EXPECT_GE(median, 0.7);
  EXPECT_LE(median, 1.3);
}
