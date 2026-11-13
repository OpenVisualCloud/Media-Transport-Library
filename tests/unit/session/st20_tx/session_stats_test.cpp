/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * st20_tx_get_session_stats() / st20_tx_reset_session_stats(): a reset sets the baseline
 * a get counts from, and neither takes the session lock nor waits for the tasklets.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxSessionStats*'
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "session/st20_tx_harness.h"

namespace {

constexpr size_t kFields = sizeof(st20_tx_user_stats) / sizeof(uint64_t);
static_assert(sizeof(st20_tx_user_stats) % sizeof(uint64_t) == 0, "uint64_t counters");

/* field i is scale * (i + 1), so a field read from the wrong place shows */
st20_tx_user_stats Scaled(uint64_t scale) {
  st20_tx_user_stats s;
  uint64_t* f = reinterpret_cast<uint64_t*>(&s);
  for (size_t i = 0; i < kFields; i++) f[i] = scale * (i + 1);
  return s;
}

void ExpectScaled(const st20_tx_user_stats& got, uint64_t scale) {
  const uint64_t* f = reinterpret_cast<const uint64_t*>(&got);
  for (size_t i = 0; i < kFields; i++) EXPECT_EQ(scale * (i + 1), f[i]) << "field " << i;
}

class St20TxSessionStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  void Add(uint64_t scale) {
    st20_tx_user_stats add = Scaled(scale);
    ut_txv_add_user_stats(ctx_, &add);
  }
  void ExpectGet(uint64_t scale) {
    st20_tx_user_stats got;
    ASSERT_EQ(0, ut_txv_get_stats(ctx_, &got));
    ExpectScaled(got, scale);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxSessionStatsTest, GetCountsEveryFieldFromTheLastReset) {
  Add(1000);
  ExpectGet(1000);
  ASSERT_EQ(0, ut_txv_reset_stats(ctx_));
  ASSERT_EQ(0, ut_txv_reset_stats(ctx_));
  ExpectGet(0);
  Add(1);
  ExpectGet(1);
  Add(2);
  ExpectGet(3);
  ASSERT_EQ(0, ut_txv_reset_stats(ctx_));
  Add(2);
  ExpectGet(2);
}

/* The periodic dump keeps deltas against its own snapshot of the counters. */
TEST_F(St20TxSessionStatsTest, ResetLeavesTheStatDumpPeriodWhole) {
  int locked_lines = 0;
  ut_txv_set_stat_port_frames(ctx_, 250);
  ASSERT_GT(ut_txv_run_sessions_stat(ctx_, &locked_lines), 0);

  ut_txv_set_stat_port_frames(ctx_, 260);
  ASSERT_EQ(0, ut_txv_reset_stats(ctx_));
  ut_txv_set_stat_port_frames(ctx_, 263);
  ASSERT_GT(ut_txv_run_sessions_stat(ctx_, &locked_lines), 0);

  EXPECT_NE(nullptr, std::strstr(ut_txv_stat_first_log_line(), "frames 13 "))
      << ut_txv_stat_first_log_line();
}

/* A reset preempted mid-copy does not hold up a get. */
TEST_F(St20TxSessionStatsTest, GetDoesNotWaitForAReset) {
  st20_tx_user_stats got;
  bool reset_at_return = false;
  Add(4);

  ASSERT_EQ(0, ut_txv_get_stats_during_reset(ctx_, &got, &reset_at_return));
  EXPECT_TRUE(reset_at_return);
  ExpectScaled(got, 4);
}

class St20TxSessionStatsCallTest : public ::testing::TestWithParam<ut_txv_control> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
    st20_tx_user_stats add = Scaled(3);
    ut_txv_add_user_stats(ctx_, &add);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  /* what the call read, or after a reset what a get reads now */
  void ExpectCallSaw() {
    st20_tx_user_stats got;
    if (GetParam() == UT_TXV_STATS_GET) {
      ut_txv_control_stats(ctx_, &got);
      ExpectScaled(got, 3);
    } else {
      ASSERT_EQ(0, ut_txv_get_stats(ctx_, &got));
      ExpectScaled(got, 0);
    }
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_P(St20TxSessionStatsCallTest, WorksInTheSessionsOwnCallbackWhileAHolderWaits) {
  EXPECT_EQ(0, ut_txv_run_control_in_callback(ctx_, GetParam(), true));
  ExpectCallSaw();
}

TEST_P(St20TxSessionStatsCallTest, DoesNotWaitForTheSessionsCallback) {
  bool done_in_callback = false;

  EXPECT_EQ(0, ut_txv_control_during_own_callback(ctx_, GetParam(), &done_in_callback));
  EXPECT_TRUE(done_in_callback);
}

INSTANTIATE_TEST_SUITE_P(Calls, St20TxSessionStatsCallTest,
                         ::testing::Values(UT_TXV_STATS_GET, UT_TXV_STATS_RESET),
                         [](const ::testing::TestParamInfo<ut_txv_control>& info) {
                           return info.param == UT_TXV_STATS_GET ? "Get" : "Reset";
                         });

class St20TxSessionStatsRaceTest : public ::testing::TestWithParam<ut_txv_tasklet> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    if (ut_txv_spare_cpus() < 3) GTEST_SKIP() << "needs three CPUs besides the EAL's";
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

/* Each get is non-negative per field and does not go back without a reset between. */
TEST_P(St20TxSessionStatsRaceTest, GettersAndResettersAgreeWhileTheTaskletsCount) {
  ut_txv_reader_load load = {};
  load.tasklet = GetParam();
  load.passes = 6000;
  load.getters = 1;
  load.resetters = 1;
  load.reset_pause_us = 20; /* time for gets with no reset around them */
  ut_txv_reader_result got = {};

  ASSERT_EQ(0, ut_txv_run_readers(ctx_, &load, &got));
  std::printf("  %llu gets, %llu resets, %llu gets compared\n",
              static_cast<unsigned long long>(got.gets),
              static_cast<unsigned long long>(got.resets),
              static_cast<unsigned long long>(got.compared));
  ASSERT_FALSE(got.hung);
  EXPECT_EQ(0, got.call_err);
  EXPECT_EQ(load.passes, got.ran);
  EXPECT_GT(got.resets, 0u);
  EXPECT_EQ(0u, got.wrapped);
  EXPECT_GT(got.compared, 0u);
  EXPECT_EQ(0u, got.backwards);
}

INSTANTIATE_TEST_SUITE_P(Tasklets, St20TxSessionStatsRaceTest,
                         ::testing::Values(UT_TXV_TASKLET_BUILDER,
                                           UT_TXV_TASKLET_TRANSMITTER),
                         [](const ::testing::TestParamInfo<ut_txv_tasklet>& info) {
                           return info.param == UT_TXV_TASKLET_BUILDER ? "Builder"
                                                                       : "Transmitter";
                         });

}  // namespace
