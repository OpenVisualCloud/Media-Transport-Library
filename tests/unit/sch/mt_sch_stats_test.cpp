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

/* The spin budget of the one tasklet; the measurement brackets the handler, so every
 * timed call that spins is at least the floor below. */
static const unsigned int kSpinUs = 50;
static const uint64_t kSpinFloorNs = 40000;
static const unsigned int kWorkEvery = 4;
static const int kSpinSlot = 1;
/* MT_SCH_TIME_SAMPLE_PERIOD: loops per timed loop, on average, while sampled */
static const uint64_t kSamplePeriod = 64;

class MtSchStatsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
  }

  void TearDown() override {
    if (started_) ut_sch_stats_stop(mt_);
    if (sampling_) ut_sch_stats_sample(false);
    if (mt_) ut_sch_stats_main_destroy(mt_);
  }

  void Create(uint64_t flags, unsigned int work_every = 0) {
    mt_ = ut_sch_stats_main_create(flags, kSpinUs, work_every);
    ASSERT_NE(mt_, nullptr);
  }

  void Sample() {
    ut_sch_stats_sample(true);
    sampling_ = true;
  }

  void RunCalls(uint64_t calls) {
    ASSERT_EQ(ut_sch_stats_start(mt_), 0);
    started_ = true;
    ASSERT_EQ(ut_sch_stats_wait_calls(mt_, calls), 0);
  }

  void Stop() {
    ASSERT_EQ(ut_sch_stats_stop(mt_), 0);
    started_ = false;
  }

  mtl_handle mt_ = nullptr;
  bool started_ = false;
  bool sampling_ = false;
};

TEST_F(MtSchStatsTest, CountersStayZeroWhileNothingTimes) {
  Create(0, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, false).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, true).cnt, 0u);
}

TEST_F(MtSchStatsTest, MeasureTimesEveryCall) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE);
  RunCalls(20);
  Stop();

  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  EXPECT_EQ(t.cnt, ut_sch_stats_calls(mt_));
  ASSERT_GT(t.cnt, 0u);
  uint64_t avg = t.sum / t.cnt;
  EXPECT_LE(t.min, avg);
  EXPECT_LE(avg, t.max);
  EXPECT_GE(t.min, kSpinFloorNs);
  /* the tasklet reports idle on every call */
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, true).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, true).sum, 0u);
}

TEST_F(MtSchStatsTest, WorkCountsOnlyTheCallsThatReportPending) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  struct ut_sch_stat w = ut_sch_stats_counter(kSpinSlot, true);
  ASSERT_GT(w.cnt, 0u);
  EXPECT_EQ(w.cnt, t.cnt / kWorkEvery);
  EXPECT_GE(w.min, kSpinFloorNs);
  EXPECT_LE(w.sum, t.sum);
  /* the idle calls are in time only, so they pull its min below the spin */
  EXPECT_LT(t.min, kSpinFloorNs);
}

TEST_F(MtSchStatsTest, SampleTimesSomeLoopsAndCountsEveryCall) {
  Create(0);
  Sample();
  RunCalls(kSamplePeriod * 32);
  Stop();

  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  /* every call is counted; the last timed one stands for a gap that can outrun the
   * loops the scheduler made */
  uint64_t calls = ut_sch_stats_calls(mt_);
  EXPECT_GE(t.cnt, calls);
  EXPECT_LT(t.cnt, calls + 2 * kSamplePeriod);
  EXPECT_GE(t.min, kSpinFloorNs);
  uint64_t avg = t.sum / t.cnt;
  EXPECT_LE(t.min, avg);
  EXPECT_LE(avg, t.max);
  /* the stats dump reports full measurements only */
  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
}

TEST_F(MtSchStatsTest, SampleEstimatesTheWorkShare) {
  /* work on every 4th call divides the period: a sample every 64th loop would see only
   * work calls or none */
  Create(0, kWorkEvery);
  Sample();
  RunCalls(kWorkEvery * kSamplePeriod * 64);
  Stop();

  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  struct ut_sch_stat w = ut_sch_stats_counter(kSpinSlot, true);
  ASSERT_GT(t.cnt, 0u);
  double share = (double)w.cnt / t.cnt;
  EXPECT_GT(share, 0.15);
  EXPECT_LT(share, 0.35);
  EXPECT_GE(w.min, kSpinFloorNs);
}

TEST_F(MtSchStatsTest, MeasureWinsOverSample) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE);
  Sample();
  RunCalls(20);
  Stop();

  EXPECT_GT(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_GT(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, false).cnt, ut_sch_stats_calls(mt_));
}

TEST_F(MtSchStatsTest, RegisterClearsTheCountersOfAReusedSlot) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE);
  /* slot 0, the empty one before the spin tasklet */
  ASSERT_EQ(ut_sch_stats_register_busy(mt_), 0);
  RunCalls(20);
  Stop();
  EXPECT_GT(ut_sch_stats_counter(0, false).cnt, 0u);
  EXPECT_GT(ut_sch_stats_counter(0, true).cnt, 0u);

  ASSERT_EQ(ut_sch_stats_register_busy(mt_), 0);
  EXPECT_EQ(ut_sch_stats_counter(0, false).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_counter(0, true).cnt, 0u);
  EXPECT_GT(ut_sch_stats_counter(kSpinSlot, false).cnt, 0u);
}

TEST_F(MtSchStatsTest, DumpResetsOnlyThePerDumpCounters) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  struct ut_sch_stat w = ut_sch_stats_counter(kSpinSlot, true);
  ASSERT_GT(w.cnt, 0u);
  ASSERT_GT(ut_sch_stats_dump_loop_cnt(mt_), 0u);

  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);

  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, false).cnt, t.cnt);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, false).sum, t.sum);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, true).cnt, w.cnt);
  EXPECT_EQ(ut_sch_stats_counter(kSpinSlot, true).sum, w.sum);
}

TEST_F(MtSchStatsTest, TheStatsDumpHandsTheCountersToTheProbe) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);
  struct ut_sch_tasklet_stat p = ut_sch_stats_probe();
  struct ut_sch_stat t = ut_sch_stats_counter(kSpinSlot, false);
  struct ut_sch_stat w = ut_sch_stats_counter(kSpinSlot, true);
  ASSERT_GT(w.cnt, 0u);
  EXPECT_EQ(p.sch_idx, 0);
  EXPECT_STREQ(p.name, "ut_spin");
  EXPECT_EQ(p.cnt, t.cnt);
  EXPECT_EQ(p.sum, t.sum);
  EXPECT_EQ(p.work_cnt, w.cnt);
  EXPECT_EQ(p.work_sum, w.sum);
}
