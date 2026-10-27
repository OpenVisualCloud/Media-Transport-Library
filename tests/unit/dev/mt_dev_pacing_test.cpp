/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevPacingTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  int CountQueueNodes(bool with_shaper) {
    int count = 0;
    for (int i = 0; i < ut_dev_tm_node_count(ctx_); i++) {
      ut_dev_tm_node node = ut_dev_tm_node_at(ctx_, i);
      if (node.node_id >= static_cast<uint32_t>(ut_dev_nb_tx_queues())) continue;
      if ((node.shaper_profile_id != ut_dev_tm_shaper_none()) == with_shaper) count++;
    }
    return count;
  }

  void ExpectQueuesClearedOnce() {
    EXPECT_EQ(CountQueueNodes(false), ut_dev_nb_tx_queues());
    EXPECT_EQ(CountQueueNodes(true), 0);
    EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 1);
    for (int q = 0; q < ut_dev_nb_tx_queues(); q++) {
      EXPECT_EQ(ut_dev_tx_queue_rl_mapping(ctx_, q), -1);
      EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), 0u);
    }
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevPacingTest, IavfTmTscClearsEveryQueueRate) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectQueuesClearedOnce();
}

TEST_F(MtDevPacingTest, IavfTmBeClearsEveryQueueRate) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_BE);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ExpectQueuesClearedOnce();
}

TEST_F(MtDevPacingTest, IavfTmSharedTxqClearsEveryQueueRate) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_RL);
  ut_dev_set_shared_txq(ctx_);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectQueuesClearedOnce();
}

TEST_F(MtDevPacingTest, IavfTmTscClearedQueuesResetAtTeardown) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ut_dev_set_started_iavf_tx(ctx_, false, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 2);
  EXPECT_EQ(CountQueueNodes(true), ut_dev_nb_tx_queues());
  EXPECT_EQ(CountQueueNodes(false), 0);
  EXPECT_EQ(ut_dev_last_shaper_rate(ctx_), 25000ull * 1000 * 1000 / 8);
}

/* every commit takes ~41 ms on iavf: one for all queues, not one each (#1883) */
TEST_F(MtDevPacingTest, IavfTmRlTeardownResetsEveryQueueInOneCommit) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_RL);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ut_dev_set_started_iavf_tx(ctx_, false, ST21_TX_PACING_WAY_RL);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 2);
  EXPECT_EQ(CountQueueNodes(true), ut_dev_nb_tx_queues());
  EXPECT_EQ(CountQueueNodes(false), 0);
  EXPECT_EQ(ut_dev_last_shaper_rate(ctx_), 25000ull * 1000 * 1000 / 8);
  for (int q = 0; q < ut_dev_nb_tx_queues(); q++)
    EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), 25000ull * 1000 * 1000 / 8);
}

/* bps is the committed rate: set_rl_rate() skips a queue whose bps already matches */
TEST_F(MtDevPacingTest, IavfTmRlTeardownCommitFailureKeepsQueueRates) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_RL);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ut_dev_set_started_iavf_tx(ctx_, false, ST21_TX_PACING_WAY_RL);
  ut_dev_fail_tm_commit(ctx_, -EIO);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 2);
  for (int q = 0; q < ut_dev_nb_tx_queues(); q++)
    EXPECT_EQ(ut_dev_tx_queue_bps(ctx_, q), 1024ull * 1024 * 1024 / 8);
}

TEST_F(MtDevPacingTest, IavfTmClearCommitFailureKeepsTsc) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ut_dev_fail_tm_commit(ctx_, -ENOTSUP);
  EXPECT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 1);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, IavfTmRlSetsShaperOnEveryQueue) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_RL);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_RL);
  EXPECT_EQ(CountQueueNodes(true), ut_dev_nb_tx_queues());
  EXPECT_EQ(CountQueueNodes(false), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 1);
}

TEST_F(MtDevPacingTest, NonIavfTscMakesNoTmCall) {
  ut_dev_set_pacing_port(ctx_, false, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_call_count(ctx_), 0);
}

TEST_F(MtDevPacingTest, IavfTmNodeAddFailureKeepsTsc) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ut_dev_fail_tm_node_add(ctx_, -ENOTSUP);
  EXPECT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, IavfTmAutoRlQueueNodeFailureClearsEveryQueueRate) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_AUTO);
  ut_dev_fail_tm_queue_node_add(ctx_, 3, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectQueuesClearedOnce();
}

TEST_F(MtDevPacingTest, IavfTmAutoRlCommitFailureClearsEveryQueueRate) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_AUTO);
  ut_dev_fail_tm_commit(ctx_, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  EXPECT_EQ(CountQueueNodes(false), ut_dev_nb_tx_queues());
  EXPECT_EQ(CountQueueNodes(true), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 2);
}

/* the failed rl init's profiles stay in use: the reset must not reuse their ids */
TEST_F(MtDevPacingTest, IavfTmAutoRlFallbackResetsLineRateAtTeardown) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_AUTO);
  ut_dev_fail_tm_queue_node_add(ctx_, 3, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ut_dev_set_started_iavf_tx(ctx_, false, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  EXPECT_EQ(CountQueueNodes(true), ut_dev_nb_tx_queues());
  EXPECT_EQ(ut_dev_last_shaper_rate(ctx_), 25000ull * 1000 * 1000 / 8);
}

TEST_F(MtDevPacingTest, IavfWithoutTmCapabilityMakesNoNodeAdd) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ut_dev_fail_tm_capabilities(ctx_, -ENOTSUP);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_node_count(ctx_), 0);
  EXPECT_EQ(ut_dev_tm_commit_count(ctx_), 0);
  EXPECT_EQ(ut_dev_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, IavfTmCapabilityErrorStillClears) {
  ut_dev_set_pacing_port(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ut_dev_fail_tm_capabilities(ctx_, -EINVAL);
  ASSERT_EQ(ut_dev_init_pacing(ctx_), 0);
  ExpectQueuesClearedOnce();
}
