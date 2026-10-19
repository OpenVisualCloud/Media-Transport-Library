/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The cumulative tasklet counters that the USDT probe sys:tasklet_stat reports. They
 * move only while the time is measured or sampled, survive the periodic stats dump,
 * and start at zero for every tasklet registered.
 */

#include <gtest/gtest.h>
#include <mtl_api.h>

#include "sch/mt_sch_harness.h"

static const unsigned int kWorkEvery = 4;
static const uint64_t kCalls = 20 * (uint64_t)kWorkEvery;
static const uint64_t kWorkCalls = kCalls / kWorkEvery;
static const uint64_t kWorkSum = kWorkCalls * UT_SCH_WORK_NS;
static const uint64_t kSum = (kCalls - kWorkCalls) * UT_SCH_IDLE_NS + kWorkSum;
static const int kFillerSlot = 0;
static const int kTaskletSlot = 1;
static const int kIdleSlot = 2;
/* MT_SCH_TIME_SAMPLE_PERIOD: loops per timed loop, on average, while sampled */
static const uint64_t kSamplePeriod = 64;
static const uint64_t kSampleCalls = 256 * kSamplePeriod;

class MtSchStatsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
  }

  void TearDown() override {
    if (sampling_) ut_sch_stats_sample(false);
    if (mt_) ut_sch_stats_main_destroy(mt_);
  }

  void Sample() {
    ut_sch_stats_sample(true);
    sampling_ = true;
  }

  void Run(uint64_t flags, unsigned int work_every, uint64_t calls) {
    mt_ = ut_sch_stats_main_create(flags);
    ASSERT_NE(mt_, nullptr);
    ASSERT_EQ(ut_sch_stats_register(mt_, work_every), 0);
    ASSERT_EQ(ut_sch_stats_run(mt_, calls), 0);
  }

  void RunMeasured() {
    Run(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery, kCalls);
  }

  mtl_handle mt_ = nullptr;
  bool sampling_ = false;
};

TEST_F(MtSchStatsTest, CountersStayZeroWhileNothingTimes) {
  Run(0, kWorkEvery, kCalls);

  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.cnt, 0u);
  EXPECT_EQ(t.work_cnt, 0u);
}

TEST_F(MtSchStatsTest, MeasureTimesEveryCall) {
  RunMeasured();

  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.cnt, kCalls);
  EXPECT_EQ(t.sum, kSum);
}

TEST_F(MtSchStatsTest, WorkCountsOnlyTheCallsThatReportPending) {
  RunMeasured();

  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.work_cnt, kWorkCalls);
  EXPECT_EQ(t.work_sum, kWorkSum);
}

TEST_F(MtSchStatsTest, SampleTimesSomeLoopsAndCountsEveryCall) {
  Sample();
  Run(0, 0, kSampleCalls);

  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  /* the last timed call stands for a gap that can outrun the loops the scheduler made */
  EXPECT_GE(t.cnt, kSampleCalls);
  EXPECT_LT(t.cnt, kSampleCalls + 2 * kSamplePeriod);
  /* every call idle, so the weighted estimate is exact */
  EXPECT_EQ(t.sum, t.cnt * UT_SCH_IDLE_NS);
  /* the stats dump reports full measurements only */
  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
}

TEST(MtSchSampleGap, GapsSpanOneToTwiceThePeriodAndAverageIt) {
  uint32_t state = 2463534242u;
  uint32_t lo = UINT32_MAX, hi = 0;
  uint64_t sum = 0;
  const uint64_t draws = 65536;
  for (uint64_t i = 0; i < draws; i++) {
    uint32_t gap = ut_sch_time_sample_gap(&state);
    lo = gap < lo ? gap : lo;
    hi = gap > hi ? gap : hi;
    sum += gap;
  }
  EXPECT_EQ(lo, 1u);
  EXPECT_EQ(hi, 2 * kSamplePeriod - 1);
  double mean = (double)sum / (double)draws;
  EXPECT_GT(mean, (double)kSamplePeriod - 1);
  EXPECT_LT(mean, (double)kSamplePeriod + 1);
}

TEST_F(MtSchStatsTest, SampleEstimatesTheWorkShare) {
  /* work on every 4th call divides the period: a sample every 64th loop would see only
   * work calls or none */
  Sample();
  Run(0, kWorkEvery, kSampleCalls);

  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  double share = (double)t.work_cnt / (double)t.cnt;
  EXPECT_GT(share, 0.15);
  EXPECT_LT(share, 0.35);
  EXPECT_EQ(t.work_sum, t.work_cnt * UT_SCH_WORK_NS);
}

TEST_F(MtSchStatsTest, MeasureWinsOverSample) {
  Sample();
  RunMeasured();

  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), kCalls);
  EXPECT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), kCalls);
  EXPECT_EQ(ut_sch_stats_counter(kTaskletSlot).cnt, kCalls);
}

TEST_F(MtSchStatsTest, RegisterClearsOnlyTheCountersOfTheReusedSlot) {
  RunMeasured();
  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  ASSERT_GT(t.cnt, 0u);
  ASSERT_GT(t.work_cnt, 0u);

  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery), 0);
  t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.cnt, 0u);
  EXPECT_EQ(t.work_cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counter(kFillerSlot).cnt, kCalls);
}

TEST_F(MtSchStatsTest, RegisterBelowATaskletKeepsItRunning) {
  mt_ = ut_sch_stats_main_create(MTL_FLAG_TASKLET_TIME_MEASURE);
  ASSERT_NE(mt_, nullptr);
  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery), 0);
  ASSERT_EQ(ut_sch_stats_register_idle(mt_), 0);
  /* back into slot 1, below ut_idle */
  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery), 0);
  ASSERT_EQ(ut_sch_stats_run(mt_, kCalls), 0);

  EXPECT_EQ(ut_sch_stats_counter(kIdleSlot).cnt, kCalls);
}

TEST_F(MtSchStatsTest, TheStatsDumpHandsTheCountersToTheProbe) {
  RunMeasured();
  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);

  struct ut_sch_tasklet_stat p = ut_sch_stats_probe();
  EXPECT_EQ(p.sch_idx, UT_SCH_STATS_SCH);
  EXPECT_STREQ(p.name, "ut_tasklet");
  EXPECT_EQ(p.cnt, kCalls);
  EXPECT_EQ(p.sum, kSum);
  EXPECT_EQ(p.work_cnt, kWorkCalls);
  EXPECT_EQ(p.work_sum, kWorkSum);
  struct ut_sch_tasklet_stat t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.cnt, kCalls);
  EXPECT_EQ(t.sum, kSum);
  EXPECT_EQ(t.work_cnt, kWorkCalls);
  EXPECT_EQ(t.work_sum, kWorkSum);
}

TEST_F(MtSchStatsTest, TheStatsDumpResetsThePerDumpTimes) {
  RunMeasured();
  ASSERT_EQ(ut_sch_stats_dump_loop_cnt(mt_), kCalls);
  ASSERT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), kCalls);

  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);
  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
}
