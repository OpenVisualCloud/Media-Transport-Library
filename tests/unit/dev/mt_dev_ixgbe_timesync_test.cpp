/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

/* ixgbe_dev_start() resets the MAC, which clears the timesync registers, so
 * mt_dev_create() must not enable timesync on an ixgbe PF before the port
 * start. A port start failure stops mt_dev_create() before the after-start
 * enable, so the events show only what runs before the port start. */
class MtDevIxgbeTimesyncTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
    ut_dev_fail_port_start(ctx_, -EIO);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  int TimesyncEnablesBeforePortStart() {
    int count = 0;
    for (int i = 0; i < ut_dev_event_count(ctx_); i++) {
      ut_dev_event event = ut_dev_event_at(ctx_, i);
      if (event == UT_DEV_EVENT_PORT_START) return count;
      if (event == UT_DEV_EVENT_TIMESYNC_ENABLE) count++;
    }
    return -1; /* no port start */
  }

  int TimesyncEnablesAfterPortStart() {
    int count = 0;
    bool started = false;
    for (int i = 0; i < ut_dev_event_count(ctx_); i++) {
      ut_dev_event event = ut_dev_event_at(ctx_, i);
      if (event == UT_DEV_EVENT_PORT_START) started = true;
      if (started && event == UT_DEV_EVENT_TIMESYNC_ENABLE) count++;
    }
    return count;
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevIxgbeTimesyncTest, IxgbePfEnablesNoTimesyncBeforePortStart) {
  ut_dev_set_driver(ctx_, "net_ixgbe");
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EIO);
  EXPECT_EQ(TimesyncEnablesBeforePortStart(), 0);
  EXPECT_FALSE(ut_dev_timesync_feature(ctx_));
}

TEST_F(MtDevIxgbeTimesyncTest, IcePfStillEnablesTimesyncBeforePortStart) {
  ut_dev_set_driver(ctx_, "net_ice");
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EIO);
  EXPECT_EQ(TimesyncEnablesBeforePortStart(), 1);
}

TEST_F(MtDevIxgbeTimesyncTest, IxgbePfEnablesTimesyncAfterPortStart) {
  ut_dev_fail_port_start(ctx_, 0);
  ut_dev_set_driver(ctx_, "net_ixgbe");
  ut_dev_set_link(ctx_, true, 0);
  /* net_ixgbe has no rate limit, so RL pacing stops mt_dev_create() at the
   * pacing init, which comes just after the after-start timesync */
  ut_dev_set_tx_pacing(ctx_, ST21_TX_PACING_WAY_RL);
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EINVAL);
  EXPECT_EQ(TimesyncEnablesBeforePortStart(), 0);
  EXPECT_EQ(TimesyncEnablesAfterPortStart(), 1);
  EXPECT_TRUE(ut_dev_timesync_feature(ctx_));
}

/* the timesync does not wait for the link: a port allowed down at the init has its link
 * and PTP later. Here no link read works, so mt_dev_create() stops at the link. */
TEST_F(MtDevIxgbeTimesyncTest, IxgbePfEnablesTimesyncBeforeItsLinkIsUp) {
  ut_dev_fail_port_start(ctx_, 0);
  ut_dev_set_driver(ctx_, "net_ixgbe");
  ut_dev_set_link(ctx_, false, -EIO);
  EXPECT_EQ(ut_dev_create_ports(ctx_), -EIO);
  EXPECT_EQ(TimesyncEnablesBeforePortStart(), 0);
  EXPECT_EQ(TimesyncEnablesAfterPortStart(), 1);
}
