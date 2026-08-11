/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

class PtpUserSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }

  ut_ptp_ctx* ctx_ = nullptr;
};

TEST_F(PtpUserSyncTest, HardwareTimestampHasNoCumulativeSyncBias) {
  constexpr uint64_t kInitialRawNs = 1000000000000ull;
  constexpr int64_t kFirstCorrectionNs = 100;
  constexpr int64_t kSecondCorrectionNs = 167;

  uint64_t user_ns = kInitialRawNs + kFirstCorrectionNs;
  ut_ptp_set_raw_time(kInitialRawNs);
  ut_ptp_set_user_time(ctx_, user_ns);
  ut_ptp_sync_from_user(ctx_);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, user_ns), user_ns);

  user_ns += kSecondCorrectionNs;
  ut_ptp_set_user_time(ctx_, user_ns);
  ut_ptp_sync_from_user(ctx_);
  EXPECT_EQ(ut_ptp_mbuf_time_stamp(ctx_, user_ns), user_ns);
}
