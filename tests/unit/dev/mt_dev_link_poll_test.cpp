/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_dev_link_poll(), the control path read of each port link, sets the flag that the TX
 * tasklets read with mt_dev_link_down(). A flag read makes no link call.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevLinkPollTest.*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "dev/mt_dev_harness.h"

namespace {

class MtDevLinkPollTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }
  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevLinkPollTest, PollOfALinkDownSetsTheFlag) {
  ut_dev_set_link(ctx_, false, 0);

  ut_dev_link_poll(ctx_);

  EXPECT_TRUE(ut_dev_link_down(ctx_));
  EXPECT_EQ(ut_dev_link_reads(ctx_), 1);
}

TEST_F(MtDevLinkPollTest, PollOfALinkUpClearsTheFlag) {
  ut_dev_set_link(ctx_, false, 0);
  ut_dev_link_poll(ctx_);
  ut_dev_set_link(ctx_, true, 0);

  ut_dev_link_poll(ctx_);

  EXPECT_FALSE(ut_dev_link_down(ctx_));
}

TEST_F(MtDevLinkPollTest, ReadErrorKeepsTheFlag) {
  ut_dev_set_link(ctx_, false, 0);
  ut_dev_link_poll(ctx_);
  ut_dev_set_link(ctx_, true, -ENOTSUP);

  ut_dev_link_poll(ctx_);

  EXPECT_TRUE(ut_dev_link_down(ctx_));
}

TEST_F(MtDevLinkPollTest, PortOfAnotherPmdIsNotRead) {
  ut_dev_set_link(ctx_, false, 0);
  ut_dev_set_pmd(ctx_, MTL_PMD_KERNEL_SOCKET);

  ut_dev_link_poll(ctx_);

  EXPECT_FALSE(ut_dev_link_down(ctx_));
  EXPECT_EQ(ut_dev_link_reads(ctx_), 0);
}

TEST_F(MtDevLinkPollTest, FlagReadMakesNoLinkCall) {
  ut_dev_set_link(ctx_, false, 0);
  ut_dev_link_poll(ctx_);

  int down = 0;
  for (int i = 0; i < 100; i++) down += ut_dev_link_down(ctx_);

  EXPECT_EQ(down, 100);
  EXPECT_EQ(ut_dev_link_reads(ctx_), 1);
}

}  // namespace
