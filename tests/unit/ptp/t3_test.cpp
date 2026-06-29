/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the async delay_req tx-timestamp (t3) state machine that runs on the
 * EAL alarm thread: the sequence guard that drops a stale alarm, the deadline
 * give-up that clears t3_pending and counts a timeout, and the single-owner
 * claim that makes a cycle's result apply exactly once when t3 and t4 complete
 * concurrently on two threads.
 *
 * Run: ./build/tests/unit/UnitTest --gtest_filter='PtpT3*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PtpT3Test : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_ptp_set_read_tx_ret(0); /* restore default for the next test */
    ut_ptp_set_read_tx_ns(0);
    ut_ptp_destroy(ctx_);
  }
  ut_ptp_ctx* ctx_ = nullptr;
};

/* A pending read whose sequence id no longer matches the latest delay_req is a
 * stale alarm: the handler clears t3_pending and returns without reading a
 * timestamp or counting a timeout. */
TEST_F(PtpT3Test, SequenceGuardDropsStaleAlarm) {
  ut_ptp_set_t3_sequence_id(ctx_, 7);
  ut_ptp_set_t3_pending(ctx_, true, 6); /* armed for an older delay_req */

  ut_ptp_run_t3_handler(ctx_);

  EXPECT_FALSE(ut_ptp_t3_pending(ctx_));
  EXPECT_EQ(ut_ptp_t3(ctx_), 0u);             /* no timestamp accepted */
  EXPECT_EQ(ut_ptp_stat_t3_timeout(ctx_), 0); /* not a deadline timeout */
}

/* Once the deadline has passed and the tx timestamp is still not ready, the
 * handler gives up: it clears t3_pending and increments the timeout counter so
 * the next sync starts clean. */
TEST_F(PtpT3Test, DeadlineGiveUpClearsPendingAndCounts) {
  ut_ptp_set_no_timesync(ctx_, false); /* route the read into the override */
  ut_ptp_set_read_tx_ret(-1);          /* tx timestamp not ready */
  ut_ptp_set_t3_sequence_id(ctx_, 5);
  ut_ptp_set_t3_pending(ctx_, true, 5);
  ut_ptp_set_t2(ctx_, 1000);
  ut_ptp_set_t3_deadline_ns(ctx_, 1); /* already in the past */

  ut_ptp_run_t3_handler(ctx_);

  EXPECT_FALSE(ut_ptp_t3_pending(ctx_));
  EXPECT_EQ(ut_ptp_stat_t3_timeout(ctx_), 1);
  EXPECT_EQ(ut_ptp_t3(ctx_), 0u);
}

/* A successfully read tx timestamp that predates t2 is a stale value latched by
 * an earlier cycle (the single HW slot is identity-less). It must be rejected
 * rather than accepted as t3, so a stale read on the deadline gives up instead
 * of feeding the implausible value into the result before lock. */
TEST_F(PtpT3Test, StaleTimestampBeforeT2Rejected) {
  ut_ptp_set_no_timesync(ctx_, false);
  ut_ptp_set_read_tx_ret(0);  /* read "succeeds" ... */
  ut_ptp_set_read_tx_ns(500); /* ... but returns a timestamp older than t2 */
  ut_ptp_set_t2(ctx_, 1000);
  ut_ptp_set_t3_sequence_id(ctx_, 9);
  ut_ptp_set_t3_pending(ctx_, true, 9);
  ut_ptp_set_t3_deadline_ns(ctx_, 1); /* already in the past */

  ut_ptp_run_t3_handler(ctx_);

  EXPECT_EQ(ut_ptp_t3(ctx_), 0u); /* stale value not accepted */
  EXPECT_FALSE(ut_ptp_t3_pending(ctx_));
  EXPECT_EQ(ut_ptp_stat_t3_timeout(ctx_), 1); /* went to give-up, not success */
}

/* The core race fix: when all four timestamps are present, two concurrent
 * completion attempts (t3 on the alarm thread, t4 on the rx thread) must apply
 * the result exactly once. The second attempt -- modelling the loser thread
 * that read the four stamps before the winner cleared them -- is replayed here
 * by restoring the timestamps without resetting the claim; it must be a no-op.
 */
TEST_F(PtpT3Test, ResultAppliedExactlyOncePerCycle) {
  ut_ptp_set_t1(ctx_, 1000);
  ut_ptp_set_t2(ctx_, 2000);
  ut_ptp_set_t3(ctx_, 3000);
  ut_ptp_set_t4(ctx_, 4000);

  ut_ptp_try_complete_result(ctx_); /* winner consumes the cycle */
  ASSERT_EQ(ut_ptp_stat_delta_cnt(ctx_), 1);
  EXPECT_EQ(ut_ptp_result_claimed(ctx_), 1);

  /* loser thread: it had observed all four stamps before the winner cleared
   * them, so replay that view without a fresh cycle (no claim reset) */
  ut_ptp_set_t1(ctx_, 1000);
  ut_ptp_set_t2(ctx_, 2000);
  ut_ptp_set_t3(ctx_, 3000);
  ut_ptp_set_t4(ctx_, 4000);

  ut_ptp_try_complete_result(ctx_); /* must NOT apply the result again */
  EXPECT_EQ(ut_ptp_stat_delta_cnt(ctx_), 1);
  EXPECT_EQ(ut_ptp_result_claimed(ctx_), 1);
}
