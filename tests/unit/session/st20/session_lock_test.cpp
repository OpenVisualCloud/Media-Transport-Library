/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the RX video session lock protocol: only the control plane takes a session's
 * spinlock, then waits out the scheduler pass in flight; the RX tasklet takes no lock
 * and skips the session while it is held.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20RxSessionLock*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <tuple>

#include "session/st20_harness.h"

namespace {

class St20RxSessionLockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20_init());
    ctx_ = ut20_ctx_create_geom(1, 2);
    ASSERT_NE(nullptr, ctx_);
    ut20_lock_setup(ctx_, true);
  }
  void TearDown() override {
    ut20_ctx_destroy(ctx_);
  }
  ut20_test_ctx* ctx_ = nullptr;
};

TEST_F(St20RxSessionLockTest, SkipsASessionWhileItsLockIsHeld) {
  ut20_lock_hold(ctx_, 0, true);
  ut20_run_pkt_tasklet(ctx_);
  ut20_lock_hold(ctx_, 0, false);
  EXPECT_EQ(0, ut20_lock_bursts(ctx_, 0));
  EXPECT_EQ(1, ut20_lock_bursts(ctx_, 1));
}

/* A locked instruction per session waits for the frame copy's streaming stores. */
TEST_F(St20RxSessionLockTest, PollsASessionWithoutTakingItsLock) {
  ut20_run_pkt_tasklet(ctx_);
  EXPECT_EQ(1, ut20_lock_bursts(ctx_, 0));
  EXPECT_FALSE(ut20_lock_burst_locked(ctx_));
}

/* A writer that finds the pass long waits for the tasklet to leave its session only. */
TEST_F(St20RxSessionLockTest, PublishesTheSessionItIsOn) {
  ut20_run_pkt_tasklet(ctx_);
  EXPECT_EQ(1, ut20_lock_burst_slot(ctx_)); /* the last burst was the peer's */
  EXPECT_EQ(-1, ut20_lock_slot_now(ctx_));
}

class St20RxSessionLockCallTest : public ::testing::TestWithParam<ut20_lock_api> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20_init());
    ctx_ = ut20_ctx_create_geom(1, 2);
    ASSERT_NE(nullptr, ctx_);
    ut20_lock_setup(ctx_, true);
  }
  void TearDown() override {
    ut20_ctx_destroy(ctx_);
  }
  ut20_test_ctx* ctx_ = nullptr;
};

using St20RxSessionLockCallbackStatsTest = St20RxSessionLockCallTest;
using St20RxSessionLockCallbackRefusedTest = St20RxSessionLockCallTest;
using St20RxSessionLockPeerCallbackTest = St20RxSessionLockCallTest;

TEST_P(St20RxSessionLockCallbackStatsTest, WorkForTheCallbacksOwnSession) {
  EXPECT_EQ(0, ut20_lock_call_in_poll(ctx_, GetParam(), 0, false));
}

/* The tasklet runs the session's callbacks: a holder of its lock waits for them, so
 * spinning on the lock there never ends. */
TEST_P(St20RxSessionLockCallbackStatsTest, ReturnsBusyInsteadOfSpinningOnAWaitingHolder) {
  EXPECT_EQ(-EBUSY, ut20_lock_call_in_poll(ctx_, GetParam(), 0, true));
}

/* The queues and the session must not change under the tasklet still on them. */
TEST_P(St20RxSessionLockCallbackRefusedTest, IsRefusedEvenWithTheLockFree) {
  EXPECT_EQ(-EBUSY, ut20_lock_call_in_poll(ctx_, GetParam(), 0, false));
}

TEST_P(St20RxSessionLockPeerCallbackTest, WorksFromAPeersCallback) {
  EXPECT_EQ(0, ut20_lock_call_in_poll(ctx_, GetParam(), 1, false));
}

/* The stats dump must not drop a session while another one's poll is long. */
TEST_F(St20RxSessionLockTest, StatsDumpReachesASessionWhileAPeersPollIsLong) {
  EXPECT_TRUE(ut20_lock_stat_dump_during_peer_poll(ctx_));
}

/* nor hang on a stuck scheduler */
TEST_F(St20RxSessionLockTest, GetTimeoutGivesUpOnAPollThatNeverEnds) {
  bool locked_after = true;

  EXPECT_FALSE(ut20_lock_get_timeout_during_own_poll(ctx_, 100, &locked_after));
  EXPECT_FALSE(locked_after);
}

/* (call, whether the tasklet fences each slot store as without membarrier(2)) */
using CallFence = std::tuple<ut20_lock_api, bool>;

class St20RxSessionLockFenceTest : public ::testing::TestWithParam<CallFence> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut20_init());
    ctx_ = ut20_ctx_create_geom(1, 2);
    ASSERT_NE(nullptr, ctx_);
    ut20_lock_setup(ctx_, true);
    if (!std::get<1>(GetParam()) && ut20_lock_fence(ctx_))
      GTEST_SKIP() << "no membarrier(2)";
    ut20_lock_set_fence(ctx_, std::get<1>(GetParam()));
  }
  void TearDown() override {
    ut20_ctx_destroy(ctx_);
  }
  ut20_test_ctx* ctx_ = nullptr;
};

using St20RxSessionLockOwnPollTest = St20RxSessionLockFenceTest;
using St20RxSessionLockMovedTest = St20RxSessionLockFenceTest;
using St20RxSessionLockAppMutexTest = St20RxSessionLockFenceTest;

/* As under the per-session lock the tasklet held before: no call changes the session
 * under its poll. */
TEST_P(St20RxSessionLockOwnPollTest, WaitsUntilTheSessionsPollReturns) {
  bool done_in_poll = true;

  EXPECT_EQ(0, ut20_lock_call_during_own_poll(ctx_, std::get<0>(GetParam()), false,
                                              &done_in_poll));
  EXPECT_FALSE(done_in_poll);
}

/* A migration moves the session before it frees the old slot, so a retry finds it. */
TEST_P(St20RxSessionLockMovedTest, FindsASessionThatMovedWhileItWaited) {
  bool done_in_poll = true;

  EXPECT_EQ(0, ut20_lock_call_during_own_poll(ctx_, std::get<0>(GetParam()), true,
                                              &done_in_poll));
  EXPECT_FALSE(done_in_poll);
}

/* The call waits only while the tasklet is on its session, not for a peer's callback
 * blocked on a mutex the caller holds. */
TEST_P(St20RxSessionLockAppMutexTest, CompletesWhilePeerCallbackNeedsCallerMutex) {
  EXPECT_TRUE(
      ut20_lock_call_with_peer_on_caller_mutex(ctx_, std::get<0>(GetParam()), 100));
}

std::string CallName(ut20_lock_api call) {
  switch (call) {
    case UT20_LOCK_STATS_GET:
      return "StatsGet";
    case UT20_LOCK_STATS_RESET:
      return "StatsReset";
    case UT20_LOCK_UPDATE_SRC:
      return "UpdateSrc";
    case UT20_LOCK_FREE:
      return "Free";
    case UT20_LOCK_ST22_UPDATE_SRC:
      return "St22UpdateSrc";
    default:
      return "St22Free";
  }
}

std::string ParamName(const ::testing::TestParamInfo<ut20_lock_api>& info) {
  return CallName(info.param);
}

std::string CallFenceName(const ::testing::TestParamInfo<CallFence>& info) {
  return CallName(std::get<0>(info.param)) +
         (std::get<1>(info.param) ? "Fence" : "Membarrier");
}

INSTANTIATE_TEST_SUITE_P(Stats, St20RxSessionLockCallbackStatsTest,
                         ::testing::Values(UT20_LOCK_STATS_GET, UT20_LOCK_STATS_RESET),
                         ParamName);

INSTANTIATE_TEST_SUITE_P(PublicApis, St20RxSessionLockCallbackRefusedTest,
                         ::testing::Values(UT20_LOCK_UPDATE_SRC, UT20_LOCK_FREE,
                                           UT20_LOCK_ST22_UPDATE_SRC,
                                           UT20_LOCK_ST22_FREE),
                         ParamName);

INSTANTIATE_TEST_SUITE_P(Stats, St20RxSessionLockPeerCallbackTest,
                         ::testing::Values(UT20_LOCK_STATS_GET, UT20_LOCK_STATS_RESET),
                         ParamName);

/* the harness has no queues for update_source or free to rebuild or release */
INSTANTIATE_TEST_SUITE_P(Stats, St20RxSessionLockOwnPollTest,
                         ::testing::Combine(::testing::Values(UT20_LOCK_STATS_GET,
                                                              UT20_LOCK_STATS_RESET),
                                            ::testing::Bool()),
                         CallFenceName);

INSTANTIATE_TEST_SUITE_P(Stats, St20RxSessionLockMovedTest,
                         ::testing::Combine(::testing::Values(UT20_LOCK_STATS_GET,
                                                              UT20_LOCK_STATS_RESET),
                                            ::testing::Bool()),
                         CallFenceName);

INSTANTIATE_TEST_SUITE_P(Stats, St20RxSessionLockAppMutexTest,
                         ::testing::Combine(::testing::Values(UT20_LOCK_STATS_GET,
                                                              UT20_LOCK_STATS_RESET),
                                            ::testing::Bool()),
                         CallFenceName);

}  // namespace
