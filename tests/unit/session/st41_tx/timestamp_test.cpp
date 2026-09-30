/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST41 TX timestamp flags, driven through the real frame-level tasklet:
 *   ST41_TX_FLAG_USER_TIMESTAMP puts an app ST10_TIMESTAMP_FMT_MEDIA_CLK
 *   value on the wire as the RTP timestamp.
 *   ST41_TX_FLAG_USER_PACING rejects ST10_TIMESTAMP_FMT_MEDIA_CLK and keeps
 *   the default epoch pacing, as ST30 and ST40 do.
 *
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St41TxTimestamp*'
 */

#include <gtest/gtest.h>

#include "session/st41_tx_harness.h"

namespace {
constexpr uint64_t kNsPerMs = 1000 * 1000;
/* a 2023 TAI instant on a 1 ms epoch boundary, and PTP half an epoch past it */
constexpr uint64_t kEpochStartNs = 1700000000000ULL * kNsPerMs;
constexpr uint64_t kPtpNs = kEpochStartNs + kNsPerMs / 2;
constexpr uint32_t kUserMediaClk = 0x12345678;
}  // namespace

class St41TxTimestampTest : public ::testing::Test {
 protected:
  ut41tx_ctx* ctx_ = nullptr;
  uint8_t payload_[4] = {1, 2, 3, 4};

  void SetUp() override {
    ASSERT_EQ(ut41tx_init(), 0) << "EAL init failed";
    ctx_ = ut41tx_ctx_create();
    ASSERT_NE(ctx_, nullptr);
    ut41tx_ctx_set_payload(ctx_, payload_, sizeof(payload_));
    ut41tx_set_mock_ptp_time(ctx_, kPtpNs);
  }

  void TearDown() override {
    ut41tx_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }
};

TEST_F(St41TxTimestampTest, UserTimestampMediaClkIsTheWireRtpTimestamp) {
  ut41tx_set_ops_flags(ctx_, ST41_TX_FLAG_USER_TIMESTAMP);

  ASSERT_EQ(ut41tx_run_frame(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, kUserMediaClk), 0);

  EXPECT_EQ(ut41tx_wire_rtp_timestamp(ctx_), kUserMediaClk);
  EXPECT_EQ(ut41tx_done_meta(ctx_)->rtp_timestamp, kUserMediaClk);
}

TEST_F(St41TxTimestampTest, UserPacingMediaClkFallsBackToDefaultPacing) {
  ut41tx_set_ops_flags(ctx_, ST41_TX_FLAG_USER_PACING);

  ASSERT_EQ(ut41tx_run_frame(ctx_, ST10_TIMESTAMP_FMT_MEDIA_CLK, kUserMediaClk), 0);

  EXPECT_EQ(ut41tx_done_meta(ctx_)->timestamp, kEpochStartNs + kNsPerMs);
  EXPECT_EQ(ut41tx_stat_epoch_mismatch(ctx_), 0u);
  EXPECT_EQ(ut41tx_stat_error_user_timestamp(ctx_), 1u);
}
