/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the ST 2110-20 RX timing parser compliance ladder and its interval
 * statistics: which frames may be charged an rtp_ts_delta violation, and how
 * per-frame results fold into the rv_tp_stat report.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTimingParserTest.*'
 */

#include <gtest/gtest.h>

#include <regex>
#include <string>

#include "session/rx_timing_parser_harness.h"
#include "session/stderr_capture.h"

namespace {
/* Any epoch far enough from 0 that epoch * 1500 is a non-zero RTP timestamp. */
constexpr uint64_t kFirstEpoch = 1000;
/* fpt slack yielding a small vrx (2 at this geometry). */
constexpr int32_t kSmallVrxSlackNs = 5000;
/* fpt slack yielding a larger vrx (5 at this geometry). */
constexpr int32_t kLargeVrxSlackNs = 16000;
/* Packet spacing tighter than trs, so the frame's ipt max is smaller. */
constexpr uint32_t kTightSpacingNs = 3000;

float parseDeltaAvg(const std::string& log) {
  std::smatch match;
  std::regex re(R"(RTP TS DELTA AVG (-?[0-9]+\.[0-9]+))");
  if (!std::regex_search(log, match, re)) return -1.0f;
  return std::stof(match[1]);
}
}  // namespace

class St20RxTimingParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_rvtp_init(), 0);
    ctx_ = ut_rvtp_create();
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_rvtp_destroy(ctx_);
  }

  struct ut_rvtp_frame narrowFrame(uint64_t epoch, int32_t fpt_slack_ns) {
    struct ut_rvtp_frame frame = {};
    ut_rvtp_narrow_frame(ctx_, &frame, epoch, fpt_slack_ns);
    return frame;
  }

  enum st_rx_tp_compliant feed(const struct ut_rvtp_frame& frame) {
    return ut_rvtp_feed_frame(ctx_, &frame);
  }

  /* Two compliant frames whose per-frame vrx and ipt maxima both descend, so
   * folding the running max off the running MIN yields the second frame's
   * value instead of the first frame's larger one. */
  void feedDescendingPair() {
    ASSERT_EQ(feed(narrowFrame(kFirstEpoch, kLargeVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
        << ut_rvtp_last_cause(ctx_);
    first_vrx_max_ = ut_rvtp_last_vrx_max(ctx_);
    first_ipt_max_ = ut_rvtp_last_ipt_max(ctx_);

    struct ut_rvtp_frame second = narrowFrame(kFirstEpoch + 1, kSmallVrxSlackNs);
    second.pkt_spacing_ns = kTightSpacingNs;
    ASSERT_EQ(feed(second), ST_RX_TP_COMPLIANT_NARROW) << ut_rvtp_last_cause(ctx_);
    second_vrx_max_ = ut_rvtp_last_vrx_max(ctx_);
    second_ipt_max_ = ut_rvtp_last_ipt_max(ctx_);
  }

  ut_rvtp_ctx* ctx_ = nullptr;
  int32_t first_vrx_max_ = 0;
  int32_t first_ipt_max_ = 0;
  int32_t second_vrx_max_ = 0;
  int32_t second_ipt_max_ = 0;
};

/* A session's very first frame has no predecessor to measure a delta against,
 * so it must not be charged an rtp_ts_delta violation. */
TEST_F(St20RxTimingParserTest, FirstFrameNarrowWithoutPredecessorDelta) {
  EXPECT_EQ(feed(narrowFrame(kFirstEpoch, kSmallVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
      << ut_rvtp_last_cause(ctx_);
  EXPECT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_), 0) << "no delta could be measured";
}

/* One frame period between two frames is the exact expected delta. */
TEST_F(St20RxTimingParserTest, SecondFrameWithExpectedDeltaStaysNarrow) {
  ASSERT_EQ(feed(narrowFrame(kFirstEpoch, kSmallVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
      << ut_rvtp_last_cause(ctx_);

  EXPECT_EQ(feed(narrowFrame(kFirstEpoch + 1, kSmallVrxSlackNs)),
            ST_RX_TP_COMPLIANT_NARROW)
      << ut_rvtp_last_cause(ctx_);
  EXPECT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_), UT_RVTP_TICKS_PER_FRAME);
}

/* A measured delta one tick short of a frame period is a TX defect. */
TEST_F(St20RxTimingParserTest, SecondFrameWithDeltaBelowMinFails) {
  ASSERT_EQ(feed(narrowFrame(kFirstEpoch, kSmallVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
      << ut_rvtp_last_cause(ctx_);

  struct ut_rvtp_frame second = narrowFrame(kFirstEpoch + 1, kSmallVrxSlackNs);
  second.rtp_tmstamp -= 1;
  EXPECT_EQ(feed(second), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut_rvtp_last_cause(ctx_), "rtp_ts_delta exceed min");
  EXPECT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_), UT_RVTP_TICKS_PER_FRAME - 1);
}

/* A measured delta beyond a frame period plus one tick is a TX defect. */
TEST_F(St20RxTimingParserTest, SecondFrameWithDeltaAboveMaxFails) {
  ASSERT_EQ(feed(narrowFrame(kFirstEpoch, kSmallVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
      << ut_rvtp_last_cause(ctx_);

  struct ut_rvtp_frame second = narrowFrame(kFirstEpoch + 1, kSmallVrxSlackNs);
  second.rtp_tmstamp += 2;
  EXPECT_EQ(feed(second), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut_rvtp_last_cause(ctx_), "rtp_ts_delta exceed max");
  EXPECT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_), UT_RVTP_TICKS_PER_FRAME + 2);
}

/* Two consecutive frames carrying one RTP timestamp measure a delta of exactly
 * zero. That is a real TX defect and must fail, unlike the unmeasured zero the
 * first frame of a session reports. */
TEST_F(St20RxTimingParserTest, RepeatedRtpTimestampFails) {
  const struct ut_rvtp_frame first = narrowFrame(kFirstEpoch, kSmallVrxSlackNs);
  ASSERT_EQ(feed(first), ST_RX_TP_COMPLIANT_NARROW) << ut_rvtp_last_cause(ctx_);

  EXPECT_EQ(feed(first), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut_rvtp_last_cause(ctx_), "rtp_ts_delta exceed min");
  EXPECT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_), 0);
}

/* The interval's vrx max is the largest per-frame vrx max, not the last one. */
TEST_F(St20RxTimingParserTest, StatVrxMaxKeepsLargestFrameMax) {
  ASSERT_NO_FATAL_FAILURE(feedDescendingPair());
  ASSERT_GT(first_vrx_max_, second_vrx_max_) << "descending maxima make the fold visible";

  EXPECT_EQ(ut_rvtp_stat_vrx_max(ctx_), first_vrx_max_);
}

/* The interval's ipt max is the largest per-frame ipt max, not the last one. */
TEST_F(St20RxTimingParserTest, StatIptMaxKeepsLargestFrameMax) {
  ASSERT_NO_FATAL_FAILURE(feedDescendingPair());
  ASSERT_GT(first_ipt_max_, second_ipt_max_) << "descending maxima make the fold visible";

  EXPECT_EQ(ut_rvtp_stat_ipt_max(ctx_), first_ipt_max_);
}

/* The reported delta average is the mean over the frames that measured a delta.
 * The first frame of the session measures none, so it must not be a divisor. */
TEST_F(St20RxTimingParserTest, StatDeltaAverageDividesByMeasuredFrames) {
  for (uint64_t epoch = kFirstEpoch; epoch < kFirstEpoch + 3; epoch++) {
    ASSERT_EQ(feed(narrowFrame(epoch, kSmallVrxSlackNs)), ST_RX_TP_COMPLIANT_NARROW)
        << ut_rvtp_last_cause(ctx_);
    ASSERT_EQ(ut_rvtp_last_rtp_ts_delta(ctx_),
              epoch == kFirstEpoch ? 0 : UT_RVTP_TICKS_PER_FRAME);
  }

  const std::string log = ut_session::capture_stderr([&] { ut_rvtp_invoke_stat(ctx_); });

  ASSERT_NE(log.find("RTP TS DELTA"), std::string::npos) << log;
  EXPECT_NEAR(parseDeltaAvg(log), UT_RVTP_TICKS_PER_FRAME, 0.01) << log;
}
