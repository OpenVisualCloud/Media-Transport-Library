/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the cursor-derivation math in `tv_sync_pacing()` (st_tx_video_session.c):
 * normal tsc/ptp cursor derivation, the ST20_TX_FLAG_EXACT_USER_PACING
 * bypass, and the negative time_to_tx_ns clamp.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxSyncPacingTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

class St20TxSyncPacingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, 1000000.0L);
    ut_txv_set_max_onward_epochs(ctx_, 3);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxSyncPacingTest, NormalCursorDerivation) {
  ut_txv_set_cur_epochs(ctx_, 9); /* steady state -> frame_count becomes 10 */
  ut_txv_set_tr_offset(ctx_, 1000.0L);
  ut_txv_set_vrx(ctx_, 2);
  ut_txv_set_trs(ctx_, 100.0L);
  ut_txv_set_mock_ptp_time(ctx_, 10000000);
  ut_txv_set_mock_tsc_time(ctx_, 500000);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  EXPECT_EQ(ut_txv_cur_epochs(ctx_), 10u);
  EXPECT_DOUBLE_EQ((double)ut_txv_ptp_time_cursor(ctx_), 10000800.0);
  EXPECT_DOUBLE_EQ((double)ut_txv_tsc_time_cursor(ctx_), 500800.0);
  EXPECT_EQ(ut_txv_tsc_time_frame_start(ctx_), 500800u);
}

TEST_F(St20TxSyncPacingTest, ExactUserPacingBypassesTransmissionStartTime) {
  ut_txv_set_cur_epochs(ctx_, 9);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, 10000000);
  ut_txv_set_mock_tsc_time(ctx_, 500000);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 10500000), 0);

  EXPECT_DOUBLE_EQ((double)ut_txv_ptp_time_cursor(ctx_),
                   10500000.0); /* verbatim required_tai */
  EXPECT_DOUBLE_EQ((double)ut_txv_tsc_time_cursor(ctx_), 1000000.0);
}

/* tv_pacing_required_tai() is the frame-level-only helper that derives
 * required_tai from app-supplied timestamps; EXACT_USER_PACING+timestamp=0
 * is flagged there, not in tv_sync_pacing() (which is also reached from the
 * RTP-level path with required_tai=0 by design and must never flag). */
TEST_F(St20TxSyncPacingTest, ExactUserPacingWithZeroRequiredTaiShouldBeFlagged) {
  ut_txv_set_user_pacing(ctx_, true);
  ut_txv_set_exact_user_pacing(ctx_, true);

  EXPECT_EQ(ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, /*timestamp=*/0),
            0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u)
      << "EXACT_USER_PACING with timestamp=0 should be flagged as an invalid "
         "request";
}

/* RTP-level callers invoke tv_sync_pacing() directly with required_tai=0,
 * bypassing tv_pacing_required_tai() entirely, even when EXACT_USER_PACING
 * is set (a session with type=RTP_LEVEL + USER_PACING|EXACT_USER_PACING
 * passes tv_ops_check()). This must never flag on every frame. */
TEST_F(St20TxSyncPacingTest, RtpLevelExactUserPacingWithZeroRequiredTaiNeverFlags) {
  ut_txv_set_cur_epochs(ctx_, 9);
  ut_txv_set_exact_user_pacing(ctx_, true);
  ut_txv_set_mock_ptp_time(ctx_, 10000000);
  ut_txv_set_mock_tsc_time(ctx_, 500000);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, /*required_tai=*/0), 0);

  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u)
      << "tv_sync_pacing() alone (the RTP-level call path) must never flag "
         "stat_error_user_timestamp regardless of EXACT_USER_PACING";
}

TEST_F(St20TxSyncPacingTest, NegativeTimeToTxClampsToZero) {
  ut_txv_set_cur_epochs(ctx_, 9); /* steady state -> frame_count becomes 10 */
  ut_txv_set_tr_offset(ctx_, 0.0L);
  ut_txv_set_vrx(ctx_, 2000);
  ut_txv_set_trs(ctx_, 100.0L); /* vrx*trs=200000, pushes start time before cur_tai */
  ut_txv_set_mock_ptp_time(ctx_, 10000000);
  ut_txv_set_mock_tsc_time(ctx_, 500000);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  EXPECT_DOUBLE_EQ((double)ut_txv_tsc_time_cursor(ctx_),
                   500000.0); /* clamped: cur_tsc + 0 */
  EXPECT_EQ(ut_txv_tsc_time_frame_start(ctx_), 500000u);
}
