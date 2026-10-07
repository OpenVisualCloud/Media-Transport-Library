/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

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

  ut_trs_ctx* ctx_ = nullptr;
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kTargetTsc);
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kLateTsc);
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kTargetTsc);
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kSecondTargetTsc);
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kTargetTsc);
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
  constexpr uint64_t kTargetTsc = 11000;
  uint64_t warmup_script[32];
  std::fill(std::begin(warmup_script), std::end(warmup_script), kWarmupEntryTsc);

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_mock_tsc_script(ctx_, warmup_script, 32);
  ut_trs_enqueue_first_pkt(ctx_, kTargetTsc);

  ut_trs_call_rl_tasklet(ctx_);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(10u, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_EQ((long double)kTargetTsc, ut_trs_modeled_launch_tsc(ctx_, kPortP));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxRlWarmUpTest, LaunchLandsOnTargetForEveryWarmUpPhase) {
  constexpr uint64_t kTargetTsc = 10000000;
  UseRealisticShaper(kPortP);

  /* 97 ns does not divide the pad drain, so the sweep visits every phase of a pad */
  for (uint64_t gap = 97; gap <= kWarmPkts * kTrs; gap += 97) {
    const uint64_t now[] = {kTargetTsc - gap};
    ut_trs_clear_train(ctx_);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    ut_trs_set_mock_tsc_script(ctx_, now, 1);

    ut_trs_pre_arm(ctx_);

    long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
    ASSERT_GE(late, 0) << "gap " << gap;
    ASSERT_LT(late, kDrain) << "gap " << gap;
  }
}

/* A stall between the tsc read of the pre-arm and its first pad burst, an interrupt or a
 * preemption, moves the doorbell later. The pads then drain later than the plan, and
 * packet 0, queued behind them, must still leave in [target, target + one pad). */
TEST_F(St20TxRlWarmUpTest, LaunchStaysOnTargetWhenTheFirstPadBurstStalls) {
  constexpr uint64_t kTargetTsc = 10000000;
  const uint64_t now[] = {kTargetTsc - (uint64_t)(kWarmPkts * kTrs)};
  UseRealisticShaper(kPortP);

  for (uint64_t stall : {1000, 10000, 50000, 150000}) {
    ut_trs_clear_train(ctx_);
    ut_trs_set_target_tsc(ctx_, kTargetTsc);
    ut_trs_set_mock_tsc_script(ctx_, now, 1);
    ut_trs_set_burst_stall(ctx_, ut_trs_burst_call_count(ctx_) + 1, stall);

    ut_trs_pre_arm(ctx_);

    long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
    EXPECT_GE(late, 0) << "stall " << stall;
    EXPECT_LT(late, kDrain) << "stall " << stall;
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
  constexpr uint64_t kTargetTsc = 20000;
  const uint64_t now[] = {10000};

  SetTrs(1000.0L);
  ut_trs_set_credit(ctx_, kPortP, 2000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  EXPECT_EQ(12u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, FractionOfAPadRoundsUpToAWholePad) {
  constexpr uint64_t kTargetTsc = 20000;
  const uint64_t now[] = {kTargetTsc - 9625, kTargetTsc - 9625, kTargetTsc - 9625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 3);

  ut_trs_pre_arm(ctx_);

  ASSERT_EQ(10u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(kTargetTsc + 375.0L, ut_trs_modeled_launch_tsc(ctx_, kPortP));
}

TEST_F(St20TxRlWarmUpTest, RefusedRoundUpPadRetriesWithoutLeaking) {
  constexpr uint64_t kTargetTsc = 20000;
  const uint64_t now[] = {kTargetTsc - 9625};

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 10);
  ut_trs_set_hang_detect_thresh_ns(ctx_, UINT64_MAX);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);
  ut_trs_set_burst_force_fail(ctx_, true);

  ut_trs_pre_arm(ctx_);

  ASSERT_EQ(10u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(11u, ut_trs_pad_refcnt(ctx_));

  ut_trs_set_burst_force_fail(ctx_, false);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(10u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, UhdBurstGapsKeepThePlan) {
  /* 2160p59 GPM: 17280 pkts, pad_interval 164, patched 2 KB bucket */
  constexpr long double kUhdTrs = 16683333.333L * 1080 / 1125 / 17280;
  constexpr long double kUhdDrain = kUhdTrs * 164 / 165;
  constexpr uint64_t kTargetTsc = 10000000;
  const uint64_t gaps[] = {200, 250, 150, 300};
  uint64_t script[64];

  ut_trs_set_trs(ctx_, kUhdTrs);
  ut_trs_set_warm_pkts_cap(ctx_, kWarmPkts);
  ut_trs_set_drain(ctx_, kPortP, kUhdDrain);
  ut_trs_set_credit(ctx_, kPortP, kUhdDrain * 2048 / kPadSize);
  /* every tsc read 150-300 ns after the previous one, a busy but healthy tasklet */
  script[0] = kTargetTsc - 118325; /* a plan of 130.125 pads */
  for (int i = 1; i < 64; i++) script[i] = script[i - 1] + gaps[i % 4];
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, script, 64);

  ut_trs_pre_arm(ctx_);

  EXPECT_EQ(0u, ut_trs_stat_recalculate_warmup(ctx_));
  long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
  EXPECT_GE(late, 0);
  EXPECT_LT(late, kUhdDrain);
}

TEST_F(St20TxRlWarmUpTest, SlowBatchTriggersRecalcButStillReachesTarget) {
  SetTrs(1000.0L);
  ut_trs_set_target_tsc(ctx_, 64000);
  /* the first 32-pad batch returns after the NIC drained it and went idle, 15.7 pads
   * before the target */
  const uint64_t script[] = {0, 0, 48300, 48300, 48400, 48500};
  ut_trs_set_mock_tsc_script(ctx_, script, 6);

  ut_trs_pre_arm(ctx_);

  EXPECT_EQ(1u, ut_trs_stat_recalculate_warmup(ctx_));
  /* 32 + 15 pads, then one more for the last 0.7 pad */
  EXPECT_EQ(48u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(3u, ut_trs_burst_call_count(ctx_));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), 64000.0L);
}

TEST_F(St20TxRlWarmUpTest, JitteredBatchesStillRecalculate) {
  SetTrs(5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  /* the tasklet stalls 170 us after the first batch */
  const uint64_t script[] = {0, 0, 1829, 170000, 171829, 183663};
  ut_trs_set_mock_tsc_script(ctx_, script, 6);

  ut_trs_pre_arm(ctx_);

  EXPECT_GT(ut_trs_stat_recalculate_warmup(ctx_), 0u);
  long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - 248740;
  EXPECT_GE(late, 0);
  EXPECT_LT(late, 5000);
}

TEST_F(St20TxRlWarmUpTest, StallPastTheTargetEndsThePadTrain) {
  SetTrs(5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  /* the tasklet comes back from the first batch 150 us after the target */
  const uint64_t script[] = {0, 0, 1829, 400000};
  ut_trs_set_mock_tsc_script(ctx_, script, 4);

  ut_trs_pre_arm(ctx_);

  EXPECT_EQ(32u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, BatchedPadTrainBurstCount) {
  constexpr uint64_t kTargetTsc = 200000;
  const uint64_t now[] = {kTargetTsc - 99625}; /* 99.625 pads */

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 100);
  ut_trs_set_target_tsc(ctx_, kTargetTsc);
  ut_trs_set_mock_tsc_script(ctx_, now, 1);

  ut_trs_pre_arm(ctx_);

  EXPECT_EQ(100u, ut_trs_pad_send_count(ctx_));
  /* 32 + 32 + 32 + 3 pads, then one more for the fraction */
  EXPECT_EQ(5u, ut_trs_burst_call_count(ctx_));
  long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
  EXPECT_GE(late, 0);
  EXPECT_LT(late, 1000);
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

  /* 12 + 12 + 12 of the 32 + 32 + 32 + 3 pads refused */
  ASSERT_EQ(36u, ut_trs_pad_inflight_num(ctx_));
  ASSERT_EQ(37u, ut_trs_pad_refcnt(ctx_));
  uint32_t bursts = ut_trs_burst_call_count(ctx_);

  ut_trs_set_burst_accept_limit(ctx_, 0);
  ut_trs_call_rl_tasklet(ctx_);

  EXPECT_EQ(bursts + 2, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(0u, ut_trs_pad_inflight_num(ctx_));
  EXPECT_EQ(1u, ut_trs_pad_refcnt(ctx_));
  EXPECT_EQ(100u, ut_trs_pad_send_count(ctx_));
}

TEST_F(St20TxRlWarmUpTest, PreArmRealisticJitterLandsWithinAPad) {
  SetTrs(5000.0L);
  ut_trs_set_target_tsc(ctx_, 248740);
  const uint64_t script[] = {
      0,     1829,  3560,  9951,   15866,  24404,  42355,  47957,  66571,  67271,  76362,
      78308, 84468, 85510, 101056, 109671, 111477, 123663, 126314, 138036, 149090,
  };
  ut_trs_set_mock_tsc_script(ctx_, script, 21);

  ut_trs_pre_arm(ctx_);

  long double late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - 248740;
  EXPECT_GE(late, 0);
  EXPECT_LT(late, 5000);
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
  EXPECT_GE(ut_trs_last_real_send_tsc(ctx_), kTargetTsc);
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

  long double p_late = ut_trs_modeled_launch_tsc(ctx_, kPortP) - kTargetTsc;
  long double r_late = ut_trs_modeled_launch_tsc(ctx_, kPortR) - kTargetTsc;
  EXPECT_GE(p_late, 0);
  EXPECT_GE(r_late, 0);
  EXPECT_LT(p_late, kDrain);
  EXPECT_LT(r_late, kDrainR);
}

TEST_F(St20TxRlWarmUpTest, PreArmStateResetsForNextFrame) {
  constexpr uint64_t kFirstWarmupTsc = 1000;
  constexpr uint64_t kFirstTargetTsc = 2000;
  constexpr uint64_t kSecondWarmupTsc = 3000;
  constexpr uint64_t kSecondTargetTsc = 4000;
  uint64_t warmup_script[16];

  SetTrs(1000.0L);
  ut_trs_set_warm_pkts_cap(ctx_, 1);
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

  EXPECT_EQ(2u, ut_trs_pad_send_count(ctx_));
  EXPECT_EQ(4u, ut_trs_burst_call_count(ctx_));
  EXPECT_EQ(2u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ((long double)kSecondTargetTsc, ut_trs_modeled_launch_tsc(ctx_, kPortP));
  EXPECT_EQ(0u, ut_trs_target_tsc(ctx_));
  EXPECT_EQ(0, ut_trs_rl_state(ctx_));
}

TEST_F(St20TxRlWarmUpTest, FailedPadsRetryAheadOfRealPacket) {
  constexpr uint64_t kWarmupTsc = 8000;
  constexpr uint64_t kBeforeTargetTsc = 9000;
  constexpr uint64_t kTargetTsc = 10000;
  const uint64_t initial_script[] = {0};
  const uint64_t warmup_script[] = {kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc,
                                    kWarmupTsc, kWarmupTsc, kWarmupTsc, kWarmupTsc};
  const uint64_t before_target_script[] = {kBeforeTargetTsc, kBeforeTargetTsc,
                                           kBeforeTargetTsc, kBeforeTargetTsc};

  SetTrs(1000.0L);
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
  EXPECT_EQ(1u, ut_trs_real_send_count(ctx_));
  EXPECT_EQ(2u, ut_trs_train_pads(ctx_, kPortP));
  EXPECT_GE(ut_trs_modeled_launch_tsc(ctx_, kPortP), (long double)kTargetTsc);
}

}  // namespace
