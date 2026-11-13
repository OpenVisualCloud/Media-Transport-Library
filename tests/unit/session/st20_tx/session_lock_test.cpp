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
#include <tuple>

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
using St20TxSessionLockRacingTest = St20TxSessionLockTest<ut_txv_control>;
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

/* A writer that finds the pass long waits for the tasklets to leave its session only. */
TEST_P(St20TxSessionLockTaskletTest, PublishesTheSessionItIsOn) {
  bool locked_in_run = true, locked_after = true;
  int in_run = -2, after = -2;

  EXPECT_EQ(1, ut_txv_run_tasklet_lock_probe(ctx_, GetParam(), false, &locked_in_run,
                                             &locked_after));
  ut_txv_lock_probe_slots(ctx_, &in_run, &after);
  EXPECT_EQ(0, in_run);
  EXPECT_EQ(-1, after);
}

/* Waiting first, a tasklet could start a pass that misses the lock taken after. */
TEST_P(St20TxSessionLockControlTest, LocksThenWaitsForTheSchedulerOnce) {
  bool locked_at_wait = false;

  EXPECT_EQ(1, ut_txv_run_control(ctx_, GetParam(), &locked_at_wait));
  EXPECT_TRUE(locked_at_wait);
}

/* The old manager's lock would leave the headers to the new scheduler's tasklets. */
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

TEST(St20TxSessionLockDeadlockTest, CrossSchedulerCallbacksQueryOppositeSessions) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);

  EXPECT_TRUE(ut_txv_cross_scheduler_gets_complete(ctx, 100));
  ut_txv_destroy(ctx);
}

TEST(St20TxSessionLockDeadlockTest,
     MigrationCompletesWhileDestinationCallbackQueriesSource) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);

  EXPECT_TRUE(ut_txv_migration_with_destination_callback_completes(ctx, 100));
  ut_txv_destroy(ctx);
}

/* (call, whether the tasklets fence each slot store as without membarrier(2)) */
using ControlFence = std::tuple<ut_txv_control, bool>;

class St20TxSessionLockAppMutexTest : public ::testing::TestWithParam<ControlFence> {};

TEST_P(St20TxSessionLockAppMutexTest, CompletesWhilePeerCallbackNeedsCallerMutex) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  ut_txv_set_dp_fence(ctx, std::get<1>(GetParam()));
  EXPECT_TRUE(
      ut_txv_control_with_peer_callback_completes(ctx, std::get<0>(GetParam()), 100));
  ut_txv_destroy(ctx);
}

/* As under the per-session lock the tasklets held before: none of the session's state
 * changes under its callback. */
class St20TxSessionLockOwnCallbackTest : public ::testing::TestWithParam<ControlFence> {};

TEST_P(St20TxSessionLockOwnCallbackTest, WaitsUntilTheSessionsCallbackReturns) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  ut_txv_set_dp_fence(ctx, std::get<1>(GetParam()));
  bool done_in_callback = true;

  EXPECT_EQ(0, ut_txv_control_during_own_callback(ctx, std::get<0>(GetParam()),
                                                  &done_in_callback));
  EXPECT_FALSE(done_in_callback);
  ut_txv_destroy(ctx);
}

TEST(St20TxSessionLockDeadlockTest, OwnSessionQueryFromAnotherSchedulerThread) {
  ASSERT_EQ(0, ut_txv_init());
  ut_txv_ctx* ctx = ut_txv_create();
  ASSERT_NE(nullptr, ctx);
  EXPECT_TRUE(ut_txv_own_query_from_other_scheduler_completes(ctx, 100));
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
                                           UT_TXV_GET_EMPTY, UT_TXV_UPDATE_DST),
                         ControlName);

INSTANTIATE_TEST_SUITE_P(Calls, St20TxSessionLockMigratedTest,
                         ::testing::Values(UT_TXV_UPDATE_DST), ControlName);

INSTANTIATE_TEST_SUITE_P(Calls, St20TxSessionLockRacingTest,
                         ::testing::Values(UT_TXV_UPDATE_DST), ControlName);

INSTANTIATE_TEST_SUITE_P(PublicApis, St20TxSessionLockCallbackRefusedTest,
                         ::testing::Values(UT_TXV_UPDATE_DST, UT_TXV_FREE), ControlName);

}  // namespace

std::string ControlFenceName(const ::testing::TestParamInfo<ControlFence>& info) {
  return ControlName(::testing::TestParamInfo<ut_txv_control>(std::get<0>(info.param),
                                                              info.index)) +
         (std::get<1>(info.param) ? "Fence" : "Membarrier");
}

INSTANTIATE_TEST_SUITE_P(
    Calls, St20TxSessionLockAppMutexTest,
    /* free blocks in the tx_video_session_get() of tv_mgr_detach() (GET),
     * create and migration in tx_video_session_get_empty() (GET_EMPTY) */
    ::testing::Combine(::testing::Values(UT_TXV_UPDATE_DST, UT_TXV_GET, UT_TXV_GET_EMPTY),
                       ::testing::Bool()),
    ControlFenceName);

INSTANTIATE_TEST_SUITE_P(Calls, St20TxSessionLockOwnCallbackTest,
                         ::testing::Combine(::testing::Values(UT_TXV_GET,
                                                              UT_TXV_GET_TIMEOUT,
                                                              UT_TXV_UPDATE_DST),
                                            ::testing::Bool()),
                         ControlFenceName);
