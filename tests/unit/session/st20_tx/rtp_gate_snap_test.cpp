/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins RTP timestamp quantization, exact-user gate timing, and the
 * EXACT_USER_PACING fail-soft lead-time check in tv_pacing_required_tai().
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20TxRtpGateSnapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint32_t kVideoClockHz = ST10_VIDEO_SAMPLING_RATE_90K;
constexpr uint64_t kEpochTai = 1775000000000000000ULL;

uint64_t roundClosest(uint64_t value, uint64_t multiplier, uint64_t divisor) {
  __uint128_t product = static_cast<__uint128_t>(value) * multiplier;
  __uint128_t quotient = product / divisor;
  __uint128_t remainder = product % divisor;
  if (remainder > divisor / 2) quotient++;
  return static_cast<uint64_t>(quotient);
}

uint64_t taiForMediaTick(uint64_t tick) {
  return roundClosest(tick, kNanosecondsPerSecond, kVideoClockHz);
}
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

TEST_F(St20TxRtpGateSnapTest, ExactGateRemainsVerbatimAcrossTickSweep) {
  /* 90kHz tick period is ~11111.11ns; sweep an odd step across more than two
   * full ticks so both "rounds down" and "rounds up" sub-tick positions are
   * exercised. */
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kCurrentTai + offset;

    ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;

    uint64_t cursor = (uint64_t)ut_txv_ptp_time_cursor(ctx_);
    EXPECT_EQ(cursor, required_tai) << "offset=" << offset;
    uint32_t media_ts = st10_tai_to_media_clk(cursor, kVideoClockHz);

    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    uint32_t rtp_ts = ut_txv_rtp_time_stamp(ctx_);
    EXPECT_EQ(rtp_ts, media_ts) << "offset=" << offset;
  }
}

TEST_F(St20TxRtpGateSnapTest, EpochTaiPreservesFullWidthGateAndWrappedRtpTimestamp) {
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kEpochTai + offset;
    uint64_t expected_tick =
        roundClosest(required_tai, kVideoClockHz, kNanosecondsPerSecond);

    ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;
    EXPECT_EQ(static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_)), required_tai)
        << "offset=" << offset;

    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    EXPECT_EQ(ut_txv_rtp_time_stamp(ctx_), static_cast<uint32_t>(expected_tick))
        << "offset=" << offset;
  }
}

TEST_F(St20TxRtpGateSnapTest, GateRemainsContinuousAcrossRtpTimestampWrap) {
  uint64_t epoch_tick = roundClosest(kEpochTai, kVideoClockHz, kNanosecondsPerSecond);
  uint64_t wrap_tick = ((epoch_tick >> 32) + 1) << 32;
  uint64_t wrap_tai = taiForMediaTick(wrap_tick);
  const int64_t offsets[] = {-25000, -11112, -1, 0, 1, 11112, 25000};

  uint64_t previous_cursor = 0;
  for (int64_t offset : offsets) {
    uint64_t required_tai =
        static_cast<uint64_t>(static_cast<int64_t>(wrap_tai) + offset);
    uint64_t expected_tick =
        roundClosest(required_tai, kVideoClockHz, kNanosecondsPerSecond);

    ASSERT_EQ(ut_txv_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;
    uint64_t cursor = static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_));
    EXPECT_EQ(cursor, required_tai) << "offset=" << offset;
    if (previous_cursor) {
      EXPECT_GE(cursor, previous_cursor);
    }

    ut_txv_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    EXPECT_EQ(ut_txv_rtp_time_stamp(ctx_), static_cast<uint32_t>(expected_tick))
        << "offset=" << offset;
    previous_cursor = cursor;
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
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai + 100000);

  EXPECT_EQ(required_tai, kCurrentTai + 100000u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 0u);
}

TEST_F(St20TxRtpGateSnapTest, ExactUserPacingRejectsOneNanosecondShortLeadTime) {
  ut_txv_set_trs(ctx_, 1000.0L);
  ut_txv_set_warm_pkts(ctx_, 100);

  uint64_t required_tai =
      ut_txv_pacing_required_tai(ctx_, ST10_TIMESTAMP_FMT_TAI, kCurrentTai + 99999);

  EXPECT_EQ(required_tai, 0u);
  EXPECT_EQ(ut_txv_stat_error_user_timestamp(ctx_), 1u);
}

TEST_F(St20TxRtpGateSnapTest, EpochDerivedStartTimeSnapsEvenWithExactUserPacingEnabled) {
  /* EXACT_USER_PACING is enabled but the app supplied no timestamp, so
   * tv_sync_pacing() falls back to the epoch-derived start time -- the same
   * source as the no-flag path, and equally in need of the media-clock snap.
   * Gating the snap on the flag alone instead of "flag AND a timestamp was
   * actually supplied" would skip it here and leave up to half a tick between
   * the real departure instant and the RTP timestamp derived from it. */
  ut_txv_set_tr_offset(ctx_, 4567); /* pushes the start time off a 90kHz tick */

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  uint64_t cursor = static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_));
  ASSERT_NE(cursor, 0u);
  EXPECT_EQ(cursor,
            taiForMediaTick(roundClosest(cursor, kVideoClockHz, kNanosecondsPerSecond)))
      << "start_time_tai " << cursor << " is not on a 90kHz tick boundary";
}

TEST_F(St20TxRtpGateSnapTest, ZeroSamplingClockRateLeavesStartTimeUnchanged) {
  /* A zero sampling clock rate is a config error, but the snap must degrade to
   * a no-op. Returning 0 instead would make start_time_tai unconditionally
   * "in the past", zero time_to_tx_ns and dump the whole frame at line rate. */
  ut_txv_set_sampling_clock_rate(ctx_, 0);
  ut_txv_set_tr_offset(ctx_, 4567);

  ASSERT_EQ(ut_txv_sync_pacing(ctx_, 0), 0);

  uint64_t expected = ut_txv_cur_epochs(ctx_) * kFramePeriodNs + 4567;
  EXPECT_EQ(static_cast<uint64_t>(ut_txv_ptp_time_cursor(ctx_)), expected);
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
