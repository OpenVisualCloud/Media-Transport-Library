/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "session/st_video_transmitter_harness.h"

namespace {

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

}  // namespace
