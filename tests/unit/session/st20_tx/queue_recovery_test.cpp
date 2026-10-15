/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins st20_tx_queue_fatal_error() of a redundant session. Nothing tells a hung R leg
 * from a healthy P, so both recover, and P's recovery flushes R first, inside the
 * tasklet. On a deep port that flush is bounded: where R did not drain in time, or there
 * was no mbuf for the flush's mark, R's queue may still reference the frames and pool
 * mbufs, so the session ends unrecoverable before it hands a frame back. These sessions
 * are on the mono pool, which tv_mempool_free() never refuses to replace.
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

}  // namespace
