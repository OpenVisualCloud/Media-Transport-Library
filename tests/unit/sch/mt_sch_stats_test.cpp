/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The cumulative tasklet counters that the USDT probe sys:tasklet_stat reports. They
 * move only while the time is measured or sampled, survive the periodic stats dump,
 * and start at zero for every tasklet registered.
 */

#include <gtest/gtest.h>
#include <mtl_api.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

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

  void Run(uint64_t flags, unsigned int work_every, uint64_t calls,
           unsigned int long_every = 0) {
    mt_ = ut_sch_stats_main_create(flags);
    ASSERT_NE(mt_, nullptr);
    ASSERT_EQ(ut_sch_stats_register(mt_, work_every, long_every), 0);
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
  EXPECT_EQ(ut_sch_stats_hist_cnt(kTaskletSlot), 0u);
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
  EXPECT_EQ(ut_sch_stats_hist_cnt(kTaskletSlot), 0u);
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
  ASSERT_GT(ut_sch_stats_hist_cnt(kTaskletSlot), 0u);

  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery, 0), 0);
  t = ut_sch_stats_counter(kTaskletSlot);
  EXPECT_EQ(t.cnt, 0u);
  EXPECT_EQ(t.work_cnt, 0u);
  EXPECT_EQ(ut_sch_stats_hist_cnt(kTaskletSlot), 0u);
  EXPECT_EQ(ut_sch_stats_counter(kFillerSlot).cnt, kCalls);
  EXPECT_EQ(ut_sch_stats_hist_cnt(kFillerSlot), kCalls);
}

TEST_F(MtSchStatsTest, RegisterBelowATaskletKeepsItRunning) {
  mt_ = ut_sch_stats_main_create(MTL_FLAG_TASKLET_TIME_MEASURE);
  ASSERT_NE(mt_, nullptr);
  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery, 0), 0);
  ASSERT_EQ(ut_sch_stats_register_idle(mt_), 0);
  /* back into slot 1, below ut_idle */
  ASSERT_EQ(ut_sch_stats_register(mt_, kWorkEvery, 0), 0);
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

static std::string g_line;

static void ut_sch_line_printer(enum mtl_log_level level, const char* format, ...) {
  char line[512];
  va_list args;

  if (level != MTL_LOG_LEVEL_NOTICE) return;
  va_start(args, format);
  (void)vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (strstr(line, "tasklet ut_tasklet,")) g_line = line;
}

TEST_F(MtSchStatsTest, TheStatsDumpLinePrintsTheTailOfTheTimedCalls) {
  /* 1 call in 13 works and every 4th of those is long, so of 520 calls 480 are idle, 30
   * work and 10 long: the p90 rank, 468, falls among the idle, p95, 494, among the work
   * and p99, 515, among the long calls */
  const unsigned int work_every = 13;
  const double idle_us = UT_SCH_IDLE_NS / 1000.0, work_us = UT_SCH_WORK_NS / 1000.0,
               long_us = UT_SCH_LONG_NS / 1000.0;
  Run(MTL_FLAG_TASKLET_TIME_MEASURE, work_every, 40 * (uint64_t)work_every,
      4 * work_every);

  g_line.clear();
  mtl_set_log_printer(ut_sch_line_printer);
  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);
  mtl_set_log_printer(nullptr);

  const char* fields = strstr(g_line.c_str(), "avg ");
  ASSERT_NE(fields, nullptr) << g_line;
  double p90, p95, p99;
  ASSERT_EQ(sscanf(fields, "avg %*fus max %*fus min %*fus, p90 %lfus p95 %lfus p99 %lfus",
                   &p90, &p95, &p99),
            3)
      << g_line;
  /* each within its bucket, at most 1/8 of the value wide */
  EXPECT_NEAR(p90, idle_us, idle_us / 8);
  EXPECT_NEAR(p95, work_us, work_us / 8);
  EXPECT_NEAR(p99, long_us, long_us / 8);
}

TEST_F(MtSchStatsTest, TheHistogramHoldsEveryTimedCallUntilTheDump) {
  RunMeasured();

  EXPECT_EQ(ut_sch_stats_hist_cnt(kTaskletSlot), kCalls);
  EXPECT_EQ(ut_sch_stats_dump(mt_), 0);
  EXPECT_EQ(ut_sch_stats_hist_cnt(kTaskletSlot), 0u);
}

/* past the last bucket bound, 2^18 ns, by two octaves */
static const uint64_t kHistSweepNs = (uint64_t)1 << 20;

TEST(MtSchTimeHist, EveryValueFallsWithinItsBucket) {
  const unsigned int nb = ut_sch_hist_nb();
  for (uint64_t ns = 0; ns < kHistSweepNs; ns++) {
    unsigned int b = ut_sch_hist_bucket(ns);
    ASSERT_LT(b, nb) << ns;
    ASSERT_GE(ns, ut_sch_hist_bucket_lo(b)) << ns;
    if (b + 1 < nb) {
      ASSERT_LT(ns, ut_sch_hist_bucket_lo(b + 1)) << ns;
    }
  }
}

TEST(MtSchTimeHist, BucketsAreNonEmptyAndAnEighthOfTheirLowerBoundWideAtMost) {
  const unsigned int nb = ut_sch_hist_nb();
  for (unsigned int b = 1; b + 1 < nb; b++) {
    uint64_t lo = ut_sch_hist_bucket_lo(b);
    uint64_t hi = ut_sch_hist_bucket_lo(b + 1);
    ASSERT_GT(hi, lo) << b;
    EXPECT_LE(hi - lo, lo / 8) << b;
  }
}

TEST(MtSchTimeHist, TheEdgeBucketsHoldTheExtremes) {
  const unsigned int last = ut_sch_hist_nb() - 1;
  EXPECT_EQ(ut_sch_hist_bucket(0), 0u);
  EXPECT_EQ(ut_sch_hist_bucket(7), 0u);
  EXPECT_EQ(ut_sch_hist_bucket(8), 1u);
  EXPECT_EQ(ut_sch_hist_bucket(((uint64_t)1 << 18) - 1), last - 1);
  EXPECT_EQ(ut_sch_hist_bucket((uint64_t)1 << 18), last);
  EXPECT_EQ(ut_sch_hist_bucket(UINT64_MAX), last);
}

/* The histogram of a set of calls, with the min and max of the interval. */
class UtSchHist {
 public:
  UtSchHist() : hist_(ut_sch_hist_nb()) {
  }

  void Add(uint64_t ns, uint64_t calls = 1) {
    hist_[ut_sch_hist_bucket(ns)] += calls;
    min_ = std::min(min_, ns);
    max_ = std::max(max_, ns);
  }

  double Quantile(unsigned int q) const {
    return ut_sch_hist_quantile(hist_.data(), min_, max_, q);
  }

  /* the width of the bucket of ns, the last one ending at the max */
  uint64_t Width(uint64_t ns) const {
    unsigned int b = ut_sch_hist_bucket(ns);
    uint64_t hi = b + 1 < hist_.size() ? ut_sch_hist_bucket_lo(b + 1) : max_;
    return hi - ut_sch_hist_bucket_lo(b);
  }

 private:
  std::vector<uint64_t> hist_;
  uint64_t min_ = UINT64_MAX;
  uint64_t max_ = 0;
};

TEST(MtSchTimeHistQuantile, TwoValuedTailsFallOnTheirSideOfTheStep) {
  const uint64_t fast = 1000, slow = 50000;
  /* nearest rank: with 900 of 1000 fast, the 900th call, p90, is the last fast one */
  const struct {
    uint64_t fast_calls;
    bool p90_slow, p95_slow, p99_slow;
  } cases[] = {
      {900, false, true, true},
      {950, false, false, true},
      {990, false, false, false},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.fast_calls);
    UtSchHist h;
    h.Add(fast, c.fast_calls);
    h.Add(slow, 1000 - c.fast_calls);
    const unsigned int qs[] = {90, 95, 99};
    const bool slow_side[] = {c.p90_slow, c.p95_slow, c.p99_slow};
    for (int i = 0; i < 3; i++) {
      uint64_t want = slow_side[i] ? slow : fast;
      EXPECT_NEAR(h.Quantile(qs[i]), (double)want, (double)h.Width(want)) << "p" << qs[i];
    }
  }
}

TEST(MtSchTimeHistQuantile, InterpolationStaysWithinABucketOfTheExactRank) {
  /* log-uniform from 1 ns to 4 ms, past the last bucket bound */
  std::mt19937_64 rng(2463534242u);
  std::uniform_real_distribution<double> log_ns(0.0, std::log(4e6));
  std::vector<uint64_t> calls(10000);
  UtSchHist h;
  for (auto& ns : calls) {
    ns = (uint64_t)std::exp(log_ns(rng));
    h.Add(ns);
  }
  std::sort(calls.begin(), calls.end());

  for (unsigned int q = 1; q <= 100; q++) {
    size_t rank = (q * calls.size() + 99) / 100;
    uint64_t exact = calls[rank - 1];
    EXPECT_NEAR(h.Quantile(q), (double)exact, (double)h.Width(exact)) << "p" << q;
  }
}

TEST(MtSchTimeHistQuantile, OneValuedQuantilesAreThatValue) {
  /* the first bucket, a middle one, and the last, which ends at the max */
  const uint64_t values[] = {5, 1000, 5000000};
  for (uint64_t ns : values) {
    UtSchHist h;
    h.Add(ns, 100);
    EXPECT_EQ(h.Quantile(90), (double)ns);
    EXPECT_EQ(h.Quantile(95), (double)ns);
    EXPECT_EQ(h.Quantile(99), (double)ns);
  }
}

TEST(MtSchTimeHistQuantile, TheLastBucketInterpolatesUpToTheMax) {
  const uint64_t lo = ut_sch_hist_bucket_lo(ut_sch_hist_nb() - 1);
  const uint64_t max = 1000000;
  /* every call past the last bucket's lower bound: only the max bounds it */
  UtSchHist h;
  h.Add(lo + lo / 8, 50);
  h.Add(max, 50);

  for (unsigned int q : {90u, 95u, 99u}) {
    EXPECT_DOUBLE_EQ(h.Quantile(q), lo + (double)(max - lo) * q / 100) << "p" << q;
  }
}
