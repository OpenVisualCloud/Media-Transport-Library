/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevRlTeardownTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  void ExpectEvents(std::initializer_list<ut_dev_event> expected) {
    ASSERT_EQ(ut_dev_event_count(ctx_), static_cast<int>(expected.size()));
    int index = 0;
    for (ut_dev_event event : expected) EXPECT_EQ(ut_dev_event_at(ctx_, index++), event);
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevRlTeardownTest, IavfFallenBackToTscResetsTxQueuesBeforeStop) {
  ut_dev_set_started_iavf_tx(ctx_, true, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_TX_RL_COMMIT, UT_DEV_EVENT_PORT_STOP});
  EXPECT_EQ(ut_dev_last_shaper_rate(ctx_), 25000ull * 1000 * 1000 / 8);
}

TEST_F(MtDevRlTeardownTest, IavfWithoutRlRootStopsWithoutTxQueueReset) {
  ut_dev_set_started_iavf_tx(ctx_, false, ST21_TX_PACING_WAY_TSC);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_PORT_STOP});
}

TEST_F(MtDevRlTeardownTest, NonIavfPortWithRlRootStopsWithoutTxQueueReset) {
  ut_dev_set_started_iavf_tx(ctx_, true, ST21_TX_PACING_WAY_RL);
  ut_dev_use_non_igc_driver(ctx_);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_PORT_STOP});
}

TEST_F(MtDevRlTeardownTest, StoppedIavfPortWithRlRootSkipsTxQueueReset) {
  ut_dev_set_started_iavf_tx(ctx_, true, ST21_TX_PACING_WAY_RL);
  ut_dev_set_port_stopped(ctx_);
  ASSERT_EQ(ut_dev_free_ports(ctx_), 0);
  ExpectEvents({});
}
