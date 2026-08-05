/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins RTP timestamp quantization and exact-user gate timing. Mirrors
 * session/st20_tx/rtp_gate_snap_test.cpp for the ST40 (ancillary) session.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxRtpGateSnapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kNanosecondsPerSecond = 1000 * kNanosecondsPerMillisecond;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint32_t kMediaClockRate = ST10_VIDEO_SAMPLING_RATE_90K;
constexpr uint64_t kEpochTai = 1775000000000000000ULL;

uint64_t roundClosest(uint64_t value, uint64_t multiplier, uint64_t divisor) {
  __uint128_t product = static_cast<__uint128_t>(value) * multiplier;
  __uint128_t quotient = product / divisor;
  __uint128_t remainder = product % divisor;
  if (remainder > divisor / 2) quotient++;
  return static_cast<uint64_t>(quotient);
}

uint64_t taiForMediaTick(uint64_t tick) {
  return roundClosest(tick, kNanosecondsPerSecond, kMediaClockRate);
}
}  // namespace

class St40TxRtpGateSnapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txa_init(), 0);
    ctx_ = ut_txa_create();
    ASSERT_NE(ctx_, nullptr);
    ut_txa_set_user_pacing(ctx_, true);
    ut_txa_set_exact_user_pacing(ctx_, true);
    ut_txa_set_mock_ptp_time(ctx_, kCurrentTai);
    ut_txa_set_mock_tsc_time(ctx_, kCurrentTsc);
  }
  void TearDown() override {
    ut_txa_destroy(ctx_);
  }
  ut_txa_ctx* ctx_ = nullptr;
};

TEST_F(St40TxRtpGateSnapTest, ExactGateRemainsVerbatimAcrossTickSweep) {
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kCurrentTai + offset;

    ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;

    uint64_t cursor = ut_txa_ptp_time_cursor(ctx_);
    EXPECT_EQ(cursor, required_tai) << "offset=" << offset;
    uint32_t media_ts = st10_tai_to_media_clk(cursor, kMediaClockRate);

    ut_txa_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    uint32_t rtp_ts = ut_txa_rtp_time_stamp(ctx_);
    EXPECT_EQ(rtp_ts, media_ts) << "offset=" << offset;
  }
}

TEST_F(St40TxRtpGateSnapTest, EpochTaiPreservesFullWidthGateAndWrappedRtpTimestamp) {
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kEpochTai + offset;
    uint64_t expected_tick =
        roundClosest(required_tai, kMediaClockRate, kNanosecondsPerSecond);

    ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;
    EXPECT_EQ(ut_txa_ptp_time_cursor(ctx_), required_tai) << "offset=" << offset;

    ut_txa_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    EXPECT_EQ(ut_txa_rtp_time_stamp(ctx_), static_cast<uint32_t>(expected_tick))
        << "offset=" << offset;
  }
}

TEST_F(St40TxRtpGateSnapTest, GateRemainsContinuousAcrossRtpTimestampWrap) {
  uint64_t epoch_tick = roundClosest(kEpochTai, kMediaClockRate, kNanosecondsPerSecond);
  uint64_t wrap_tick = ((epoch_tick >> 32) + 1) << 32;
  uint64_t wrap_tai = taiForMediaTick(wrap_tick);
  const int64_t offsets[] = {-25000, -11112, -1, 0, 1, 11112, 25000};

  uint64_t previous_cursor = 0;
  for (int64_t offset : offsets) {
    uint64_t required_tai =
        static_cast<uint64_t>(static_cast<int64_t>(wrap_tai) + offset);
    uint64_t expected_tick =
        roundClosest(required_tai, kMediaClockRate, kNanosecondsPerSecond);

    ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;
    uint64_t cursor = ut_txa_ptp_time_cursor(ctx_);
    EXPECT_EQ(cursor, required_tai) << "offset=" << offset;
    if (previous_cursor) {
      EXPECT_GE(cursor, previous_cursor);
    }

    ut_txa_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    EXPECT_EQ(ut_txa_rtp_time_stamp(ctx_), static_cast<uint32_t>(expected_tick))
        << "offset=" << offset;
    previous_cursor = cursor;
  }
}
