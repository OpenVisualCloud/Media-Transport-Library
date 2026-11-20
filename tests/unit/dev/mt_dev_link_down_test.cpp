/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_dev_link_down(), the link read of the TX hang detectors: true only when the link
 * of a DPDK user PMD port reads down. A port of another PMD has no ethdev link to read.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevLinkDownTest.*'
 */

#include <gtest/gtest.h>

#include <cerrno>

#include "dev/mt_dev_harness.h"

namespace {

class MtDevLinkDownTest : public ::testing::Test {
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

TEST_F(MtDevLinkDownTest, DpdkPortWithTheLinkDownReadsDown) {
  ut_dev_set_link(ctx_, false, 0);

  EXPECT_TRUE(ut_dev_link_down(ctx_));
  EXPECT_EQ(ut_dev_link_reads(ctx_), 1);
}

TEST_F(MtDevLinkDownTest, DpdkPortWithTheLinkUpReadsUp) {
  ut_dev_set_link(ctx_, true, 0);

  EXPECT_FALSE(ut_dev_link_down(ctx_));
}

TEST_F(MtDevLinkDownTest, ReadErrorCountsAsUp) {
  ut_dev_set_link(ctx_, false, -ENOTSUP);

  EXPECT_FALSE(ut_dev_link_down(ctx_));
}

TEST_F(MtDevLinkDownTest, PortOfAnotherPmdReadsNoLink) {
  ut_dev_set_link(ctx_, false, 0);
  ut_dev_set_pmd(ctx_, MTL_PMD_KERNEL_SOCKET);

  EXPECT_FALSE(ut_dev_link_down(ctx_));
  EXPECT_EQ(ut_dev_link_reads(ctx_), 0);
}

}  // namespace
