/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the tx_ancillary_session_sync_pacing() fix that snaps the real
 * TX-gate time (ptp_time_cursor/tsc_time_cursor) to the exact media-clock
 * tick the RTP timestamp will round to. Mirrors
 * session/st20_tx/rtp_gate_snap_test.cpp for the ST40 (ancillary) session.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40TxRtpGateSnapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40_tx_harness.h"

namespace {
constexpr uint64_t kNanosecondsPerMillisecond = 1000 * 1000;
constexpr uint64_t kFramePeriodNs = kNanosecondsPerMillisecond;
constexpr uint64_t kCurrentTai = 10 * kFramePeriodNs;
constexpr uint64_t kCurrentTsc = kFramePeriodNs / 2;
constexpr uint32_t kMediaClockRate = ST10_VIDEO_SAMPLING_RATE_90K;
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

TEST_F(St40TxRtpGateSnapTest, PtpTimeCursorRoundTripsLosslessAcrossTickSweep) {
  for (uint64_t offset = 0; offset < 25000; offset += 137) {
    uint64_t required_tai = kCurrentTai + offset;

    ASSERT_EQ(ut_txa_sync_pacing(ctx_, required_tai), 0) << "offset=" << offset;

    uint64_t cursor = ut_txa_ptp_time_cursor(ctx_);
    uint32_t media_ts = st10_tai_to_media_clk(cursor, kMediaClockRate);
    uint64_t implied_ns = st10_media_clk_to_ns(media_ts, kMediaClockRate);
    EXPECT_EQ(implied_ns, cursor)
        << "offset=" << offset << " ptp_time_cursor=" << cursor
        << " does not round-trip losslessly through the media clock";

    ut_txa_update_rtp_time_stamp(ctx_, ST10_TIMESTAMP_FMT_TAI, 0);
    uint32_t rtp_ts = ut_txa_rtp_time_stamp(ctx_);
    EXPECT_EQ(rtp_ts, media_ts) << "offset=" << offset;
    EXPECT_LE(st10_media_clk_to_ns(rtp_ts, kMediaClockRate), cursor)
        << "offset=" << offset
        << ": RTP timestamp implies a later real time than the actual TX "
           "gate -- negative latency";
  }
}
