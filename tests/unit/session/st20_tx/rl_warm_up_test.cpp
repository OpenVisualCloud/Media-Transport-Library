/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "session/st20_tx_harness.h"
#include "session/st_video_transmitter_harness.h"

namespace {

constexpr int kPortP = 0;
constexpr int kPortR = 1;

/* 1080p59 GPM on a trained E810 shaper with the patched 2 KB burst bucket */
constexpr long double kTrs = 16683333.333L * 1080 / 1125 / 4320;
constexpr uint32_t kWarmPkts = 128;
constexpr uint16_t kPadSize = 1262;
constexpr long double kDrain = kTrs * 164 / 165;
constexpr long double kCredit = kDrain * 2048 / kPadSize;
/* the pads a cut of up to a pad takes: 64 B coarse cuts, then 8 B fine ones */
constexpr int kTrimPads = kPadSize / 64 + 64 / 8;

class St20TxRlWarmUpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_trs_init());
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    ut_trs_set_warm_pkts_cap(ctx_, 1000); /* effectively unbounded */
  }

  void TearDown() override {
    ut_trs_destroy(ctx_);
  }

  /* shaper draining one pad per trs, no burst credit */
  void SetTrs(long double trs) {
    ut_trs_set_trs(ctx_, trs);
    ut_trs_set_drain(ctx_, kPortP, trs);
  }

  void UseRealisticShaper(int port) {
    ut_trs_set_trs(ctx_, kTrs);
    ut_trs_set_warm_pkts_cap(ctx_, kWarmPkts);
    ut_trs_set_drain(ctx_, port, kDrain);
    ut_trs_set_credit(ctx_, port, kCredit);
  }

  /* a realistic frame from the warm-up open, pad burst call (from 1) stalling; a held
   * packet 0 goes when the tasklet is back at the target */
  static constexpr uint64_t kStallTargetTsc = 10000000;
  void RunFrameWithStall(uint32_t call, uint64_t stall_ns, bool after_doorbell) {
    const uint64_t warm_up[] = {kStallTargetTsc - (uint64_t)(kWarmPkts * kTrs)};
    const uint64_t at_target[] = {kStallTargetTsc};

    ut_trs_destroy(ctx_);
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    UseRealisticShaper(kPortP);
    ut_trs_set_mock_tsc_script(ctx_, warm_up, 1);
    ut_trs_enqueue_first_pkt(ctx_, kStallTargetTsc);
    ut_trs_set_burst_stall(ctx_, call, stall_ns, after_doorbell);

    ut_trs_call_rl_tasklet(ctx_);
    ut_trs_call_rl_tasklet(ctx_);
    queued_by_pre_arm_ = ut_trs_real_send_count(ctx_);
    ut_trs_set_mock_tsc_script(ctx_, at_target, 1);
    ut_trs_call_rl_tasklet(ctx_);
  }

  /* packet 0 on the plan: at its target, or later by less than one fine trim */
  void ExpectOnPlan(long double late, int port, const std::string& what) {
    EXPECT_GE(late, 0) << what;
    EXPECT_LT(late, ut_trs_fine_trim_ns(ctx_, port)) << what;
  }

  ut_trs_ctx* ctx_ = nullptr;
  uint32_t queued_by_pre_arm_ = 0;
};

TEST_F(St20TxRlWarmUpTest, FastIterationsReachTargetWithoutShrinking) {
  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_target_tsc(ctx_, 10000);
  /* Successful bursts and recalculation each sample TSC. */
  const uint64_t script[] = {0,    1000, 1000, 2000, 2000, 3000,  3000,
                             4000, 4000, 5000, 5000, 6000, 6000,  7000,
                             7000, 8000, 8000, 9000, 9000, 10000, 10000};
  ut_trs_set_mock_tsc_script(ctx_, script, 21);

  ut_trs_warm_up(ctx_);

  EXPECT_EQ(10u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx_));
}

TEST_F(St20TxRlWarmUpTest, SlowIterationTriggersRecalcButStillReachesTarget) {
  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_target_tsc(ctx_, 10000);
  const uint64_t script[] = {0,    0,    0,    0,    0,    6000,  6000, 7000,
                             7000, 8000, 8000, 9000, 9000, 10000, 10000};
  ut_trs_set_mock_tsc_script(ctx_, script, 15);

  ut_trs_warm_up(ctx_);

  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
  EXPECT_EQ(7u, ut_trs_pad_send_count(ctx_));
  EXPECT_GE(ut_trs_last_tsc(ctx_), 10000u);
}

TEST_F(St20TxRlWarmUpTest, FrozenClockDoesNotOvershootPlannedPadCount) {
  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_target_tsc(ctx_, 10000);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  uint64_t script[33];
  script[0] = 0;
  for (int i = 1; i <= 30; i++) script[i] = 0;
  script[31] = 10000;
  script[32] = 10000;
  ut_trs_set_mock_tsc_script(ctx_, script, 33);

  ut_trs_warm_up(ctx_);

  EXPECT_EQ(10u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, RealisticJitterNeverUndershootsTarget) {
  ut_trs_set_trs(ctx_, 5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  const uint64_t script[] = {
      0,     1829,  3560,  9951,   15866,  24404,  42355,  47957,  66571,  67271,  76362,
      78308, 84468, 85510, 101056, 109671, 111477, 123663, 126314, 138036, 149090,
  };
  ut_trs_set_mock_tsc_script(ctx_, script, 21);

  ut_trs_warm_up(ctx_);

  EXPECT_GT(ut_trs_stat_recalculate_warmup(ctx_), 0u);
  EXPECT_GE(ut_trs_last_tsc(ctx_), 248740u);
}

TEST_F(St20TxRlWarmUpTest, NominalPlanDoesNotQueueRealPacketBeforeTarget) {
  constexpr uint64_t kWarmupEntryTsc = 1000;
  constexpr uint64_t kTargetTsc = 11000;
  uint64_t warmup_script[32];
  std::fill(std::begin(warmup_script), std::end(warmup_script), kWarmupEntryTsc);

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 32);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(kTargetTsc, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(10u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_));

  const uint64_t target_script[] = {kTargetTsc, kTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, target_script, 2);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(11u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kTargetTsc, ut_trs_last_real_send_tsc(ctx_));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
}

TEST_F(St20TxRlWarmUpTest, AlreadyLateTargetQueuesRealPacketImmediately) {
  constexpr uint64_t kTargetTsc = 11000;
  constexpr uint64_t kLateTsc = 12000;
  const uint64_t script[] = {kLateTsc, kLateTsc, kLateTsc, kLateTsc};

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_mock_tsc_script(ctx_, script, 4);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kLateTsc, ut_trs_last_real_send_tsc(ctx_));
}

TEST_F(St20TxRlWarmUpTest, ZeroWarmPacketsWaitsDirectlyForTarget) {
  constexpr uint64_t kEarlyTsc = 1000;
  constexpr uint64_t kTargetTsc = 11000;
  const uint64_t early_script[] = {kEarlyTsc, kEarlyTsc, kEarlyTsc, kEarlyTsc};

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 0);
  ut_trs_set_mock_tsc_script(ctx_, early_script, 4);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_));

  const uint64_t target_script[] = {kTargetTsc, kTargetTsc, kTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, target_script, 3);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(1u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kTargetTsc, ut_trs_last_real_send_tsc(ctx_));
}

TEST_F(St20TxRlWarmUpTest, StateResetsForNextFrame) {
  constexpr uint64_t kFirstWarmupTsc = 1000;
  constexpr uint64_t kFirstTargetTsc = 2000;
  constexpr uint64_t kSecondWarmupTsc = 3000;
  constexpr uint64_t kSecondTargetTsc = 4000;
  uint64_t warmup_script[16];

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 1);
  std::fill(std::begin(warmup_script), std::end(warmup_script), kFirstWarmupTsc);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 16);
  ut_trs_enqueue_first_pkt(ctx_, kFirstTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);
  const uint64_t first_target_script[] = {kFirstTargetTsc, kFirstTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, first_target_script, 2);
  ut_trs_call_rl_tasklet(ctx_);

  std::fill(std::begin(warmup_script), std::end(warmup_script), kSecondWarmupTsc);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 16);
  ut_trs_enqueue_first_pkt(ctx_, kSecondTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);
  const uint64_t second_target_script[] = {kSecondTargetTsc, kSecondTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, second_target_script, 2);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(2u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(4u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(2u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kSecondTargetTsc, ut_trs_last_real_send_tsc(ctx_));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxRlWarmUpTest, FailedPadsRetryBeforeTargetWhileRealPacketWaits) {
  constexpr uint64_t kWarmupTsc = 8000;
  constexpr uint64_t kBeforeTargetTsc = 9000;
  constexpr uint64_t kTargetTsc = 10000;
  const uint64_t initial_script[] = {0};
  const uint64_t warmup_script[] = {kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc,
                                    kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc};
  const uint64_t before_target_script[] = {kBeforeTargetTsc, kBeforeTargetTsc,
                                           kBeforeTargetTsc, kBeforeTargetTsc};
  const uint64_t target_script[] = {kTargetTsc, kTargetTsc};

  ut_trs_set_trs(ctx_, 1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 2);
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  ut_trs_set_mock_tsc_script(ctx_, initial_script, 1);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);

  ut_trs_set_burst_force_fail(ctx_, true);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 8);
  ut_trs_call_rl_tasklet(ctx_);

  ASSERT_EQ(2u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(3u, ut_trs_pad_refcnt(ctx_));
  ASSERT_EQ(1u, ut_trs_inflight_num(ctx_));
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_set_burst_force_fail(ctx_, false);
  ut_trs_set_mock_tsc_script(ctx_, before_target_script, 4);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(2u, ut_trs_pad_send_count(ctx_));
  EXPECT_LT(ut_trs_last_pad_send_tsc(ctx_), kTargetTsc);
  EXPECT_EQ(1u, ut_trs_inflight_num(ctx_));
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_set_mock_tsc_script(ctx_, target_script, 2);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kTargetTsc, ut_trs_last_real_send_tsc(ctx_));
}

TEST_F(St20TxRlWarmUpTest, EmptyClockScriptAdvancesMonotonically) {
  EXPECT_EQ(1u, ut_trs_get_mock_tsc(ctx_));
  EXPECT_EQ(2u, ut_trs_get_mock_tsc(ctx_));
  EXPECT_EQ(3u, ut_trs_get_mock_tsc(ctx_));
}

TEST_F(St20TxRlWarmUpTest, NoPlanPadRetryStaysOnePadPerBurst) {
  ut_trs_set_pad_inflight_num(ctx_, 3);

  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(2u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(2u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_inflight_num(ctx_));
}

/* ports with a plan: pre-arm */

TEST_F(St20TxRlWarmUpTest, PreArmHandsRealPacketOverBehindThePads) {
  constexpr uint64_t kWarmupEntryTsc = 1000;
  constexpr uint64_t kTargetTsc = 31000;
  uint64_t warmup_script[32];
  std::fill(std::begin(warmup_script), std::end(warmup_script), kWarmupEntryTsc);

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 30);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 32);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  /* 30 us to the target: 30 whole pads */
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(30u, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_EQ(kTargetTsc, ut_trs_modeled_launch_tsc(ctx_, kPortP));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxRlWarmUpTest, LaunchLandsOnThePlanForEveryWarmUpPhase) {
  constexpr uint64_t kTargetTsc = 10000000;
  UseRealisticShaper(kPortP);

  /* 97 ns does not divide the pad drain, so the sweep visits every phase of a pad */
  for (uint64_t gap = 97; gap <= kWarmPkts * kTrs; gap += 97) {
    const uint64_t now[] = {kTargetTsc - gap};
    const std::string what = "gap " + std::to_string(gap);
    ut_trs_clear_train(ctx_);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    ut_trs_set_mock_tsc_script(ctx_, now, 1);

    if (gap + kCredit > (kTrimPads - 1) * kDrain) {
      ASSERT_TRUE(ut_trs_pre_arm(ctx_)) << what;
      ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc, kPortP, what);
    } else {
      /* Fewer than trim_pads, the pads enough for a full cut. They would launch packet 0
       * late by what they cannot cut; held, the tasklet sends it at the target, which is
       * tighter. */
      EXPECT_FALSE(ut_trs_pre_arm(ctx_)) << what;
      EXPECT_EQ(0u, ut_trs_train_pads(ctx_, kPortP)) << what;
    }
    if (HasFailure()) return;
  }
}

/* The plan tv_init_pacing() makes on a VF whose shaper clock runs ahead of ice's psm_clk:
 * a retrain programmed less than training measured, and the shaper fills its 2 KB bucket
 * at that programmed rate. 1080p59.94 unless a test plans another format. */
class St20TxRlFastClockTest : public St20TxRlWarmUpTest {
 protected:
  static constexpr uint64_t kTargetTsc = 10000000;

  void SetUp() override {
    St20TxRlWarmUpTest::SetUp();
    ASSERT_NO_FATAL_FAILURE(Plan(1920, 1080, ST_FPS_P59_94, 306359640));
  }

  void Plan(uint32_t width, uint32_t height, enum st_fps fps, uint64_t programmed_bps) {
    const float trained[1] = {164};
    ut_txv_ctx* txv = ut_txv_create();
    ASSERT_NE(nullptr, txv);
    ut_txv_set_rl_format(txv, width, height, false, fps);
    ut_txv_set_rl_queue_bps(txv, kPortP, programmed_bps);
    ASSERT_EQ(0, ut_txv_init_rl_pacing(txv, 1, 0, false, trained, false));
    ASSERT_TRUE(ut_txv_rl_armed(txv, kPortP));
    drain_ = ut_txv_rl_drain(txv, kPortP);
    credit_ = ut_txv_rl_credit(txv, kPortP);
    trs_ = ut_txv_pacing_trs(txv);
    warm_pkts_ = ut_txv_pacing_warm_pkts(txv);
    bucket_ = 2048 * 1e9L / programmed_bps;
    ut_txv_destroy(txv);
    Apply();
  }

  void Apply() {
    ut_trs_set_trs(ctx_, trs_);
    ut_trs_set_warm_pkts_cap(ctx_, warm_pkts_);
    ut_trs_set_drain(ctx_, kPortP, drain_);
    ut_trs_set_credit(ctx_, kPortP, credit_);
    ut_trs_set_shaper_bucket(ctx_, kPortP, bucket_);
  }

  /* the longest timeslot ice programs at these rates: 257 B at the drain training saw */
  long double Aim() const {
    return 257 * drain_ / kPadSize;
  }

  /* pre-arms from gap with that credit; held, none, else with a slot, the launch at each
   * phase of it */
  std::vector<long double> Lates(uint64_t gap, long double credit, long double slot) {
    const uint64_t now[] = {kTargetTsc - gap};
    std::vector<long double> lates;
    ut_trs_set_credit(ctx_, kPortP, credit);
    ut_trs_clear_train(ctx_);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    ut_trs_set_mock_tsc_script(ctx_, now, 1);
    if (!ut_trs_pre_arm(ctx_)) return lates;
    for (long double phase : phases_) {
      ut_trs_set_shaper_timeslot(ctx_, kPortP, slot, phase * slot);
      lates.push_back(ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc);
    }
    ut_trs_set_shaper_timeslot(ctx_, kPortP, 0, 0);
    return lates;
  }

  /* the gaps from tasklet entry to the target that leave the plan its trim pads, or not,
   * across a pad's phases */
  std::vector<uint64_t> Gaps(bool with_trim_pads) const {
    const uint64_t edge = (uint64_t)((kTrimPads - 1) * drain_ - credit_);
    std::vector<uint64_t> gaps;
    for (uint64_t gap = with_trim_pads ? edge + 1 : 997;
         gap <= (with_trim_pads ? warm_pkts_ * trs_ : edge); gap += 997)
      gaps.push_back(gap);
    return gaps;
  }

  /* A frame from the warm-up open whose tasklet is preempted after its first pad burst,
   * until back; a held packet 0 goes when the tasklet is back at the target. Returns the
   * launch past the target. */
  long double PreemptedAfterTheFirstBurst(uint64_t back) {
    const uint64_t entry = kTargetTsc - (uint64_t)(warm_pkts_ * trs_);
    /* the dequeue, the warm-up check, the gap, the first burst's sizing and its return,
     * then the next burst's sizing and on */
    const uint64_t script[] = {entry, entry, entry, entry, entry, back, back};
    const uint64_t at_target[] = {kTargetTsc};
    ut_trs_destroy(ctx_);
    ctx_ = ut_trs_create();
    if (!ctx_) {
      ADD_FAILURE() << "ut_trs_create";
      return -1;
    }
    Apply();
    ut_trs_set_mock_tsc_script(ctx_, script, 7);
    ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
    ut_trs_call_rl_tasklet(ctx_);
    ut_trs_call_rl_tasklet(ctx_);
    ut_trs_set_mock_tsc_script(ctx_, at_target, 1);
    ut_trs_call_rl_tasklet(ctx_);
    return ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
  }

  long double drain_ = 0, credit_ = 0, trs_ = 0, bucket_ = 0;
  uint32_t warm_pkts_ = 0;
  std::vector<long double> phases_ = {0};
};

/* the bucket at the programmed rate holds ~0.7 us more than pad_ns * 2048 / 1262 */
TEST_F(St20TxRlFastClockTest, FluidShaperLaunchesOneAimPastTheTarget) {
  for (uint64_t gap : Gaps(true)) {
    std::vector<long double> late = Lates(gap, credit_, 0);
    ASSERT_EQ(1u, late.size()) << "gap " << gap;
    ExpectOnPlan(late[0] - Aim(), kPortP, "gap " + std::to_string(gap));
    if (HasFailure()) return;
  }
}

/* The shaper releases packet 0 at a token tick, up to a slot before the fluid drain end.
 * Aimed a slot late it is never early at any phase of the grid; without the aim it is. */
TEST_F(St20TxRlFastClockTest, TimeslotGridNeverReleasesPacket0EarlyBehindTheAim) {
  std::mt19937 rng(2110);
  std::uniform_real_distribution<double> phase(0, 1);
  phases_.clear();
  for (int i = 0; i < 64; i++) phases_.push_back(phase(rng));
  long double earliest_without = 0;

  for (uint64_t gap : Gaps(true)) {
    std::vector<long double> lates = Lates(gap, credit_, Aim());
    ASSERT_EQ(phases_.size(), lates.size()) << "gap " << gap;
    for (long double late : lates) EXPECT_GE(late, 0) << "gap " << gap;
    for (long double late : Lates(gap, credit_ - Aim(), Aim()))
      earliest_without = std::min(earliest_without, late);
    if (HasFailure()) return;
  }
  EXPECT_LT(earliest_without, -Aim() / 2);
}

/* A tasklet reaching the pre-arm late leaves fewer than trim_pads, enough to cut a pad:
 * behind them packet 0 would land up to the uncut part of a pad past target + aim. Held,
 * it goes when the tasklet is back at the target. 720p59.94, 8.3 us pads, a retrain at
 * 15/16 of tv_rl_bps(). */
TEST_F(St20TxRlFastClockTest, LateEntryShortOfTheTrimPadsHoldsPacket0ToItsTarget) {
  ASSERT_NO_FATAL_FAILURE(Plan(1280, 720, ST_FPS_P59_94, 136159840));

  for (uint64_t gap : Gaps(false)) {
    const uint64_t entry[] = {kTargetTsc - gap};
    const uint64_t at_target[] = {kTargetTsc};
    ut_trs_destroy(ctx_);
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    Apply();
    ut_trs_set_mock_tsc_script(ctx_, entry, 1);
    ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
    ut_trs_call_rl_tasklet(ctx_);
    ut_trs_call_rl_tasklet(ctx_);
    ut_trs_set_mock_tsc_script(ctx_, at_target, 1);
    ut_trs_call_rl_tasklet(ctx_);

    long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
    EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_)) << "gap " << gap << " late " << late;
    EXPECT_EQ(0, late) << "gap " << gap << " past target + aim by " << late - Aim();
    if (HasFailure()) return;
  }
}

/* Preempted between two pad bursts, the tasklet comes back 8 us before the target: the
 * queued pads drained, and fewer than trim_pads fit the rest. Behind them packet 0 would
 * land late by what they cannot cut; held, it goes at the target behind the drained pads.
 */
TEST_F(St20TxRlFastClockTest, PreemptedBetweenBurstsShortOfTheTrimPadsHolds) {
  long double late = PreemptedAfterTheFirstBurst(kTargetTsc - 8000);

  EXPECT_EQ(32u, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
  EXPECT_EQ(0, late) << "past target + aim by " << late - Aim();
}

/* Back from the preemption with the rest of the drain at trim_pads - 1 pads or less, the
 * pre-arm holds; a ns earlier the rest takes trim_pads, and the plan still lands. */
TEST_F(St20TxRlFastClockTest, IdleShaperKeepsThePlanWhileTheTrimPadsFit) {
  const uint64_t held = (uint64_t)ceill(kTargetTsc + credit_ - (kTrimPads - 1) * drain_);

  long double late = PreemptedAfterTheFirstBurst(held);
  EXPECT_EQ(0, late) << "past target + aim by " << late - Aim();
  EXPECT_EQ(32u, ut_trs_train_pads(ctx_, kPortP));

  late = PreemptedAfterTheFirstBurst(held - 1);
  ExpectOnPlan(late - Aim(), kPortP, "back at " + std::to_string(held - 1));
  EXPECT_EQ(32u + kTrimPads, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
}

/* An interrupt or a preemption inside a pad burst, a timer tick or longer. Before the
 * doorbell, the pads reach the shaper late; after it, only the tasklet is late. The tsc
 * reads around the burst cannot tell which, and packet 0 must leave in
 * [target, target + one pad) either way. */
TEST_F(St20TxRlWarmUpTest, Packet0LeavesOnTargetAfterAStallInAPadBurst) {
  for (uint32_t call : {1, 2}) {
    for (uint64_t stall : {10000, 50000, 150000}) {
      for (bool after_doorbell : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(RunFrameWithStall(call, stall, after_doorbell));

        /* on the plan, or held: then the tasklet sends it at the target, at the latest
         * when the pads drain on the plan */
        long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kStallTargetTsc;
        EXPECT_GE(late, 0) << "call " << call << " stall " << stall << " after "
                           << after_doorbell;
        EXPECT_LT(late, ut_trs_fine_trim_ns(ctx_, kPortP))
            << "call " << call << " stall " << stall << " after " << after_doorbell;
      }
    }
  }
}

/* Behind the first burst the shaper is busy ~112 us: a shorter stall in the next one
 * changes nothing, and packet 0 still goes behind the pads, not held for the tasklet. */
TEST_F(St20TxRlWarmUpTest, StallTheQueuedPadsCoverKeepsThePreArm) {
  for (uint64_t stall : {10000, 50000}) {
    for (bool after_doorbell : {false, true}) {
      ASSERT_NO_FATAL_FAILURE(RunFrameWithStall(2, stall, after_doorbell));

      EXPECT_EQ(1u, queued_by_pre_arm_)
          << "stall " << stall << " after " << after_doorbell;
      EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx_))
          << "stall " << stall << " after " << after_doorbell;
    }
  }
}

TEST_F(St20TxRlWarmUpTest, PlanUsesTrainedDrainPeriod) {
  constexpr uint64_t kTargetTsc = 1000000;
  constexpr uint32_t kWindowPkts = 124;
  const uint64_t now[] = {kTargetTsc - (uint64_t)(kWindowPkts * kTrs)};

  ut_trs_set_trs(ctx_, kTrs);
  ut_trs_set_warm_pkts_cap(ctx_, kWindowPkts);
  ut_trs_set_drain(ctx_, kPortP, kDrain);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  /* 124 trs drain in 124.76 pads at pad_interval 164 */
  EXPECT_EQ(125u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, PlanAddsBurstCredit) {
  constexpr uint64_t kTargetTsc = 40000;
  const uint64_t now[] = {10000};

  SetTrs(1000.0L);
  ut_trs_set_credit(ctx_, kPortP, 2000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 30);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  /* 30 us to the target and 2 us of credit */
  EXPECT_EQ(32u, ut_trs_pad_send_count(ctx_));
}

/* 29.625 pads to the target: 30 pads, 0.375 of a pad cut from the last 27, 472 of its
 * 1262 bytes, 7 coarse cuts and 3 fine ones */
TEST_F(St20TxRlWarmUpTest, FractionOfAPadIsCutFromTheLastPads) {
  constexpr uint64_t kTargetTsc = 40000;
  const uint64_t now[] = {kTargetTsc - 29625, kTargetTsc - 29625, kTargetTsc - 29625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 30);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 3);

  ut_trs_pre_arm(ctx_);

  ASSERT_EQ(30u, ut_trs_pad_send_count(ctx_));
  EXPECT_NEAR((double)(kTargetTsc + 375 - 472 * 1000.0L / kPadSize),
              (double)ut_trs_modeled_launch_tsc(ctx_, kPortP), 1e-6);
  EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0));
  EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1));
}

/* refused pads retry as NORMAL ones, cut or not, later and never early */
TEST_F(St20TxRlWarmUpTest, RefusedCutPadsRetryWithoutLeaking) {
  constexpr uint64_t kTargetTsc = 40000;
  const uint64_t now[] = {kTargetTsc - 29625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 30);
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);
  ut_trs_set_burst_force_fail(ctx_, true);

  ut_trs_pre_arm(ctx_);

  ASSERT_EQ(30u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(31u, ut_trs_pad_refcnt(ctx_));
  ASSERT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0));
  ASSERT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1));

  ut_trs_set_burst_force_fail(ctx_, false);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(30u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, UhdBurstGapsKeepThePlan) {
  /* 2160p59 GPM: 17280 pkts, pad_interval 164, patched 2 KB bucket */
  constexpr long double kUhdTrs = 16683333.333L * 1080 / 1125 / 17280;
  constexpr long double kUhdDrain = kUhdTrs * 164 / 165;
  constexpr uint64_t kTargetTsc = 10000000;
  const uint64_t gaps[] = {200, 250, 1000, 300};
  uint64_t script[64];

  ut_trs_set_trs(ctx_, kUhdTrs);
  ut_trs_set_warm_pkts_cap(ctx_, kWarmPkts);
  ut_trs_set_drain(ctx_, kPortP, kUhdDrain);
  ut_trs_set_credit(ctx_, kPortP, kUhdDrain * 2048 / kPadSize);
  /* a busy but healthy tasklet: each burst returns 0.2-1 us after its tsc read, longer
   * than a pad, and the next read follows 250-300 ns later */
  script[0] = kTargetTsc - 118325; /* a plan of 130.06 pads */
  for (int i = 1; i < 64; i++) script[i] = script[i - 1] + gaps[i % 4];
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, script, 64);

  EXPECT_TRUE(ut_trs_pre_arm(ctx_));

  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx_));
  ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc, kPortP, "");
}

TEST_F(St20TxRlWarmUpTest, SlowBatchHoldsPacket0ForTheTarget) {
  SetTrs(1000.0L);
  ut_trs_set_target_tsc(ctx_, 64000);
  /* the first 32-pad batch returns 48.3 us late: its pads may drain until 80.3 us */
  const uint64_t script[] = {0, 0, 48300, 48300, 48400, 48500};
  ut_trs_set_mock_tsc_script(ctx_, script, 6);

  EXPECT_FALSE(ut_trs_pre_arm(ctx_));

  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
  EXPECT_EQ(32u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_burst_call_count(ctx_));
}

/* 50 pads to the target, the first burst the 23 ahead of the 27 trim_pads. 16 are left
 * after the stall, too few: held, packet 0 goes at the target behind the 23, drained
 * 134 us before it */
TEST_F(St20TxRlWarmUpTest, JitteredBatchesStillRecalculate) {
  constexpr uint64_t kTargetTsc = 248740;
  SetTrs(5000.0L);
  /* the dequeue and the warm-up check, then the tasklet stalls 170 us after the first
   * batch returned */
  const uint64_t script[] = {0, 0, 0, 0, 1829, 170000, 171829, 183663};
  const uint64_t at_target[] = {kTargetTsc};
  ut_trs_set_mock_tsc_script(ctx_, script, 8);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_set_mock_tsc_script(ctx_, at_target, 1);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_GT(ut_trs_stat_recalculate_warmup(ctx_), 0u);
  EXPECT_EQ(23u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(0, ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc);
}

TEST_F(St20TxRlWarmUpTest, StallPastTheTargetEndsThePadTrain) {
  SetTrs(5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  /* the tasklet comes back from the first batch 150 us after the target */
  const uint64_t script[] = {0, 0, 1829, 400000};
  ut_trs_set_mock_tsc_script(ctx_, script, 4);

  ut_trs_pre_arm(ctx_);

  /* 50 pads to the target, the first burst all but the 27 of the cut */
  EXPECT_EQ(23u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, BatchedPadTrainBurstCount) {
  constexpr uint64_t kTargetTsc = 200000;
  const uint64_t now[] = {kTargetTsc - 99625}; /* 99.625 pads */

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 100);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  /* 99.625 pads to the target: 32 + 32 + 9, then the 27 the cut is spread over */
  EXPECT_EQ(100u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(4u, ut_trs_burst_call_count(ctx_));
  ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc, kPortP, "");
}

TEST_F(St20TxRlWarmUpTest, PartiallyAcceptedBatchesRetryInBatches) {
  constexpr uint64_t kTargetTsc = 200000;
  const uint64_t now[] = {kTargetTsc - 99625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 100);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);
  ut_trs_set_burst_accept_limit(ctx_, 20);

  ut_trs_pre_arm(ctx_);

  /* 12 + 12 + 0 + 7 of the 32 + 32 + 9 + 27 pads refused, the cut ones first */
  ASSERT_EQ(31u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(32u, ut_trs_pad_refcnt(ctx_));
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  ut_trs_set_burst_accept_limit(ctx_, 0);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(bursts + 1, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(100u, ut_trs_pad_send_count(ctx_));
}

/* 99.625 pads in bursts of 32 + 32 + 9 + 27, the last 7 coarse, 3 fine and 17 NORMAL */
TEST_F(St20TxRlWarmUpTest, PartlyRefusedCutPadsMoveTheirRefsToTheNormalPad) {
  constexpr uint64_t kTargetTsc = 200000;
  const uint64_t now[] = {kTargetTsc - 99625};
  const unsigned int bursts[] = {32, 32, 9, 27};

  for (uint16_t limit = 1; limit <= 28; limit++) {
    unsigned int refused = 0;
    for (unsigned int n : bursts) refused += n > limit ? n - limit : 0;

    ut_trs_destroy(ctx_);
    ctx_ = ut_trs_create();
    ASSERT_NE(nullptr, ctx_);
    SetTrs(1000.0L);
    ut_trs_set_warm_pkts_cap(ctx_, 100);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    ut_trs_set_mock_tsc_script(ctx_, now, 1);
    ut_trs_set_burst_accept_limit(ctx_, limit);

    ut_trs_pre_arm(ctx_);

    ASSERT_EQ(4u, ut_trs_burst_call_count(ctx_)) << "limit " << limit;
    EXPECT_EQ(refused, ut_trs_pad_inflight_num(ctx_)) << "limit " << limit;
    EXPECT_EQ(1u + refused, ut_trs_pad_refcnt(ctx_)) << "limit " << limit;
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 0)) << "limit " << limit;
    EXPECT_EQ(1u, ut_trs_trim_pad_refcnt(ctx_, 1)) << "limit " << limit;
    if (HasFailure()) return;
  }
}

/* every refcnt atomic between the plan's tsc read and the doorbell delays packet 0 */
TEST_F(St20TxRlWarmUpTest, PreArmTakesOneRefUpdatePerPadMbufBeforeTheDoorbell) {
  constexpr uint64_t kTargetTsc = 200000;
  const uint64_t now[] = {kTargetTsc - 99625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 100);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  ASSERT_EQ(4u, ut_trs_burst_call_count(ctx_));
  /* the NORMAL pad and the two trim pads */
  EXPECT_EQ(3u, ut_trs_max_refcnt_updates_before_burst(ctx_));
}

TEST_F(St20TxRlWarmUpTest, PreArmRealisticJitterLandsOnThePlan) {
  SetTrs(5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  const uint64_t script[] = {
      0,     1829,  3560,  9951,   15866,  24404,  42355,  47957,  66571,  67271,  76362,
      78308, 84468, 85510, 101056, 109671, 111477, 123663, 126314, 138036, 149090,
  };
  ut_trs_set_mock_tsc_script(ctx_, script, 21);

  ut_trs_pre_arm(ctx_);

  ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortP) - 248740, kPortP, "");
}

TEST_F(St20TxRlWarmUpTest, GapBeyondWindowFallsBackToHold) {
  constexpr uint64_t kTargetTsc = 11000;
  const uint64_t initial_script[] = {0};
  /* the warm-up gate passes, then the clock reads earlier than the window */
  const uint64_t early_script[] = {1000, 500, 500, 500};
  const uint64_t target_script[] = {kTargetTsc, kTargetTsc, kTargetTsc};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_mock_tsc_script(ctx_, initial_script, 1);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);

  ut_trs_set_mock_tsc_script(ctx_, early_script, 4);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(1u, ut_trs_stat_troffset_mismatch(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_set_mock_tsc_script(ctx_, target_script, 3);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kTargetTsc, ut_trs_last_real_send_tsc(ctx_));
}

/* a target the plan reaches just after it passed: no pads, the burst credit would only
 * delay packet 0, which WAIT_TARGET sends at once */
TEST_F(St20TxRlWarmUpTest, TargetJustPassedSendsNoPads) {
  constexpr uint64_t kTargetTsc = 10000000;
  const uint64_t now[] = {kTargetTsc + 1000};
  UseRealisticShaper(kPortP);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  EXPECT_FALSE(ut_trs_pre_arm(ctx_));

  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(0u, ut_trs_stat_troffset_mismatch(ctx_));
}

/* two pkt times or more past, as video_trs_rl_warm_up() counts it: a mismatch */
TEST_F(St20TxRlWarmUpTest, TargetTwoPktTimesPastIsAMismatch) {
  constexpr uint64_t kTargetTsc = 10000000;
  const uint64_t now[] = {kTargetTsc + (uint64_t)(2 * kTrs)};
  UseRealisticShaper(kPortP);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  EXPECT_FALSE(ut_trs_pre_arm(ctx_));

  EXPECT_EQ(0u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_stat_troffset_mismatch(ctx_));
}

TEST_F(St20TxRlWarmUpTest, RedundantLegsLaunchTogetherThoughServedApart) {
  constexpr uint64_t kTargetTsc = 10000000;
  constexpr long double kDrainR = kTrs * 170 / 171;
  const uint64_t p_now[] = {kTargetTsc - 400000};
  const uint64_t r_now[] = {kTargetTsc - 300013};

  UseRealisticShaper(kPortP);
  ut_trs_set_drain(ctx_, kPortR, kDrainR);
  ut_trs_set_credit(ctx_, kPortR, kDrainR * 2048 / kPadSize);
  ut_trs_set_target_tsc_port(ctx_, kPortP, kTargetTsc);
  ut_trs_set_target_tsc_port(ctx_, kPortR, kTargetTsc);

  ut_trs_set_mock_tsc_script(ctx_, p_now, 1);
  ut_trs_pre_arm_port(ctx_, kPortP);
  ut_trs_set_mock_tsc_script(ctx_, r_now, 1);
  ut_trs_pre_arm_port(ctx_, kPortR);

  ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc, kPortP, "P");
  ExpectOnPlan(ut_trs_modeled_launch_tsc(ctx_, kPortR) - kTargetTsc, kPortR, "R");
}

TEST_F(St20TxRlWarmUpTest, PreArmStateResetsForNextFrame) {
  constexpr uint64_t kFirstWarmupTsc = 1000;
  constexpr uint64_t kFirstTargetTsc = 28000;
  constexpr uint64_t kSecondWarmupTsc = 30000;
  constexpr uint64_t kSecondTargetTsc = 57000;
  uint64_t warmup_script[16];

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 27);
  std::fill(std::begin(warmup_script), std::end(warmup_script), kFirstWarmupTsc);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 16);
  ut_trs_enqueue_first_pkt(ctx_, kFirstTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  ut_trs_clear_train(ctx_);
  std::fill(std::begin(warmup_script), std::end(warmup_script), kSecondWarmupTsc);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 16);
  ut_trs_enqueue_first_pkt(ctx_, kSecondTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  /* 27 us to each target: the 27 trim_pads, enough for any cut, one burst a frame */
  EXPECT_EQ(54u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(4u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(2u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(kSecondTargetTsc, ut_trs_modeled_launch_tsc(ctx_, kPortP));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxRlWarmUpTest, FailedPadsRetryAheadOfRealPacket) {
  constexpr uint64_t kWarmupTsc = 8000;
  constexpr uint64_t kBeforeTargetTsc = 35000;
  constexpr uint64_t kTargetTsc = 36000;
  const uint64_t initial_script[] = {0};
  const uint64_t warmup_script[] = {kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc,
                                    kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc};
  const uint64_t before_target_script[] = {kBeforeTargetTsc, kBeforeTargetTsc,
                                           kBeforeTargetTsc, kBeforeTargetTsc};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 28);
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  ut_trs_set_mock_tsc_script(ctx_, initial_script, 1);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);
  ut_trs_call_rl_tasklet(ctx_);

  ut_trs_set_burst_force_fail(ctx_, true);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 8);
  ut_trs_call_rl_tasklet(ctx_);

  /* 28 us to the target: 28 pads */
  ASSERT_EQ(28u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(29u, ut_trs_pad_refcnt(ctx_));
  ASSERT_EQ(1u, ut_trs_inflight_num(ctx_));
  ASSERT_EQ(0u, ut_trs_real_send_count(ctx_));

  ut_trs_set_burst_force_fail(ctx_, false);
  ut_trs_set_mock_tsc_script(ctx_, before_target_script, 4);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(28u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(28u, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), kTargetTsc);
}

}  // namespace
