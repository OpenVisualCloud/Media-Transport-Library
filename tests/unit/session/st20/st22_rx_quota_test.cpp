/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The scheduler quota an ST22 RX session asks for. Under a user quota it is its
 * bandwidth (doubled on the RTP path); without one it is scaled by
 * ST_QUOTA_TX1080P_PER_SCH / ST_QUOTA_RX1080P_ST22_PER_SCH, 24 / 12, for both paths.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='St22RxQuotaTest*'
 */

#include <gtest/gtest.h>

#include "session/st20_harness.h"

namespace {

constexpr int kScale =
    2; /* ST_QUOTA_TX1080P_PER_SCH 24 / ST_QUOTA_RX1080P_ST22_PER_SCH 12 */

class St22RxQuotaTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
  }
};

TEST_F(St22RxQuotaTest, AFrameSessionChargesTwiceItsBandwidth) {
  int bandwidth = ut20_st22_rx_quota_mbs(ST22_TYPE_FRAME_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut20_st22_rx_quota_mbs(ST22_TYPE_FRAME_LEVEL, false), kScale * bandwidth);
}

TEST_F(St22RxQuotaTest, AnRtpSessionChargesTwiceItsBandwidth) {
  int bandwidth = ut20_st22_rx_quota_mbs(ST22_TYPE_RTP_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut20_st22_rx_quota_mbs(ST22_TYPE_RTP_LEVEL, false), kScale * bandwidth);
}

}  // namespace
