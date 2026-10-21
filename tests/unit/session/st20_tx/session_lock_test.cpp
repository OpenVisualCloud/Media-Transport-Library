/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the TX video session lock protocol: only the control plane takes a session's
 * spinlock, then waits out the scheduler pass in flight; the tasklets take no lock and
 * skip the session while it is held.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxSessionLock*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "session/st20_tx_harness.h"

namespace {

template <typename Param>
class St20TxSessionLockTest : public ::testing::TestWithParam<Param> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

using St20TxSessionLockTaskletTest = St20TxSessionLockTest<ut_txv_tasklet>;
using St20TxSessionLockControlTest = St20TxSessionLockTest<ut_txv_control>;
using St20TxSessionLockMigratedTest = St20TxSessionLockTest<ut_txv_control>;
using St20TxSessionLockCallbackTest = St20TxSessionLockTest<ut_txv_control>;
using St20TxSessionLockRacingTest = St20TxSessionLockTest<ut_txv_control>;
using St20TxSessionLockCallbackStatsTest = St20TxSessionLockTest<ut_txv_control>;
using St20TxSessionLockCallbackRefusedTest = St20TxSessionLockTest<ut_txv_control>;

TEST_P(St20TxSessionLockTaskletTest, SkipsASessionWhileItsLockIsHeld) {
  bool locked_in_run = false, locked_after = false;

  EXPECT_EQ(0, ut_txv_run_tasklet_lock_probe(ctx_, GetParam(), true, &locked_in_run,
                                             &locked_after));
  EXPECT_TRUE(locked_after);
}

/* A locked instruction per session drains the store buffer behind the NIC doorbell. */
TEST_P(St20TxSessionLockTaskletTest, RunsASessionWithoutTakingItsLock) {
  bool locked_in_run = true, locked_after = true;

  EXPECT_EQ(1, ut_txv_run_tasklet_lock_probe(ctx_, GetParam(), false, &locked_in_run,
                                             &locked_after));
  EXPECT_FALSE(locked_in_run);
  EXPECT_FALSE(locked_after);
}

/* Waiting first, a tasklet could start a pass that misses the lock taken after. */
TEST_P(St20TxSessionLockControlTest, LocksThenWaitsForTheSchedulerOnce) {
  bool locked_at_wait = false;

  EXPECT_EQ(1, ut_txv_run_control(ctx_, GetParam(), &locked_at_wait));
  EXPECT_TRUE(locked_at_wait);
}

/* The old manager's lock would leave the counters to the new scheduler's tasklets. */
TEST_P(St20TxSessionLockMigratedTest, LocksTheNewSlotAndWaitsForTheNewScheduler) {
  bool locked_at_wait = false;

  EXPECT_EQ(1, ut_txv_run_control_after_migration(ctx_, GetParam(), &locked_at_wait));
  EXPECT_TRUE(locked_at_wait);
}

/* A migration moves the session before it frees the old slot, so a retry finds it. */
TEST_P(St20TxSessionLockRacingTest, FindsASessionThatMigratedWhileItWaited) {
  EXPECT_EQ(0, ut_txv_run_control_racing_migration(ctx_, GetParam()));
}

TEST_P(St20TxSessionLockRacingTest, FindsASessionMovedByAHolderOfItsOldSlot) {
  EXPECT_EQ(0, ut_txv_run_control_blocked_by_migration(ctx_, GetParam()));
}

/* The holder waits for the very pass the callback runs in, so spinning never ends. */
TEST_P(St20TxSessionLockCallbackTest, ReturnsBusyInsteadOfSpinningOnAWaitingHolder) {
  EXPECT_EQ(-EBUSY, ut_txv_run_control_in_callback(ctx_, GetParam(), true));
}

TEST_P(St20TxSessionLockCallbackStatsTest, WorkForTheCallbacksOwnSession) {
  EXPECT_EQ(0, ut_txv_run_control_in_callback(ctx_, GetParam(), false));
}

/* Free: the calling tasklet may still use the session. Update: ARP would stall it. */
TEST_P(St20TxSessionLockCallbackRefusedTest, IsRefusedEvenWithTheLockFree) {
  EXPECT_EQ(-EBUSY, ut_txv_run_control_in_callback(ctx_, GetParam(), false));
}

/* The stats dump must not hang on a stuck scheduler. */
TEST(St20TxSessionLockTimeoutTest, GetTimeoutGivesUpOnAPassThatNeverEnds) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  bool locked_after = true;

  EXPECT_FALSE(ut_txv_get_timeout_on_endless_pass(ctx, 100, &locked_after));
  EXPECT_FALSE(locked_after);
  ut_txv_destroy(ctx);
}

std::string ControlName(const ::testing::TestParamInfo<ut_txv_control>& info) {
  switch (info.param) {
    case UT_TXV_GET:
      return "Get";
    case UT_TXV_GET_TIMEOUT:
      return "GetTimeout";
    case UT_TXV_GET_EMPTY:
      return "GetEmpty";
    case UT_TXV_STATS_GET:
      return "StatsGet";
    case UT_TXV_STATS_RESET:
      return "StatsReset";
    case UT_TXV_UPDATE_DST:
      return "UpdateDst";
    default:
      return "Free";
  }
}

INSTANTIATE_TEST_SUITE_P(Tasklets, St20TxSessionLockTaskletTest,
                         ::testing::Values(UT_TXV_TASKLET_BUILDER,
                                           UT_TXV_TASKLET_TRANSMITTER),
                         [](const ::testing::TestParamInfo<ut_txv_tasklet>& info) {
                           return info.param == UT_TXV_TASKLET_BUILDER ? "Builder"
                                                                       : "Transmitter";
                         });

INSTANTIATE_TEST_SUITE_P(Calls, St20TxSessionLockControlTest,
                         ::testing::Values(UT_TXV_GET, UT_TXV_GET_TIMEOUT,
                                           UT_TXV_GET_EMPTY, UT_TXV_STATS_GET,
                                           UT_TXV_STATS_RESET, UT_TXV_UPDATE_DST),
                         ControlName);

INSTANTIATE_TEST_SUITE_P(Stats, St20TxSessionLockMigratedTest,
                         ::testing::Values(UT_TXV_STATS_GET, UT_TXV_STATS_RESET),
                         ControlName);

INSTANTIATE_TEST_SUITE_P(Stats, St20TxSessionLockRacingTest,
                         ::testing::Values(UT_TXV_STATS_GET, UT_TXV_STATS_RESET),
                         ControlName);

INSTANTIATE_TEST_SUITE_P(Stats, St20TxSessionLockCallbackStatsTest,
                         ::testing::Values(UT_TXV_STATS_GET, UT_TXV_STATS_RESET),
                         ControlName);

INSTANTIATE_TEST_SUITE_P(PublicApis, St20TxSessionLockCallbackRefusedTest,
                         ::testing::Values(UT_TXV_UPDATE_DST, UT_TXV_FREE), ControlName);

INSTANTIATE_TEST_SUITE_P(Stats, St20TxSessionLockCallbackTest,
                         ::testing::Values(UT_TXV_STATS_GET, UT_TXV_STATS_RESET),
                         ControlName);

}  // namespace
