/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * MTL_PORT_FLAG_MAC_LOOPBACK: an igc port is configured with rte_eth_conf.lpbk_mode 1 and
 * its link is taken as up without a partner; any other port refuses the flag before
 * rte_eth_dev_configure().
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevMacLoopbackTest*'
 */

#include <gtest/gtest.h>

#include "dev/mt_dev_harness.h"

class MtDevMacLoopbackTest : public testing::Test {
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

TEST_F(MtDevMacLoopbackTest, IgcConfiguresLpbkMode) {
  ut_dev_set_driver(ctx_, "net_igc");
  ut_dev_set_mac_loopback(ctx_, true);
  ASSERT_EQ(ut_dev_config_port(ctx_), 0);
  EXPECT_EQ(ut_dev_configure_calls(ctx_), 1);
  EXPECT_EQ(ut_dev_configured_lpbk_mode(ctx_), 1u);
}

TEST_F(MtDevMacLoopbackTest, IgcWithoutFlagKeepsLpbkModeOff) {
  ut_dev_set_driver(ctx_, "net_igc");
  ASSERT_EQ(ut_dev_config_port(ctx_), 0);
  EXPECT_EQ(ut_dev_configure_calls(ctx_), 1);
  EXPECT_EQ(ut_dev_configured_lpbk_mode(ctx_), 0u);
}

TEST_F(MtDevMacLoopbackTest, NonIgcPmdRejectsFlagBeforeConfigure) {
  ut_dev_set_driver(ctx_, "net_ice");
  ut_dev_set_mac_loopback(ctx_, true);
  EXPECT_EQ(ut_dev_config_port(ctx_), -ENOTSUP);
  EXPECT_EQ(ut_dev_configure_calls(ctx_), 0);
}

TEST_F(MtDevMacLoopbackTest, NonDpdkBackendRejectsFlag) {
  ut_dev_set_driver(ctx_, "kernel_socket");
  ut_dev_set_mac_loopback(ctx_, true);
  EXPECT_EQ(ut_dev_config_port(ctx_), -ENOTSUP);
  EXPECT_EQ(ut_dev_configure_calls(ctx_), 0);
}

TEST_F(MtDevMacLoopbackTest, LinkIsTakenAsUpAt2500) {
  ut_dev_set_driver(ctx_, "net_igc");
  ut_dev_set_mac_loopback(ctx_, true);
  ASSERT_EQ(ut_dev_detect_link(ctx_), 0);
  EXPECT_EQ(ut_dev_link_speed(ctx_), 2500u);
}
