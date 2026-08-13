/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevIgcTest : public testing::Test {
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

TEST_F(MtDevIgcTest, TimesyncRunsAfterQueuesBeforeStartAndAgainAfterStart) {
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_START,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_TIMESYNC_READ,
                UT_DEV_EVENT_TIMESYNC_READ});
  EXPECT_TRUE(ut_dev_port_started(ctx_));
  EXPECT_TRUE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, PreStartTimesyncFailurePreventsPortStartAndFeature) {
  ut_dev_fail_timesync_enable(ctx_, 1, -EIO);
  EXPECT_EQ(ut_dev_start_port(ctx_), -EIO);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE});
  EXPECT_FALSE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, PortStartFailurePreventsPostTimesyncAndStateChanges) {
  ut_dev_fail_port_start(ctx_, -EIO);
  EXPECT_EQ(ut_dev_start_port(ctx_), -EIO);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_START});
  EXPECT_FALSE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, IgcWithoutTimingStartsWithoutTimesync) {
  ut_dev_set_ptp_enabled(ctx_, false);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_PORT_START});
  EXPECT_TRUE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, NonIgcPortDoesNotUseIgcTimesyncOrdering) {
  ut_dev_use_non_igc_driver(ctx_);
  ASSERT_EQ(ut_dev_start_port(ctx_), 0);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_PORT_START});
  EXPECT_TRUE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, PostStartTimesyncFailureLeavesFeatureUnset) {
  ut_dev_fail_timesync_enable(ctx_, 2, -EIO);
  EXPECT_EQ(ut_dev_start_port(ctx_), -EIO);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_START,
                UT_DEV_EVENT_TIMESYNC_ENABLE});
  EXPECT_TRUE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, PostStartTimesyncReadFailureLeavesFeatureUnset) {
  ut_dev_fail_timesync_read(ctx_, 1, -EIO);
  EXPECT_EQ(ut_dev_start_port(ctx_), -EIO);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_START,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_TIMESYNC_READ});
  EXPECT_TRUE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIgcTest, InitCleanupStopsPortAfterPostStartTimesyncFailure) {
  ut_dev_fail_timesync_enable(ctx_, 2, -EIO);
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EIO);
  ExpectEvents({UT_DEV_EVENT_RX_QUEUE_SETUP, UT_DEV_EVENT_TX_QUEUE_SETUP,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_START,
                UT_DEV_EVENT_TIMESYNC_ENABLE, UT_DEV_EVENT_PORT_STOP});
  EXPECT_FALSE(ut_dev_port_started(ctx_));
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}