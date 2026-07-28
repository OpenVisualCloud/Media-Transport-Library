/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pure-function pins: `ptp_correct_ts` (coefficient scaling about the last
 * sync point) and `ptp_net_tmstamp_to_ns` (48-bit network-order seconds +
 * nanoseconds).
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='PtpDeltaMath*'
 */

#include <gtest/gtest.h>

#include "ptp/ptp_harness.h"

static constexpr uint64_t kNsPerS = 1000000000ULL;

class PtpDeltaMathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }
  ut_ptp_ctx* ctx_ = nullptr;
};

/* coefficient 1.0 is the identity transform regardless of last_sync_ts. */
TEST_F(PtpDeltaMathTest, CorrectTsIdentityAtUnitCoefficient) {
  ut_ptp_set_coefficient(ctx_, 1.0);
  ut_ptp_set_last_sync_ts(ctx_, 1000);
  EXPECT_EQ(ut_ptp_correct_ts(ctx_, 2000), 2000u);
  /* ts before last_sync_ts -> negative advance, still identity */
  EXPECT_EQ(ut_ptp_correct_ts(ctx_, 500), 500u);
}

/* coefficient != 1.0 scales the advance from last_sync_ts. */
TEST_F(PtpDeltaMathTest, CorrectTsScalesAdvance) {
  ut_ptp_set_coefficient(ctx_, 1.5);
  ut_ptp_set_last_sync_ts(ctx_, 1000);
  /* advance = 1000, scaled = 1500 -> 2500 */
  EXPECT_EQ(ut_ptp_correct_ts(ctx_, 2000), 2500u);
}

/* Negative advance with a coefficient applied. */
TEST_F(PtpDeltaMathTest, CorrectTsScalesNegativeAdvance) {
  ut_ptp_set_coefficient(ctx_, 2.0);
  ut_ptp_set_last_sync_ts(ctx_, 2000);
  /* advance = -1000, scaled = -2000 -> 0 */
  EXPECT_EQ(ut_ptp_correct_ts(ctx_, 1000), 0u);
}

/* network-order seconds (msb<<32 | lsb) * 1e9 + ns. */
TEST_F(PtpDeltaMathTest, NetTmstampBasic) {
  EXPECT_EQ(ut_ptp_net_tmstamp_to_ns(0, 5, 123), 5ULL * kNsPerS + 123);
}

/* 48-bit seconds boundary uses both the msb and lsb words. */
TEST_F(PtpDeltaMathTest, NetTmstamp48BitBoundary) {
  const uint64_t sec = (uint64_t)0xFFFFULL << 32 | 0xFFFFFFFFULL; /* 2^48 - 1 */
  EXPECT_EQ(ut_ptp_net_tmstamp_to_ns(0xFFFF, 0xFFFFFFFF, 0), sec * kNsPerS);
}

/* msb contributes the high 16 bits of the 48-bit seconds field. */
TEST_F(PtpDeltaMathTest, NetTmstampMsbContribution) {
  const uint64_t sec = (uint64_t)0x0001ULL << 32 | 0x00000002ULL;
  EXPECT_EQ(ut_ptp_net_tmstamp_to_ns(0x0001, 0x00000002, 7), sec * kNsPerS + 7);
}
