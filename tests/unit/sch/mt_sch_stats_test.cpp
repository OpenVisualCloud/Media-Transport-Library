/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The cumulative tasklet counters of the USDT probe sys:tasklet_stat: exact while the
 * time is measured, sampled while only the probe is attached, zero otherwise.
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "sch/mt_sch_harness.h"

namespace {

const unsigned int kWorkEvery = 4;
const uint64_t kCalls = 20 * kWorkEvery;
const uint64_t kWorkCalls = kCalls / kWorkEvery;
const uint64_t kWorkSum = kWorkCalls * UT_SCH_WORK_NS;
const uint64_t kSum = (kCalls - kWorkCalls) * UT_SCH_IDLE_NS + kWorkSum;
const uint64_t kSamplePeriod = ut_sch_time_sample_period();
const uint64_t kSampleCalls = 256 * kSamplePeriod;

class MtSchStatsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
  }

  void TearDown() override {
    ut_sch_stats_attach(false);
    if (s_) ut_sch_stats_destroy(s_);
  }

  /* with_idle: ut_idle in slot 1, after ut_tasklet */
  void Run(uint64_t flags, unsigned int work_every, uint64_t calls,
           bool with_idle = false) {
    s_ = ut_sch_stats_create(flags, work_every);
    ASSERT_NE(s_, nullptr);
    if (with_idle) {
      ASSERT_EQ(ut_sch_stats_register_idle(s_), 0);
    }
    ASSERT_EQ(ut_sch_stats_run(s_, calls), 0);
  }

  struct ut_sch_stats* s_ = nullptr;
};

TEST_F(MtSchStatsTest, MeasureCountsEveryCallAndTheDumpReportsThem) {
  ut_sch_stats_attach(true);
  ASSERT_NO_FATAL_FAILURE(Run(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery, kCalls));
  /* the measure wins over the sample the attached probe asks for */
  EXPECT_EQ(ut_sch_stats_measured(s_), kCalls);

  struct ut_sch_tasklet_stat p = ut_sch_stats_dump(s_);
  EXPECT_EQ(p.sch_idx, UT_SCH_STATS_IDX);
  EXPECT_STREQ(p.name, "ut_tasklet");
  EXPECT_EQ(p.cnt, kCalls);
  EXPECT_EQ(p.sum_ns, kSum);
  EXPECT_EQ(p.work_cnt, kWorkCalls);
  EXPECT_EQ(p.work_sum_ns, kWorkSum);
  /* cumulative: the dump leaves them */
  EXPECT_EQ(ut_sch_stats_counters(0).cnt, kCalls);
  EXPECT_EQ(ut_sch_stats_counters(0).work_sum_ns, kWorkSum);
}

TEST_F(MtSchStatsTest, CountersStayZeroWhileNothingTimes) {
  ASSERT_NO_FATAL_FAILURE(Run(0, kWorkEvery, kCalls));

  EXPECT_EQ(ut_sch_stats_counters(0).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counters(0).sum_ns, 0u);
}

TEST_F(MtSchStatsTest, SampleCountsEveryCallToWithinAGap) {
  ut_sch_stats_attach(true);
  ASSERT_NO_FATAL_FAILURE(Run(0, 0, kSampleCalls));

  struct ut_sch_tasklet_stat c = ut_sch_stats_counters(0);
  /* the last sampled loop stands for a gap that can outrun the run */
  EXPECT_GE(c.cnt, kSampleCalls);
  EXPECT_LT(c.cnt, kSampleCalls + 2 * kSamplePeriod);
  /* every call idle, so the weighted sum is exact */
  EXPECT_EQ(c.sum_ns, c.cnt * UT_SCH_IDLE_NS);
  /* the per-dump time lines stay off */
  EXPECT_EQ(ut_sch_stats_measured(s_), 0u);
}

TEST_F(MtSchStatsTest, SampleEstimatesTheWorkShare) {
  /* work every 4th call divides the period, so a fixed gap of 64 would see one phase */
  ut_sch_stats_attach(true);
  ASSERT_NO_FATAL_FAILURE(Run(0, kWorkEvery, kSampleCalls));

  struct ut_sch_tasklet_stat c = ut_sch_stats_counters(0);
  EXPECT_NEAR((double)c.work_cnt / (double)c.cnt, 1.0 / kWorkEvery, 0.1);
  EXPECT_EQ(c.work_sum_ns, c.work_cnt * UT_SCH_WORK_NS);
}

TEST_F(MtSchStatsTest, WorkIsPerCallNotPerLoop) {
  ASSERT_NO_FATAL_FAILURE(Run(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery, kCalls, true));

  /* ut_idle also runs right after the calls of ut_tasklet that worked */
  EXPECT_EQ(ut_sch_stats_counters(1).work_cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counters(0).work_cnt, kWorkCalls);
}

TEST_F(MtSchStatsTest, RegisterClearsOnlyTheCountersOfTheReusedSlot) {
  ASSERT_NO_FATAL_FAILURE(Run(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery, kCalls, true));
  ASSERT_EQ(ut_sch_stats_counters(1).cnt, kCalls);

  ASSERT_EQ(ut_sch_stats_register_idle(s_), 0);
  EXPECT_EQ(ut_sch_stats_counters(1).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counters(0).cnt, kCalls);
}

TEST_F(MtSchStatsTest, ARequestedSchCountsInEverySlotUntilFreed) {
  s_ = ut_sch_stats_request(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  ASSERT_NE(s_, nullptr);
  ASSERT_EQ(ut_sch_stats_run(s_, kCalls), 0);

  EXPECT_EQ(ut_sch_stats_counters(0).work_cnt, kWorkCalls);
  EXPECT_EQ(ut_sch_stats_counters(UT_SCH_REQUEST_TASKLETS - 1).cnt, kCalls);
  /* glibc aborts this free if the loop wrote past the counters sch_request() sized */
  EXPECT_EQ(ut_sch_stats_destroy(s_), 0);
  s_ = nullptr;
}

TEST(MtSchTimeSampleGap, GapsSpanOneToTwiceThePeriodAndAverageIt) {
  uint32_t state = ut_sch_time_sample_seed();
  uint32_t lo = UINT32_MAX, hi = 0;
  uint64_t sum = 0;
  const int draws = 65536;
  for (int i = 0; i < draws; i++) {
    uint32_t gap = ut_sch_time_sample_gap(&state);
    lo = std::min(lo, gap);
    hi = std::max(hi, gap);
    sum += gap;
  }
  EXPECT_EQ(lo, 1u);
  EXPECT_EQ(hi, 2 * kSamplePeriod - 1);
  EXPECT_NEAR((double)sum / draws, (double)kSamplePeriod, 1.0);
}

}  // namespace
