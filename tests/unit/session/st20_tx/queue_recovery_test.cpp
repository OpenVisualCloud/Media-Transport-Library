/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins st20_tx_queue_fatal_error() of a redundant session. Nothing tells a hung R leg
 * from a healthy P, so both recover, and P's recovery flushes R first, inside the
 * tasklet. On a deep port that flush is bounded: where R did not drain in time, or there
 * was no mbuf for the flush's mark, R's queue may still reference the frames and pool
 * mbufs, so the session ends unrecoverable before it hands a frame back. These sessions
 * are on the mono pool, which tv_mempool_free() never refuses to replace. A new queue
 * keeps the rate the leg trained on.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxQueueRecoveryTest.*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "session/st20_tx_harness.h"

namespace {

constexpr int kP = 0;
constexpr int kR = 1;
constexpr uint64_t kStartNs = 1000000000;
/* a parked pool is older than an app call that read it */
constexpr uint64_t kParkNs = 1000000001;

class St20TxQueueRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_mock_tsc_time(ctx_, kStartNs);
    ASSERT_EQ(ut_txv_recovery_setup(ctx_, 2), 0);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxQueueRecoveryTest, FlushedOtherLegLetsTheFramesBack) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_FALSE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_recovered_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 0);
}

/* a pre-arm that kept its last burst back for packet 0 leaves nothing to the new queue */
TEST_F(St20TxQueueRecoveryTest, RecoveryDropsTheKeptBackTail) {
  ut_txv_set_rl_tail(ctx_, kP, true, kStartNs);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  long double drained = -1;
  EXPECT_FALSE(ut_txv_rl_tail(ctx_, kP, &drained));
  EXPECT_EQ(drained, 0);
}

TEST_F(St20TxQueueRecoveryTest, LegItsOtherLegFlushedRunsItsOwnDoneCleanup) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);
  ASSERT_EQ(ut_txv_recovery_flushes(ctx_, kR), 1);

  EXPECT_EQ(ut_txv_recover(ctx_, kR), 0);

  EXPECT_EQ(ut_txv_recovery_done_cleanups(ctx_, kR), 1);
}

/* inside the tasklet, without the per-pad loop: up to 2 x nb_tx_desc ms on a hung ring */
TEST_F(St20TxQueueRecoveryTest, OtherLegIsFlushedWithoutThePerPadFallback) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_recovery_flushes(ctx_, kR), 1);
  EXPECT_EQ(ut_txv_recovery_fallback_flushes(ctx_, kR), 0);
}

class St20TxQueueRecoveryFlushFailTest : public St20TxQueueRecoveryTest,
                                         public ::testing::WithParamInterface<int> {};

TEST_P(St20TxQueueRecoveryFlushFailTest, OtherLegStillHoldingTheFramesEndsTheSession) {
  ut_txv_recovery_set_flush_ret(ctx_, kR, GetParam());

  EXPECT_EQ(ut_txv_recover(ctx_, kP), GetParam());

  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_recovered_events(ctx_), 0);
  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 1);
}

INSTANTIATE_TEST_SUITE_P(Ret, St20TxQueueRecoveryFlushFailTest,
                         ::testing::Values(-EBUSY, -ENOMEM),
                         [](const ::testing::TestParamInfo<int>& info) {
                           return info.param == -EBUSY ? "NotDrained" : "NoMark";
                         });

/* The leg trained its pad interval and plan at its queue's rate, which a later session's
 * retrain of tv_rl_bps(), the one tv_init_hw() would program now, must not change. */
TEST_F(St20TxQueueRecoveryTest, RecoveryKeepsTheQueueRateTheLegTrainedOn) {
  constexpr uint64_t kTrainedBps = 300000000;
  ut_txv_recovery_set_queue_bps(ctx_, kP, kTrainedBps);
  ASSERT_EQ(ut_txv_add_trained_bps(ctx_, kTrainedBps + 3000000), 0);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_txq_get_bps(ctx_), kTrainedBps);
}

/* a queue without a programmed rate, a shared one: the rate tv_init_hw() asks for */
TEST_F(St20TxQueueRecoveryTest, RecoveryOfAQueueWithoutARateLooksItUp) {
  constexpr uint64_t kRetrainedBps = 303000000;
  ut_txv_recovery_set_queue_bps(ctx_, kP, 0);
  ASSERT_EQ(ut_txv_add_trained_bps(ctx_, kRetrainedBps), 0);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_txq_get_bps(ctx_), kRetrainedBps);
}

/* the dropped pkts leave a seq gap that the RTCP ring of each leg would refuse */
TEST_F(St20TxQueueRecoveryTest, RecoveryRestartsTheRtcpRingOfEachLeg) {
  ut_txv_set_rtcp_legs(ctx_);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_rtcp_resets(ctx_, kP), 1);
  EXPECT_EQ(ut_txv_rtcp_resets(ctx_, kR), 1);
}

TEST_F(St20TxQueueRecoveryTest, LinkResyncRestartsTheRtcpRingOfEachLeg) {
  ut_txv_set_rtcp_legs(ctx_);

  ut_txv_link_resync(ctx_);

  EXPECT_EQ(ut_txv_rtcp_resets(ctx_, kP), 1);
  EXPECT_EQ(ut_txv_rtcp_resets(ctx_, kR), 1);
}

/* the dead queue keeps chain mbufs of the frame: they count on a retired sh_info */
TEST_F(St20TxQueueRecoveryTest, FrameHeldByTheDeadQueueIsRetiredAndHandedBack) {
  ut_txv_recovery_set_sh_info(ctx_, 1, 0);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_frame_retired(ctx_));
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 0);
}

/* a dead queue of an earlier recovery holds another sh_info: the frame counts on a free
 * one, as at the second link loss of an E610 */
TEST_F(St20TxQueueRecoveryTest, FrameHeldByASecondDeadQueueIsRetiredToo) {
  ut_txv_recovery_set_sh_info(ctx_, 1, 1);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_frame_retired(ctx_));
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 0);
}

/* dead queues of earlier recoveries hold every other sh_info: no queue can free one, so
 * another recovery would only take one more queue a hang period */
TEST_F(St20TxQueueRecoveryTest, FrameHeldByDeadQueuesOnEverySh_infoEndsTheSession) {
  ut_txv_recovery_set_sh_info(ctx_, 1, ut_txv_frame_sh_info_max() - 1);
  int gets = ut_txv_recovery_txq_gets(ctx_);

  EXPECT_LT(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_recovered_events(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_txq_gets(ctx_), gets);
}

/* a direct chain recovery that fails gives back the count it took on each frame, so the
 * session free frees the frames once the queues give their mbufs back */
TEST_P(St20TxQueueRecoveryFlushFailTest,
       FailedDirectChainRecoveryKeepsNoCountOnTheFrame) {
  ut_txv_recovery_set_sh_info(ctx_, 1, 0);
  ut_txv_recovery_set_direct_chain(ctx_);
  ut_txv_recovery_set_flush_ret(ctx_, kR, GetParam());

  EXPECT_EQ(ut_txv_recover(ctx_, kP), GetParam());

  EXPECT_EQ(ut_txv_recovery_sh_cur_count(ctx_), 1u);
}

/* A hung queue completes no descriptor, so its ring still holds mbufs of the session
 * pools when the recovery replaces them. The recovery makes the new pools first, so the
 * app thread never reads a NULL pool, then parks each old one until its mbufs are back.
 * With no room left to park, the session ends with its old pools. */
class St20TxQueueRecoveryOwnedPoolTest : public St20TxQueueRecoveryTest {
 protected:
  void SetUp() override {
    St20TxQueueRecoveryTest::SetUp();
    ASSERT_EQ(ut_txv_recovery_own_pools(ctx_), 0);
  }
};

TEST_F(St20TxQueueRecoveryOwnedPoolTest, PoolTheDeadQueueStillUsesDoesNotEndTheSession) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);

  EXPECT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_FALSE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_recovered_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 0);
  EXPECT_TRUE(ut_txv_recovery_pool_replaced(ctx_));
  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_))
      << "a pool with mbufs in the dead queue must not be freed";
}

TEST_F(St20TxQueueRecoveryOwnedPoolTest, PoolsAreNeverNullDuringARecovery) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_recovery_null_pools(ctx_), 0);
}

TEST_F(St20TxQueueRecoveryOwnedPoolTest, ParkedPoolIsFreedOnceItsMbufsAreBack) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  ut_txv_recovery_release_mbufs(ctx_);
  ut_txv_set_mock_tsc_time(ctx_, kStartNs + kParkNs);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_FALSE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* the app thread may still hold the old pool: it goes at the next recovery */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, IdlePoolIsFreedAtTheNextRecovery) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);
  ASSERT_TRUE(ut_txv_recovery_pool_replaced(ctx_));
  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_));
  ut_txv_set_mock_tsc_time(ctx_, kStartNs + kParkNs);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_FALSE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* the stats dump, with the tasklets of the session held off, a stat period after the
 * app thread last read the old pool, frees a parked pool whose mbufs are back */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, ParkedIdlePoolIsFreedAtTheStatsDump) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);
  ut_txv_recovery_release_mbufs(ctx_);
  ut_txv_set_mock_tsc_time(ctx_, kStartNs + kParkNs);
  int locked_lines = 0;

  ut_txv_run_sessions_stat(ctx_, &locked_lines);

  EXPECT_FALSE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* rte_mempool_free() and its log run with the tasklets of the session free to go */
TEST_F(St20TxQueueRecoveryOwnedPoolTest,
       StatsDumpFreesTheParkedPoolOutsideTheSessionLock) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);
  ut_txv_recovery_release_mbufs(ctx_);
  ut_txv_set_mock_tsc_time(ctx_, kStartNs + kParkNs);
  int locked_lines = 0;

  ut_txv_run_sessions_stat(ctx_, &locked_lines);

  ASSERT_FALSE(ut_txv_recovery_first_pool_alive(ctx_));
  EXPECT_EQ(ut_txv_stat_locked_pool_frees(), 0);
}

/* the app thread may have read the pool just before it parked: a recovery right after,
 * of the other leg in the same transmitter call, leaves it */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, PoolParkedJustBeforeStaysAtTheNextRecovery) {
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* the session uninit frees a parked pool whose mbufs are back */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, ParkedIdlePoolIsFreedAtUninit) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);
  ut_txv_recovery_release_mbufs(ctx_);

  EXPECT_EQ(ut_txv_mempool_free(ctx_), 0);

  EXPECT_FALSE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* an mbuf of it can still go back, so the pool stays allocated */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, ParkedPoolInUseIsLeftAtUninit) {
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_mempool_free(ctx_), -EBUSY);

  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* 2 header pools a recovery: 8 recoveries fill the 16 parked places */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, NoPlaceToParkEndsTheSessionWithItsPools) {
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);
    ASSERT_EQ(ut_txv_recover(ctx_, kP), 0) << "recovery " << i;
  }
  ASSERT_EQ(ut_txv_recovery_hold_mbufs(ctx_), 0);

  EXPECT_LT(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 1);
  EXPECT_TRUE(ut_txv_recovery_pools_set(ctx_));
}

/* the second header pool fails: the first new one was never published and goes cleanly,
 * while the old ones stay intact for lock-free app readers */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, FailedPoolCreateNeverFreesAPublishedPool) {
  ut_txv_recovery_fail_create(ctx_, 2);

  EXPECT_LT(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_recovery_live_pool_frees(ctx_), 0);
  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_TRUE(ut_txv_recovery_pools_set(ctx_));
  EXPECT_FALSE(ut_txv_recovery_pool_replaced(ctx_));
  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* the second header pool fails: the first new one goes, the old ones stay */
TEST_F(St20TxQueueRecoveryOwnedPoolTest, FailedPoolCreateEndsTheSessionWithItsPools) {
  ut_txv_recovery_fail_create(ctx_, 2);

  EXPECT_LT(ut_txv_recover(ctx_, kP), 0);

  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_TRUE(ut_txv_recovery_pools_set(ctx_));
  EXPECT_FALSE(ut_txv_recovery_pool_replaced(ctx_));
  EXPECT_TRUE(ut_txv_recovery_first_pool_alive(ctx_));
}

/* recovery hands an EXT frame back through tv_frame_put(): addr and refcnt are cleared
 * before notify_frame_done(), so the callback can re-arm the frame right away */
TEST_F(St20TxQueueRecoveryTest, ExtFrameRecoveredCanBeRearmedInDoneCallback) {
  uint8_t buf_a[UT_TXV_FRAME_SIZE] = {};
  uint8_t buf_b[UT_TXV_FRAME_SIZE] = {};
  ut_txv_recovery_set_ext_frame(ctx_, buf_a, buf_b);

  ASSERT_EQ(ut_txv_recover(ctx_, kP), 0);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_done_refcnt(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_done_addr(ctx_), nullptr);
  EXPECT_EQ(ut_txv_recovery_done_saved_addr(ctx_), buf_a);
  EXPECT_FALSE(ut_txv_recovery_done_link_held(ctx_));
  EXPECT_EQ(ut_txv_rearm_ret(ctx_), 0);
}

/* tv_link_release_held() hands an EXT frame back through tv_frame_put() too */
TEST_F(St20TxQueueRecoveryTest,
       ExtFrameReleasedAfterLinkDrainCanBeRearmedInDoneCallback) {
  uint8_t buf_a[UT_TXV_FRAME_SIZE] = {};
  uint8_t buf_b[UT_TXV_FRAME_SIZE] = {};
  ut_txv_recovery_set_ext_frame(ctx_, buf_a, buf_b);

  ut_txv_link_release_held(ctx_);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_done_refcnt(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_done_addr(ctx_), nullptr);
  EXPECT_EQ(ut_txv_recovery_done_saved_addr(ctx_), buf_a);
  EXPECT_FALSE(ut_txv_recovery_done_link_held(ctx_));
  EXPECT_EQ(ut_txv_rearm_ret(ctx_), 0);
}

/* when every other sh_info slot is busy, tv_link_release_held() leaves link_pins and
 * sh_cur refcnt intact instead of dropping pins before failing to retire */
TEST_F(St20TxQueueRecoveryTest, LinkReleaseHeldKeepsPinsWhenEveryShInfoIsBusy) {
  ut_txv_recovery_set_sh_info(ctx_, 2, ut_txv_frame_sh_info_max() - 1);
  ut_txv_recovery_set_link_held(ctx_, 1u << kP);

  ut_txv_link_release_held(ctx_);

  EXPECT_EQ(ut_txv_notify_frame_done_calls(ctx_), 0);
  EXPECT_EQ(ut_txv_recovery_frame_refcnt(ctx_), 1);
  EXPECT_TRUE(ut_txv_recovery_link_held(ctx_));
  EXPECT_EQ(ut_txv_recovery_link_pins(ctx_), 1u << kP);
  EXPECT_EQ(ut_txv_recovery_sh_cur_count(ctx_), 2u);
}

/* once port 0 recovery fails fatally, the transmitter skips port 1 recovery and later
 * pacing calls on the dead session */
TEST_F(St20TxQueueRecoveryTest,
       TransmitterStopsAfterFatalRecoveryWithoutRunningOtherLeg) {
  ut_txv_recovery_set_sh_info(ctx_, 1, ut_txv_frame_sh_info_max() - 1);

  int pacing_calls = ut_txv_recovery_run_transmitter(ctx_, true, true);

  EXPECT_TRUE(ut_txv_recovery_session_dead(ctx_));
  EXPECT_EQ(ut_txv_recovery_fatal_events(ctx_), 1);
  EXPECT_EQ(ut_txv_recovery_recovered_events(ctx_), 0);
  EXPECT_EQ(pacing_calls, 0);
}

}  // namespace
