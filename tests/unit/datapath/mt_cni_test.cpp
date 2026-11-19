/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "datapath/mt_cni_harness.h"

class CniRxL2PtpTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_cni_eal_init(), 0);
  }
};

/* Only MTL_PORT_P and a port with RX timestamp offload get a PTP instance; an L2
 * PTP pkt on any other port, e.g. a promiscuous VF, must not reach mt_ptp_parse(). */
TEST_P(CniRxL2PtpTest, PortWithoutPtpInstanceSkipsParse) {
  ut_cni_ctx* ctx = ut_cni_create(false);
  ASSERT_NE(ctx, nullptr);

  EXPECT_EQ(ut_cni_rx_l2_ptp(ctx, GetParam()), 0);
  EXPECT_EQ(ut_cni_ptp_parse_calls(), 0);
  EXPECT_GT(ut_cni_eth_rx_bytes(ctx), 0u);

  ut_cni_destroy(ctx);
}

TEST_P(CniRxL2PtpTest, PortWithPtpInstanceParses) {
  ut_cni_ctx* ctx = ut_cni_create(true);
  ASSERT_NE(ctx, nullptr);

  EXPECT_EQ(ut_cni_rx_l2_ptp(ctx, GetParam()), 0);
  EXPECT_EQ(ut_cni_ptp_parse_calls(), 1);

  ut_cni_destroy(ctx);
}

INSTANTIATE_TEST_SUITE_P(Vlan, CniRxL2PtpTest, ::testing::Bool());
