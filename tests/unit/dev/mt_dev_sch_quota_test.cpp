/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The data quota of each scheduler when the app sets none, which the session quotas
 * are scaled against.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtDevSchQuotaTest*'
 */

#include <gtest/gtest.h>
#include <st20_api.h>

#include "dev/mt_dev_harness.h"

namespace {

class MtDevSchQuotaTest : public testing::Test {
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

TEST_F(MtDevSchQuotaTest, TheDefaultQuotaIs24TxSessionsOf1080p59) {
  EXPECT_EQ(ut_dev_sch_init_quota(ctx_),
            24 * static_cast<int>(st20_1080p59_yuv422_10bit_bandwidth_mps()));
}

}  // namespace
