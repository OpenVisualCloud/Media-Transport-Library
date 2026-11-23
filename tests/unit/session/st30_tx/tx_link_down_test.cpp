/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The audio manager hang detector reads the link flag of the port. The queue of a port
 * with no link is not hung: a failed burst restarts the hang time, and the pkt stays for
 * the next burst.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St30TxLinkDownTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30_tx_harness.h"

namespace {

constexpr int kPort = 0;
constexpr uint64_t kThreshNs = 1000;

class St30TxLinkDownTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa30_init(), 0);
    ctx_ = ut_txa30_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txa30_destroy(ctx_);
  }
  uint16_t FailAt(uint64_t tsc_ns) {
    ut_txa30_set_mock_tsc_time(ctx_, tsc_ns);
    return ut_txa30_trs_burst_fail(ctx_, kPort, kThreshNs);
  }
  ut_txa30_ctx* ctx_ = nullptr;
};

TEST_F(St30TxLinkDownTest, HangWithTheLinkDownKeepsThePkt) {
  ut_txa30_set_link_down(ctx_, true);

  EXPECT_EQ(FailAt(5000), 0);
  EXPECT_EQ(ut_txa30_last_burst_succ_tsc(ctx_, kPort), 5000u);
}

TEST_F(St30TxLinkDownTest, HangWithTheLinkUpTakesTheRecovery) {
  EXPECT_EQ(FailAt(5000), 1);
}

TEST_F(St30TxLinkDownTest, LinkUpWithTheQueueStillHungRecoversAThresholdLater) {
  ut_txa30_set_link_down(ctx_, true);
  FailAt(5000);
  ut_txa30_set_link_down(ctx_, false);

  EXPECT_EQ(FailAt(5000 + kThreshNs / 2), 0);
  EXPECT_EQ(FailAt(5000 + 2 * kThreshNs), 1);
}

TEST_F(St30TxLinkDownTest, LinkDownDropDoesNotCountAsTransmittedBurst) {
  int bursts = -1, drops = -1;

  ASSERT_EQ(ut_txa30_trs_link_down_drop_stats(ctx_, &bursts, &drops), 0);

  EXPECT_EQ(drops, 1);
  EXPECT_EQ(bursts, 0);
}

}  // namespace
