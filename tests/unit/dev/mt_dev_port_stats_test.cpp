/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevPortStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_use_sw_stats(ctx_);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevPortStatsTest, GetCopiesUnderTheStatsLock) {
  struct mtl_port_status stats = {};

  ut_dev_stats_writer_start_on_unlock(ctx_);
  ASSERT_EQ(ut_dev_get_port_stats(ctx_, &stats), 0);
  ASSERT_EQ(ut_dev_stats_writer_join(ctx_), 0);

  EXPECT_EQ(stats.rx_packets, UT_DEV_STATS_WRITER_STEP);
  EXPECT_EQ(stats.tx_packets, UT_DEV_STATS_WRITER_STEP);
}

TEST_F(MtDevPortStatsTest, ResetWaitsForTheStatsLock) {
  struct mtl_port_status stats = {};

  ASSERT_EQ(ut_dev_stats_writer_start(ctx_), 0);
  ASSERT_EQ(ut_dev_reset_port_stats(ctx_), 0);
  ASSERT_EQ(ut_dev_stats_writer_join(ctx_), 0);

  ut_dev_user_port_stats(ctx_, &stats);
  EXPECT_EQ(stats.rx_packets, 0u);
  EXPECT_EQ(stats.tx_packets, 0u);
}
