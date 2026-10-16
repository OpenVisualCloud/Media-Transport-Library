/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_sch_get_stats() reports the cumulative tasklet and loop timing of one scheduler.
 * The counters move only while the time is measured or sampled, survive the periodic
 * stats dump, and clear on mtl_sch_reset_stats().
 */

#include <errno.h>
#include <gtest/gtest.h>
#include <mtl_api.h>

#include "sch/mt_sch_harness.h"

/* The spin budget of the one tasklet; the measurement brackets the handler, so every
 * sample is at least this long minus nothing. */
static const unsigned int kSpinUs = 50;
static const uint64_t kSpinFloorNs = 40000;
static const unsigned int kWorkEvery = 4;

class MtlSchStatsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
  }

  void TearDown() override {
    if (started_) ut_sch_stats_stop(mt_);
    if (mt_) ut_sch_stats_main_destroy(mt_);
  }

  void Create(uint64_t flags, unsigned int work_every = 0) {
    mt_ = ut_sch_stats_main_create(flags, kSpinUs, work_every);
    ASSERT_NE(mt_, nullptr);
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
  struct mtl_sch_stats stats_ = {};
};

TEST_F(MtlSchStatsTest, CountReturnsTheSchSlotNumber) {
  Create(0);
  EXPECT_EQ(mtl_sch_get_count(mt_), ut_sch_max_sch_num());
}

TEST_F(MtlSchStatsTest, RejectsANullStatsPointer) {
  Create(0);
  EXPECT_EQ(mtl_sch_get_stats(mt_, 0, nullptr), -EINVAL);
}

TEST_F(MtlSchStatsTest, RejectsANegativeSchIndex) {
  Create(0);
  EXPECT_EQ(mtl_sch_get_stats(mt_, -1, &stats_), -EINVAL);
  EXPECT_EQ(mtl_sch_reset_stats(mt_, -1), -EINVAL);
}

TEST_F(MtlSchStatsTest, RejectsOnePastTheLastSchIndex) {
  Create(0);
  EXPECT_EQ(mtl_sch_get_stats(mt_, ut_sch_max_sch_num(), &stats_), -EINVAL);
  EXPECT_EQ(mtl_sch_reset_stats(mt_, ut_sch_max_sch_num()), -EINVAL);
}

TEST_F(MtlSchStatsTest, ReportsNoEntryForAnInactiveSch) {
  Create(0);
  EXPECT_EQ(mtl_sch_get_stats(mt_, 1, &stats_), -ENOENT);
  EXPECT_EQ(mtl_sch_reset_stats(mt_, 1), -ENOENT);
}

/* sch_free() clears the array before it clears active; a getter that checked active
 * before taking the lock would read through the NULL array. */
TEST_F(MtlSchStatsTest, ReportsNoEntryWhenTheTaskletArrayIsGone) {
  Create(0);
  ut_sch_stats_drop_tasklets(mt_);
  EXPECT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), -ENOENT);
  EXPECT_EQ(mtl_sch_reset_stats(mt_, 0), -ENOENT);
}

TEST_F(MtlSchStatsTest, SkipsEmptyTaskletSlots) {
  Create(0);
  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_STREQ(stats_.name, "ut_sch");
  EXPECT_EQ(stats_.nb_tasklets, 1);
  EXPECT_EQ(stats_.nb_tasklets_total, 1);
  EXPECT_STREQ(stats_.tasklets[0].name, UT_SCH_SPIN_TASKLET_NAME);
}

TEST_F(MtlSchStatsTest, ReportsTheTotalWhenTaskletsExceedTheArray) {
  Create(0);
  ASSERT_EQ(ut_sch_stats_add_idle_tasklets(mt_, MTL_SCH_MAX_TASKLETS), 0);

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_EQ(stats_.nb_tasklets, MTL_SCH_MAX_TASKLETS);
  EXPECT_EQ(stats_.nb_tasklets_total, MTL_SCH_MAX_TASKLETS + 1);
  EXPECT_STREQ(stats_.tasklets[0].name, UT_SCH_SPIN_TASKLET_NAME);
  EXPECT_STREQ(stats_.tasklets[MTL_SCH_MAX_TASKLETS - 1].name, "ut_idle_64");
}

TEST_F(MtlSchStatsTest, CountersStayZeroWhileMeasureIsOff) {
  Create(0, kWorkEvery);
  RunCalls(kWorkEvery * 20);

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_FALSE(stats_.time_measure_enabled);
  EXPECT_EQ(stats_.loop_time.cnt, 0u);
  EXPECT_EQ(stats_.loop_time.min_ns, 0u);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  EXPECT_EQ(stats_.tasklets[0].time.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].time.min_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.min_ns, 0u);
}

TEST_F(MtlSchStatsTest, MeasuresASpinningTasklet) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE);
  RunCalls(20);

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_TRUE(stats_.time_measure_enabled);
  EXPECT_EQ(stats_.lcore, UINT32_MAX);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  EXPECT_STREQ(stats_.tasklets[0].name, UT_SCH_SPIN_TASKLET_NAME);

  const struct mtl_time_stats& t = stats_.tasklets[0].time;
  ASSERT_GT(t.cnt, 0u);
  uint64_t avg = t.sum_ns / t.cnt;
  EXPECT_LE(t.min_ns, avg);
  EXPECT_LE(avg, t.max_ns);
  EXPECT_GE(t.min_ns, kSpinFloorNs);

  ASSERT_GT(stats_.loop_time.cnt, 0u);
  EXPECT_GE(stats_.loop_time.min_ns, kSpinFloorNs);
}

TEST_F(MtlSchStatsTest, WorkCountsOnlyTheCallsThatReportPending) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  const struct mtl_time_stats& t = stats_.tasklets[0].time;
  const struct mtl_time_stats& w = stats_.tasklets[0].work;
  ASSERT_GT(w.cnt, 0u);
  EXPECT_LT(w.cnt, t.cnt);
  EXPECT_EQ(w.cnt, t.cnt / kWorkEvery);
  EXPECT_GE(w.min_ns, kSpinFloorNs);
  EXPECT_LE(w.sum_ns, t.sum_ns);
  EXPECT_LE(w.max_ns, t.max_ns);
  /* the idle calls are in time only, so they pull its min below the spin */
  EXPECT_LT(t.min_ns, kSpinFloorNs);
}

TEST_F(MtlSchStatsTest, WorkStaysZeroForATaskletThatReportsIdle) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE);
  RunCalls(20);
  Stop();

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  EXPECT_GT(stats_.tasklets[0].time.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.sum_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.min_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.max_ns, 0u);
}

TEST_F(MtlSchStatsTest, SampleTimesSomeLoopsAndCountsEveryCall) {
  Create(MTL_FLAG_TASKLET_TIME_SAMPLE);
  RunCalls(MTL_SCH_TIME_SAMPLE_PERIOD * 32);
  Stop();

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_FALSE(stats_.time_measure_enabled);
  EXPECT_TRUE(stats_.time_sample_enabled);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  const struct mtl_time_stats& t = stats_.tasklets[0].time;
  /* every call is counted; the last timed one stands for a gap that can outrun the
   * loops the scheduler made */
  uint64_t calls = ut_sch_stats_calls(mt_);
  EXPECT_GE(t.cnt, calls);
  EXPECT_LT(t.cnt, calls + 2 * MTL_SCH_TIME_SAMPLE_PERIOD);
  EXPECT_GE(t.min_ns, kSpinFloorNs);
  uint64_t avg = t.sum_ns / t.cnt;
  EXPECT_LE(t.min_ns, avg);
  EXPECT_LE(avg, t.max_ns);
  /* a timed loop pays for its own timing, so the loop time is never sampled */
  EXPECT_EQ(stats_.loop_time.cnt, 0u);
  /* the stats dump reports full measurements only */
  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_EQ(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
}

TEST_F(MtlSchStatsTest, SampleEstimatesTheWorkShare) {
  /* work on every 4th call divides the period: a sample every 64th loop would see only
   * work calls or none */
  Create(MTL_FLAG_TASKLET_TIME_SAMPLE, kWorkEvery);
  RunCalls(kWorkEvery * MTL_SCH_TIME_SAMPLE_PERIOD * 64);
  Stop();

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  ASSERT_EQ(stats_.nb_tasklets, 1);
  const struct mtl_time_stats& t = stats_.tasklets[0].time;
  const struct mtl_time_stats& w = stats_.tasklets[0].work;
  ASSERT_GT(t.cnt, 0u);
  double share = (double)w.cnt / t.cnt;
  EXPECT_GT(share, 0.15);
  EXPECT_LT(share, 0.35);
  EXPECT_GE(w.min_ns, kSpinFloorNs);
}

TEST_F(MtlSchStatsTest, MeasureWinsOverSample) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE | MTL_FLAG_TASKLET_TIME_SAMPLE);
  RunCalls(20);
  Stop();

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_TRUE(stats_.time_measure_enabled);
  EXPECT_FALSE(stats_.time_sample_enabled);
  EXPECT_GT(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  EXPECT_GT(ut_sch_stats_dump_tasklet_cnt(mt_), 0u);
  EXPECT_EQ(stats_.tasklets[0].time.cnt, ut_sch_stats_calls(mt_));
}

TEST_F(MtlSchStatsTest, DumpResetsOnlyThePerDumpCounters) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();

  struct mtl_sch_stats before = {};
  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &before), 0);
  ASSERT_GT(before.loop_time.cnt, 0u);
  ASSERT_GT(before.tasklets[0].work.cnt, 0u);
  ASSERT_GT(ut_sch_stats_dump_loop_cnt(mt_), 0u);

  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);

  EXPECT_EQ(ut_sch_stats_dump_loop_cnt(mt_), 0u);
  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_EQ(stats_.loop_time.cnt, before.loop_time.cnt);
  EXPECT_EQ(stats_.loop_time.sum_ns, before.loop_time.sum_ns);
  EXPECT_EQ(stats_.tasklets[0].time.cnt, before.tasklets[0].time.cnt);
  EXPECT_EQ(stats_.tasklets[0].time.sum_ns, before.tasklets[0].time.sum_ns);
  EXPECT_EQ(stats_.tasklets[0].work.cnt, before.tasklets[0].work.cnt);
  EXPECT_EQ(stats_.tasklets[0].work.sum_ns, before.tasklets[0].work.sum_ns);
}

TEST_F(MtlSchStatsTest, ResetClearsTheCumulativeCounters) {
  Create(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery);
  RunCalls(kWorkEvery * 20);
  Stop();
  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  ASSERT_GT(stats_.tasklets[0].time.cnt, 0u);
  ASSERT_GT(stats_.tasklets[0].work.cnt, 0u);

  EXPECT_EQ(mtl_sch_reset_stats(mt_, 0), 0);

  ASSERT_EQ(mtl_sch_get_stats(mt_, 0, &stats_), 0);
  EXPECT_EQ(stats_.loop_time.cnt, 0u);
  EXPECT_EQ(stats_.loop_time.sum_ns, 0u);
  EXPECT_EQ(stats_.loop_time.min_ns, 0u);
  EXPECT_EQ(stats_.loop_time.max_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].time.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].time.sum_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].time.min_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].time.max_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.cnt, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.sum_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.min_ns, 0u);
  EXPECT_EQ(stats_.tasklets[0].work.max_ns, 0u);
}
