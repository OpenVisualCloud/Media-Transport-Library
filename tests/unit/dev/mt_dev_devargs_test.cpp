/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>
#include <mtl_api.h>

#include <cstdint>
#include <string>
#include <vector>

#include "dev/mt_dev_harness.h"

/*
 * Covers the PCI devarg builder only; dev_eal_init()'s argv assembly needs a real EAL.
 * The buffer's widest writers are the net_af_xdp and eth_af_packet vdev branches, which
 * MT_EAL_PORT_ARG_MAX_LEN is dimensioned for and no tier covers. No tier proves
 * rl_burst_size reaches the ice driver either: CI passes auto and tsc pacing only.
 */
class MtDevDevargsTest : public testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_dev_create_ctx();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_dev_destroy_ctx(ctx_);
  }

  std::string BuildDevarg(enum mtl_port port) {
    std::vector<char> devarg(ut_dev_pci_devarg_size(), '\0');
    ut_dev_build_pci_devarg(ctx_, port, devarg.data(), devarg.size());
    return std::string(devarg.data());
  }

  ut_dev_ctx* ctx_ = nullptr;
};

TEST_F(MtDevDevargsTest, UnsetBurstSizeBuildsBareBdf) {
  ut_dev_set_port(ctx_, MTL_PORT_P, "0000:c9:01.0", 0);
  std::string devarg = BuildDevarg(MTL_PORT_P);
  EXPECT_EQ(devarg, "0000:c9:01.0");
  EXPECT_EQ(devarg.find("rl_burst_size"), std::string::npos);
}

TEST_F(MtDevDevargsTest, SetBurstSizeAppendsDevarg) {
  ut_dev_set_port(ctx_, MTL_PORT_P, "0000:c9:01.0", 2048);
  EXPECT_EQ(BuildDevarg(MTL_PORT_P), "0000:c9:01.0,rl_burst_size=2048");
}

TEST_F(MtDevDevargsTest, BurstSizeAppliesOnlyToThePortThatSetIt) {
  ut_dev_set_port(ctx_, MTL_PORT_P, "0000:c9:01.0", 2048);
  ut_dev_set_port(ctx_, MTL_PORT_R, "0000:c9:01.1", 0);
  EXPECT_EQ(BuildDevarg(MTL_PORT_P), "0000:c9:01.0,rl_burst_size=2048");
  EXPECT_EQ(BuildDevarg(MTL_PORT_R), "0000:c9:01.1");
}

/* The ice driver owns the valid range, so MTL passes any non-zero value through. */
TEST_F(MtDevDevargsTest, OutOfRangeBurstSizeIsPassedThroughUnvalidated) {
  ut_dev_set_port(ctx_, MTL_PORT_P, "0000:c9:01.0", 1);
  EXPECT_EQ(BuildDevarg(MTL_PORT_P), "0000:c9:01.0,rl_burst_size=1");
}

TEST_F(MtDevDevargsTest, LongestBdfWithLargestBurstSizeIsNotTruncated) {
  std::string bdf(MTL_PORT_MAX_LEN - 1, 'a');
  ut_dev_set_port(ctx_, MTL_PORT_P, bdf.c_str(), UINT32_MAX);
  EXPECT_EQ(BuildDevarg(MTL_PORT_P), bdf + ",rl_burst_size=4294967295");
}
