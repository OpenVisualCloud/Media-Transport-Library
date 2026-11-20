/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "dev/mt_dev_harness.h"

namespace {

/* two session rates, apart from each other and from RL init's 1 Gbit/s default */
constexpr uint64_t kRateA = 100ull * 1000 * 1000;
constexpr uint64_t kRateB = 2 * kRateA;

}  // namespace

/* A TM commit rewrites the rate of every queue on the port (iavf sends all of them to
 * the PF), which can launch the next packet 0 of a queue already streaming early. The
 * parameter is iavf (VF) or ice (PF). */
class MtDevTxQueueRateTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_set_pacing_port(ctx_, GetParam(), ST21_TX_PACING_WAY_RL);
    ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
    ASSERT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_RL);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

/* FIX-G's churn: one session is created and freed again and again, its rate alternating.
 * Each rate commits once; every later create finds a free queue at its rate. */
TEST_P(MtDevTxQueueRateTest, ReCreateAtAFreedRateMakesNoCommit) {
  const int q_a = ut_dev_get_tx_queue(ctx_, kRateA);
  ASSERT_GE(q_a, 0);
  ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q_a), 0);
  const int q_b = ut_dev_get_tx_queue(ctx_, kRateB);
  ASSERT_GE(q_b, 0);
  ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q_b), 0);
  EXPECT_NE(q_b, q_a) << "rate B took the free queue that held rate A";
  const int commits = ut_dev_tm_commit_count(ctx_);

  for (int i = 0; i < 4; i++) {
    const uint64_t rate = i % 2 ? kRateB : kRateA;
    const int q = ut_dev_get_tx_queue(ctx_, rate);
    ASSERT_EQ(q, i % 2 ? q_b : q_a) << "create " << i;
    EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), rate);
    ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q), 0);
  }
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), commits);
}

/* With no free queue at its rate, a create still commits it, overwriting a freed rate
 * once no free queue is left at the default rate. */
TEST_P(MtDevTxQueueRateTest, NewRateOnAFullPoolStillCommits) {
  const int queues = ut_dev_nb_tx_queues();
  for (int q = 0; q < queues; q++) {
    ASSERT_EQ(ut_dev_get_tx_queue(ctx_, kRateA + q), q);
    EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), kRateA + q);
  }
  for (int q = 0; q < queues; q++) ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q), 0);
  const int commits = ut_dev_tm_commit_count(ctx_);

  const int q = ut_dev_get_tx_queue(ctx_, kRateB);
  ASSERT_GE(q, 0);
  EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), kRateB);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), commits + 1);
}

/* iavf's failed commit drops every node and profile, so no queue keeps a rate to match.
 */
TEST_P(MtDevTxQueueRateTest, FailedCommitLeavesNoQueueRate) {
  const int q_a = ut_dev_get_tx_queue(ctx_, kRateA);
  const int q_b = ut_dev_get_tx_queue(ctx_, kRateB);
  ASSERT_GE(q_a, 0);
  ASSERT_GE(q_b, 0);
  ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q_a), 0);
  ASSERT_EQ(ut_dev_put_tx_queue(ctx_, q_b), 0);

  ut_dev_fail_tm_commit(ctx_, -EIO);
  ut_dev_get_tx_queue(ctx_, kRateB + kRateA);

  EXPECT_NE(ut_dev_tx_queue_bps(ctx_, q_a), kRateA);
  EXPECT_NE(ut_dev_tx_queue_bps(ctx_, q_b), kRateB);
}

INSTANTIATE_TEST_SUITE_P(IavfAndIce, MtDevTxQueueRateTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? "Iavf" : "Ice";
                         });
