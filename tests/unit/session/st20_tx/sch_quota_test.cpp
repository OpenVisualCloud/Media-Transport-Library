/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The scheduler quota a TX video session asks for. Under a user quota it is its
 * bandwidth. Without one a frame-level ST20 session keeps it, so a scheduler holds
 * ST_QUOTA_TX1080P_PER_SCH (24) of them at 1080p59, slice-level ST20 and frame-level
 * ST22 sessions are scaled by 24 / 12 to hold 12, and RTP sessions by 24 / 8 to hold 8.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='TxVideoSchQuotaTest*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {

constexpr int kSliceScale =
    2; /* 24 / ST_QUOTA_TX1080P_SLICE_PER_SCH, _ST22_FRAME_PER_SCH 12 */
constexpr int kRtpScale = 3; /* 24 / ST_QUOTA_TX1080P_RTP_PER_SCH 8 */

class TxVideoSchQuotaTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
  }
};

TEST_F(TxVideoSchQuotaTest, AnSt20FrameSessionChargesItsBandwidth) {
  int bandwidth = ut_txv_st20_tx_quota_mbs(ST20_TYPE_FRAME_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut_txv_st20_tx_quota_mbs(ST20_TYPE_FRAME_LEVEL, false), bandwidth);
}

TEST_F(TxVideoSchQuotaTest, AnSt20SliceSessionChargesTwiceItsBandwidth) {
  int bandwidth = ut_txv_st20_tx_quota_mbs(ST20_TYPE_SLICE_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut_txv_st20_tx_quota_mbs(ST20_TYPE_SLICE_LEVEL, false),
            kSliceScale * bandwidth);
}

TEST_F(TxVideoSchQuotaTest, AnSt20RtpSessionChargesThriceItsBandwidth) {
  int bandwidth = ut_txv_st20_tx_quota_mbs(ST20_TYPE_RTP_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut_txv_st20_tx_quota_mbs(ST20_TYPE_RTP_LEVEL, false), kRtpScale * bandwidth);
}

TEST_F(TxVideoSchQuotaTest, AnSt22FrameSessionChargesTwiceItsBandwidth) {
  int bandwidth = ut_txv_st22_tx_quota_mbs(ST22_TYPE_FRAME_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut_txv_st22_tx_quota_mbs(ST22_TYPE_FRAME_LEVEL, false),
            kSliceScale * bandwidth);
}

TEST_F(TxVideoSchQuotaTest, AnSt22RtpSessionChargesThriceItsBandwidth) {
  int bandwidth = ut_txv_st22_tx_quota_mbs(ST22_TYPE_RTP_LEVEL, true);
  ASSERT_GT(bandwidth, 0);
  EXPECT_EQ(ut_txv_st22_tx_quota_mbs(ST22_TYPE_RTP_LEVEL, false), kRtpScale * bandwidth);
}

}  // namespace
