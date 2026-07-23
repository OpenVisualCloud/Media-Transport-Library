/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the tv_sync_pacing() fix that snaps the real TX-gate time
 * (ptp_time_cursor/tsc_time_cursor) to the exact media-clock tick the RTP
 * timestamp will round to, so the declared RTP timestamp and the actual
 * departure gate always derive from one identical instant and the packet
 * can never appear to depart before its own RTP timestamp. Also pins the
 * EXACT_USER_PACING fail-soft lead-time check in tv_pacing_required_tai().
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRtpGateSnapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint32_t kVideoClockHz = ST10_VIDEO_SAMPLING_RATE_90K;
}  // namespace

class St20TxRtpGateSnapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txv_set_frame_time(ctx_, kFramePeriodNs);
    ut_txv_set_max_onward_epochs(ctx_, 3);
    ut_txv_set_user_pacing(ctx_, true);
    ut_txv_set_exact_user_pacing(ctx_, true);
    ut_txv_set_mock_ptp_time(ctx_, kCurrentTai);
    ut_txv_set_mock_tsc_time(ctx_, kCurrentTsc);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(St20TxRtpGateSnapTest, PtpTimeCursorRoundTripsLosslessAcrossTickSweep) {
  /* 90kHz tick period is ~11111.11ns; sweep an odd step across more than two
   * full ticks so both "rounds down" and "rounds up" sub-tick positions are
   * exercised. */
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kCurrentTai + offset;

    ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;

    uint64_t cursor = (uint64_t)ut_txv_ptp_time_cursor(ctx_);
    uint32_t media_ts = st10_tai_to_media_clk(cursor, kVideoClockHz);
    uint64_t implied_ns = st10_media_clk_to_ns(media_ts, kVideoClockHz);
    EXPECT_EQ(implied_ns, cursor)
        << "offset=" << offset << " ptp_time_cursor=" << cursor
        << " does not round-trip losslessly through the media clock, so its "
           "RTP timestamp would imply a different real time than the gate "
           "it was derived from";

    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    uint32_t rtp_ts = ut_txv_rtp_time_stamp(ctx_);
    EXPECT_EQ(rtp_ts, media_ts) << "offset=" << offset;
    EXPECT_LE(st10_media_clk_to_ns(rtp_ts, kVideoClockHz), cursor)
        << "offset=" << offset
        << ": RTP timestamp implies a later real time than the actual TX "
           "gate -- negative latency";
  }
}

TEST_F(St20TxRtpGateSnapTest, ExactUserPacingRejectsInsufficientWarmUpLeadTime) {
  ut_txv_set_trs(ctx_, 1000.0L);
  ut_txv_set_warm_pkts(ctx_, 100); /* needs 100 * 1000ns = 100000ns lead time */

  uint64_t required_tai =
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai + 50000);

  EXPECT_EQ(required_tai, 0u)
      << "required_tai only 50000ns ahead, less than the 100000ns the RL "
         "warm-up sequence needs to gate the first packet, must fall back "
         "to default pacing";
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St20TxRtpGateSnapTest, ExactUserPacingAcceptsSufficientWarmUpLeadTime) {
  ut_txv_set_trs(ctx_, 1000.0L);
  ut_txv_set_warm_pkts(ctx_, 100); /* needs 100 * 1000ns = 100000ns lead time */

  uint64_t required_tai =
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai + 200000);

  EXPECT_EQ(required_tai, kCurrentTai + 200000u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxRtpGateSnapTest, DefaultPacingWithZeroWarmPktsHasNoLeadTimeFloor) {
  /* warm_pkts defaults to 0 (TSC pacing, no RL warm-up) -- any non-negative
   * lead time must be accepted, matching pre-fix behavior for this case. */
  ut_txv_set_trs(ctx_, 1000.0L);

  uint64_t required_tai =
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai + 1);

  EXPECT_EQ(required_tai, kCurrentTai + 1u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}
