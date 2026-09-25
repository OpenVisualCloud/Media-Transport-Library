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
    ut_dev_set_tx_pacing_way(ctx_, ST21_TX_PACING_WAY_TSN);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevPacingTest, RxOnlyPortWithoutLaunchTimePacesWithTsc) {
  ASSERT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_, MTL_PORT_P), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, RxOnlyPortWithLaunchTimePacesWithTsc) {
  ut_dev_enable_launch_time(ctx_, MTL_PORT_P);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_, MTL_PORT_P), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, RxOnlyPortAcceptsTsnWithoutPtp) {
  ut_dev_enable_launch_time(ctx_, MTL_PORT_P);
  ut_dev_set_ptp_enabled(ctx_, false);
  EXPECT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), 0);
}

TEST_F(MtDevPacingTest, RxOnlyPortBesideTxPortPacesWithTsc) {
  ut_dev_set_num_ports(ctx_, 2);
  ut_dev_set_tx_queues_cnt(ctx_, MTL_PORT_P, 1);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_R), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_, MTL_PORT_R), ST21_TX_PACING_WAY_TSC);
}

TEST_F(MtDevPacingTest, TxPortWithoutLaunchTimeRejectsTsn) {
  ut_dev_set_tx_queues_cnt(ctx_, MTL_PORT_P, 1);
  EXPECT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), -EINVAL);
}

TEST_F(MtDevPacingTest, DeprecatedTxSessionsMaxMakesPortTx) {
  ut_dev_set_tx_sessions_cnt_max(ctx_, 1);
  EXPECT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), -EINVAL);
}

TEST_F(MtDevPacingTest, TxPortWithLaunchTimeKeepsTsn) {
  ut_dev_set_tx_queues_cnt(ctx_, MTL_PORT_P, 1);
  ut_dev_enable_launch_time(ctx_, MTL_PORT_P);
  ASSERT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), 0);
  EXPECT_EQ(ut_dev_tx_pacing_way(ctx_, MTL_PORT_P), ST21_TX_PACING_WAY_TSN);
}

TEST_F(MtDevPacingTest, TxPortWithLaunchTimeRequiresPtp) {
  ut_dev_set_tx_queues_cnt(ctx_, MTL_PORT_P, 1);
  ut_dev_enable_launch_time(ctx_, MTL_PORT_P);
  ut_dev_set_ptp_enabled(ctx_, false);
  EXPECT_EQ(ut_dev_init_pacing(ctx_, MTL_PORT_P), -EINVAL);
}
