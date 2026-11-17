/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins mt_sch_quiescent_wait(), which a control-plane writer calls after taking a session
 * lock the tasklets check without taking: it returns only once no tasklet pass of the
 * scheduler that could have missed the lock is still running.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*MtSchQuiescent*'
 */

#include <gtest/gtest.h>

#include "sch/mt_sch_harness.h"

namespace {

class MtSchQuiescentTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sch_init(), 0);
  }
};

TEST_F(MtSchQuiescentTest, WaitReturnsAtOnceOnAStoppedScheduler) {
  int polls = -1;

  ASSERT_EQ(ut_sch_qs_wait_after_stop(&polls), 0);
  EXPECT_EQ(polls, 0);
}

/* The scheduler thread is past its own tasklets' loads, and polling would never end. */
TEST_F(MtSchQuiescentTest, WaitReturnsAtOnceOnTheSchedulerThread) {
  int polls = -1;

  ASSERT_EQ(ut_sch_qs_wait_on_sch_thread(&polls), 0);
  EXPECT_EQ(polls, 0);
}

/* A sleeping scheduler must not keep a writer waiting for the length of its sleep. */
TEST_F(MtSchQuiescentTest, SchedulerGoesOfflineBeforeItSleeps) {
  int sleeps = 0, online_sleeps = -1;

  ASSERT_EQ(ut_sch_qs_sleeps_offline(&sleeps, &online_sleeps), 0);
  EXPECT_GE(sleeps, 3);
  EXPECT_EQ(online_sleeps, 0);
}

TEST_F(MtSchQuiescentTest, EndOfARunningSchedulerPassSatisfiesTheWait) {
  bool published = false;

  ASSERT_EQ(ut_sch_qs_pass_end_publishes(&published), 0);
  EXPECT_TRUE(published);
}

class MtSchQuiescentPassTest : public MtSchQuiescentTest,
                               public testing::WithParamInterface<ut_sch_qs_pass_end> {};

TEST_P(MtSchQuiescentPassTest, WaitOutlastsThePassInFlight) {
  bool returned_in_pass = true, returned_after_end = false;

  ASSERT_EQ(
      ut_sch_qs_wait_across_pass(GetParam(), &returned_in_pass, &returned_after_end), 0);
  EXPECT_FALSE(returned_in_pass);
  EXPECT_TRUE(returned_after_end);
}

INSTANTIATE_TEST_SUITE_P(Ends, MtSchQuiescentPassTest,
                         testing::Values(UT_SCH_QS_END_OF_PASS, UT_SCH_QS_GO_OFFLINE),
                         [](const testing::TestParamInfo<ut_sch_qs_pass_end>& info) {
                           return info.param == UT_SCH_QS_END_OF_PASS ? "EndOfPass"
                                                                      : "GoOffline";
                         });

}  // namespace
