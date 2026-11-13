/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The RX video tasklet holds a session's spinlock while it polls the session and runs
 * its callbacks. Pins what the public calls on that session do from there, from another
 * session's callback on that scheduler, and from another thread.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20RxSessionLock*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "session/st20_harness.h"

namespace {

class St20RxSessionLockTest : public ::testing::TestWithParam<ut20_cb_api> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20_init());
    ctx_ = ut20_cb_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut20_cb_destroy(ctx_);
  }
  ut20_cb_ctx* ctx_ = nullptr;
};

using St20RxSessionLockOwnCallbackTest = St20RxSessionLockTest;
using St20RxSessionLockPeerStatsTest = St20RxSessionLockTest;
using St20RxSessionLockPeerControlTest = St20RxSessionLockTest;
using St20RxSessionLockOtherThreadTest = St20RxSessionLockTest;

/* Its scheduler would spin on the lock it holds itself. */
TEST_P(St20RxSessionLockOwnCallbackTest, LeavesTheSessionWorking) {
  ASSERT_EQ(-EBUSY, ut20_cb_call_in_poll(ctx_, GetParam(), 0));
  EXPECT_FALSE(ut20_cb_took_effect(ctx_, GetParam()));

  ut20_cb_run_pass(ctx_);
  EXPECT_EQ(2, ut20_cb_polls(ctx_, 0));
  EXPECT_EQ(0, ut20_cb_call(ctx_, GetParam()));
  EXPECT_TRUE(ut20_cb_took_effect(ctx_, GetParam()));
}

/* Only the polled session's lock is held there. */
TEST_P(St20RxSessionLockPeerStatsTest, WorkFromAnotherSessionsCallback) {
  EXPECT_EQ(0, ut20_cb_call_in_poll(ctx_, GetParam(), 1));
  EXPECT_TRUE(ut20_cb_took_effect(ctx_, GetParam()));
}

/* Refused by thread, not by lock state, which other threads change. */
TEST_P(St20RxSessionLockPeerControlTest, ReturnsBusyFromAnotherSessionsCallback) {
  EXPECT_EQ(-EBUSY, ut20_cb_call_in_poll(ctx_, GetParam(), 1));
  EXPECT_FALSE(ut20_cb_took_effect(ctx_, GetParam()));
}

TEST_P(St20RxSessionLockOtherThreadTest, WaitsForThePollThenTakesEffect) {
  bool returned_during_poll = true;

  EXPECT_EQ(0, ut20_cb_call_during_poll(ctx_, GetParam(), &returned_during_poll));
  EXPECT_FALSE(returned_during_poll);
  EXPECT_TRUE(ut20_cb_took_effect(ctx_, GetParam()));
}

std::string CallName(const ::testing::TestParamInfo<ut20_cb_api>& info) {
  switch (info.param) {
    case UT20_CB_STATS_GET:
      return "StatsGet";
    case UT20_CB_STATS_RESET:
      return "StatsReset";
    case UT20_CB_UPDATE_SRC:
      return "UpdateSource";
    case UT20_CB_FREE:
      return "Free";
    case UT20_CB_ST22_UPDATE_SRC:
      return "St22UpdateSource";
    case UT20_CB_ST22_FREE:
      return "St22Free";
  }
  return std::to_string(info.param);
}

INSTANTIATE_TEST_SUITE_P(Calls, St20RxSessionLockOwnCallbackTest,
                         ::testing::Values(UT20_CB_STATS_GET, UT20_CB_STATS_RESET,
                                           UT20_CB_UPDATE_SRC, UT20_CB_FREE,
                                           UT20_CB_ST22_UPDATE_SRC, UT20_CB_ST22_FREE),
                         CallName);
INSTANTIATE_TEST_SUITE_P(Calls, St20RxSessionLockPeerStatsTest,
                         ::testing::Values(UT20_CB_STATS_GET, UT20_CB_STATS_RESET),
                         CallName);
INSTANTIATE_TEST_SUITE_P(Calls, St20RxSessionLockPeerControlTest,
                         ::testing::Values(UT20_CB_UPDATE_SRC, UT20_CB_FREE,
                                           UT20_CB_ST22_UPDATE_SRC, UT20_CB_ST22_FREE),
                         CallName);
INSTANTIATE_TEST_SUITE_P(Calls, St20RxSessionLockOtherThreadTest,
                         ::testing::Values(UT20_CB_STATS_GET, UT20_CB_FREE), CallName);

}  // namespace
