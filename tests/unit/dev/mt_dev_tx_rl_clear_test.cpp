/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

namespace {

constexpr uint16_t kTxQueues = 4;

class MtDevTxRlClearTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_set_driver(ctx_, "net_iavf");
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  void ExpectEveryTxQueueCommittedAt(uint64_t bytes_per_sec) {
    for (uint16_t q = 0; q < kTxQueues; q++) {
      uint64_t rate = 0;
      ASSERT_TRUE(ut_dev_tm_committed_rate(ctx_, q, &rate)) << "queue " << q;
      EXPECT_EQ(rate, bytes_per_sec) << "queue " << q;
    }
  }

  ut_dev_ctx* ctx_ = nullptr;
};

class MtDevTxRlClearQosProbeTest : public MtDevTxRlClearTest {
 protected:
  void SetUp() override {
    MtDevTxRlClearTest::SetUp();
    if (!ut_dev_dpdk_iavf_commit_checks_qos_first())
      GTEST_SKIP() << "iavf TM ops can crash without QoS before DPDK 26.07";
  }
};

class MtDevTxRlClearNonRlTest
    : public MtDevTxRlClearQosProbeTest,
      public testing::WithParamInterface<enum st21_tx_pacing_way> {};

TEST_P(MtDevTxRlClearNonRlTest, IavfCommitsEveryTxQueueWithoutRateLimit) {
  ASSERT_EQ(ut_dev_init_pacing(ctx_, GetParam(), kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), GetParam());
  ExpectEveryTxQueueCommittedAt(0);
}

INSTANTIATE_TEST_SUITE_P(PacingWays, MtDevTxRlClearNonRlTest,
                         testing::Values(ST21_TX_PACING_WAY_TSC, ST21_TX_PACING_WAY_PTP,
                                         ST21_TX_PACING_WAY_BE,
                                         ST21_TX_PACING_WAY_TSC_NARROW));

TEST_F(MtDevTxRlClearQosProbeTest, IavfSharedTxQueueCommitsEveryTxQueueWithoutRateLimit) {
  ut_dev_enable_shared_tx_queue(ctx_);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_AUTO, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectEveryTxQueueCommittedAt(0);
}

TEST_F(MtDevTxRlClearTest, IavfRlCommitsEveryTxQueueAtDefaultRate) {
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_RL, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_RL);
  ExpectEveryTxQueueCommittedAt(ut_dev_default_rl_bps());
}

TEST_F(MtDevTxRlClearTest, IavfWithoutPfQosMakesNoTmCallThatWouldCrash) {
  ut_dev_tm_set_vf_without_qos(ctx_);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_TSC, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  EXPECT_EQ(ut_dev_tm_calls_needing_qos(ctx_), 0);
}

TEST_F(MtDevTxRlClearQosProbeTest, IavfClearFailureDoesNotFailTscInit) {
  ut_dev_tm_fail_queue_node_add(ctx_, 1, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_TSC, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevTxRlClearQosProbeTest,
       IavfAutoRlQueueNodeFailureFallsBackToTscWithoutRateLimit) {
  ut_dev_tm_fail_queue_node_add(ctx_, 3, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_AUTO, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectEveryTxQueueCommittedAt(0);
}

TEST_F(MtDevTxRlClearQosProbeTest,
       IavfAutoRlCommitFailureFallsBackToTscWithoutRateLimit) {
  ut_dev_tm_fail_commit_once(ctx_, -EIO);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_AUTO, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_), ST21_TX_PACING_WAY_TSC);
  ExpectEveryTxQueueCommittedAt(0);
}

TEST_F(MtDevTxRlClearTest, IavfBeforeDpdk2607MakesNoTmCallOnTsc) {
  if (ut_dev_dpdk_iavf_commit_checks_qos_first())
    GTEST_SKIP() << "DPDK 26.07 or later clears the queue rates";
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_TSC, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tm_calls(ctx_), 0);
}

TEST_F(MtDevTxRlClearTest, IcePfTscLeavesTmUntouched) {
  ut_dev_set_driver(ctx_, "net_ice");
  ASSERT_EQ(ut_dev_init_pacing(ctx_, ST21_TX_PACING_WAY_TSC, kTxQueues), 0);
  EXPECT_EQ(ut_dev_tm_calls(ctx_), 0);
}

}  // namespace
