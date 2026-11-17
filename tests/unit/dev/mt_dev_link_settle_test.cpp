/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include <string>

#include "dev/mt_dev_harness.h"

/* dev_link_settle(), see MT_DEV_IXGBE_LINK_SETTLE_MS in mt_dev.h. The fake clock
 * moves 10 ms per poll, so a limit of N polls ends at read N + 1. */
class MtDevLinkSettleTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_set_driver(ctx_, "net_ixgbe");
    settle_ = ut_dev_link_settle_polls();
    ASSERT_GT(settle_, 1);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  void Script(const std::string& script) {
    script_ = script;
    ut_dev_set_link_script(ctx_, script_.c_str());
  }

  ut_dev_ctx* ctx_ = nullptr;
  int settle_ = 0;
  std::string script_;
};

TEST_F(MtDevLinkSettleTest, FalseUpAfterStartWaitsForTheLinkToComeBack) {
  /* 10 polls (100 ms) of the old link, 470 polls (4.7 s) of training */
  Script(std::string(10, 'U') + std::string(470, 'D') + "U");
  EXPECT_EQ(ut_dev_link_settle(ctx_, false), 0);
  EXPECT_EQ(ut_dev_link_reads(ctx_), 10 + 470 + settle_ + 1);
}

TEST_F(MtDevLinkSettleTest, StableLinkSettlesAfterTheSettleTime) {
  Script("U");
  EXPECT_EQ(ut_dev_link_settle(ctx_, false), 0);
  EXPECT_EQ(ut_dev_link_reads(ctx_), settle_ + 1);
}

TEST_F(MtDevLinkSettleTest, FlapRestartsTheSettleTime) {
  Script(std::string(settle_ - 1, 'U') + "DU");
  EXPECT_EQ(ut_dev_link_settle(ctx_, false), 0);
  EXPECT_EQ(ut_dev_link_reads(ctx_), settle_ - 1 + 1 + settle_ + 1);
}

TEST_F(MtDevLinkSettleTest, NoLinkStopsAtTheTimeout) {
  Script("D");
  EXPECT_EQ(ut_dev_link_settle(ctx_, false), -ETIMEDOUT);
  EXPECT_EQ(ut_dev_link_reads(ctx_), ut_dev_link_settle_timeout_polls(false) + 1);
}

TEST_F(MtDevLinkSettleTest, PortAllowedDownUsesTheShortTimeout) {
  Script("D");
  EXPECT_EQ(ut_dev_link_settle(ctx_, true), -ETIMEDOUT);
  EXPECT_EQ(ut_dev_link_reads(ctx_), ut_dev_link_settle_timeout_polls(true) + 1);
  EXPECT_LT(ut_dev_link_settle_timeout_polls(true),
            ut_dev_link_settle_timeout_polls(false));
}

TEST_F(MtDevLinkSettleTest, IcePortDoesNotWait) {
  ut_dev_set_driver(ctx_, "net_ice");
  Script("U");
  EXPECT_EQ(ut_dev_link_settle(ctx_, false), 0);
  EXPECT_EQ(ut_dev_link_reads(ctx_), 0);
}

TEST_F(MtDevLinkSettleTest, PortStartTakesNoFalseUpOfIxgbe) {
  /* 2 reads of the old link, 478 of training, a stable link for the settle, then
   * down, so dev_detect_link() fails and mt_dev_create() stops there */
  Script(std::string(2, 'U') + std::string(478, 'D') + std::string(settle_ + 1, 'U') +
         "D");
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EIO);
  /* without the settle, dev_detect_link() takes reads 1 and 2 and fails at read 302 */
  EXPECT_GT(ut_dev_link_reads(ctx_), 2 + 478 + settle_);
}
