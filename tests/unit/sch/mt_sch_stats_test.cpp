/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The cumulative tasklet counters of the USDT probe sys:tasklet_stat: exact while the
 * time is measured, sampled while only the probe is attached, zero otherwise. And the
 * percentiles of the measured times that the stats dump prints.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

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
  /* the per-dump time lines and their histogram stay off */
  EXPECT_EQ(ut_sch_stats_measured(s_), 0u);
  EXPECT_EQ(ut_sch_stats_hist_calls(s_, 0), 0u);
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
  ASSERT_EQ(ut_sch_stats_hist_calls(s_, 1), kCalls);
  ut_sch_stats_dump(s_);
  ASSERT_EQ(ut_sch_stats_hist_prev_calls(s_, 1), kCalls);

  ASSERT_EQ(ut_sch_stats_register_idle(s_), 0);
  EXPECT_EQ(ut_sch_stats_counters(1).cnt, 0u);
  EXPECT_EQ(ut_sch_stats_hist_calls(s_, 1), 0u);
  EXPECT_EQ(ut_sch_stats_hist_prev_calls(s_, 1), 0u);
  EXPECT_EQ(ut_sch_stats_counters(0).cnt, kCalls);
  EXPECT_EQ(ut_sch_stats_hist_calls(s_, 0), kCalls);
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

std::string g_tasklet_line;

void TaskletLinePrinter(enum mtl_log_level, const char* format, ...) {
  char line[512];
  va_list args;

  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (strstr(line, "tasklet ut_tasklet,")) g_tasklet_line = line;
}

TEST_F(MtSchStatsTest, TheDumpNeverWritesTheHistogramOfTheScheduler) {
  ASSERT_NO_FATAL_FAILURE(Run(MTL_FLAG_TASKLET_TIME_MEASURE, kWorkEvery, kCalls));
  ut_sch_stats_dump(s_);

  EXPECT_EQ(ut_sch_stats_hist_calls(s_, 0), kCalls);
}

TEST_F(MtSchStatsTest, TheMeasuredLineEndsWithThePercentilesSinceTheLastDump) {
  const unsigned int work_every = 20;
  const uint64_t calls = 100 * work_every;
  /* every call of the first dump works; ut_idle's stay out of ut_tasklet's */
  ASSERT_NO_FATAL_FAILURE(Run(MTL_FLAG_TASKLET_TIME_MEASURE, 1, calls, true));
  ut_sch_stats_dump(s_);
  /* then 1 in 20: by rank, p90 and p95 are idle calls and p99 a working one */
  ut_sch_stats_set_work_every(s_, work_every);
  ASSERT_EQ(ut_sch_stats_run(s_, calls), 0);
  g_tasklet_line.clear();
  mtl_set_log_printer(TaskletLinePrinter);
  ut_sch_stats_dump(s_);
  mtl_set_log_printer(nullptr);

  const char* p = strstr(g_tasklet_line.c_str(), ", p90 ");
  double p90, p95, p99;
  ASSERT_NE(p, nullptr) << g_tasklet_line;
  ASSERT_EQ(sscanf(p, ", p90 %lfus p95 %lfus p99 %lfus", &p90, &p95, &p99), 3);
  EXPECT_NEAR(p90, UT_SCH_IDLE_NS / 1e3, UT_SCH_IDLE_NS / 8e3);
  EXPECT_NEAR(p95, UT_SCH_IDLE_NS / 1e3, UT_SCH_IDLE_NS / 8e3);
  EXPECT_NEAR(p99, UT_SCH_WORK_NS / 1e3, UT_SCH_WORK_NS / 8e3);
}

TEST(MtSchTimeHist, BucketsAreAnEighthOfAPowerOfTwoFrom8nsTo2Pow18ns) {
  const unsigned int nb = ut_sch_hist_nb();
  EXPECT_EQ(ut_sch_hist_bucket_lo(1), 8u);
  EXPECT_EQ(ut_sch_hist_bucket_lo(nb - 1), 1u << 18);
  EXPECT_EQ(ut_sch_hist_bucket(UINT64_MAX), nb - 1);
  for (unsigned int b = 1; b < nb; b++) {
    uint64_t lo = ut_sch_hist_bucket_lo(b);
    EXPECT_EQ(ut_sch_hist_bucket(lo - 1), b - 1) << lo;
    EXPECT_EQ(ut_sch_hist_bucket(lo), b) << lo;
    if (b + 1 < nb) {
      EXPECT_LE(ut_sch_hist_bucket_lo(b + 1) - lo, lo / 8) << lo;
    }
  }
}

TEST(MtSchTimeHist, PercentilesAreWithinAnEighthOfTheExactRank) {
  /* log-uniform over the range the buckets resolve */
  std::mt19937_64 rng(1);
  std::uniform_real_distribution<double> log_ns(std::log(8.0), std::log(262144.0));
  std::vector<uint64_t> calls(10000);
  for (uint64_t& ns : calls) ns = (uint64_t)std::exp(log_ns(rng));
  std::sort(calls.begin(), calls.end());

  for (unsigned int q : {90u, 95u, 99u}) {
    double exact = (double)calls[(q * calls.size() + 99) / 100 - 1];
    EXPECT_NEAR(ut_sch_hist_quantile(calls.data(), calls.size(), q), exact, exact / 8)
        << "p" << q;
  }
}

}  // namespace
