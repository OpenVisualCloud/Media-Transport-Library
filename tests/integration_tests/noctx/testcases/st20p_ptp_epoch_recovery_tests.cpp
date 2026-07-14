/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include <atomic>
#include <chrono>
#include <thread>

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
