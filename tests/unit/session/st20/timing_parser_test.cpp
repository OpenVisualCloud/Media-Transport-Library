/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-21 RX timing parser: which frames the compliance ladder may charge a
 * violation. Three subjects are pinned here: the rtp_ts_delta a session's first
 * frame has no predecessor to measure against, the tick quantum an integer RTP
 * timestamp lends the latency, and the frame the parser measured no packet of at
 * all. Frames are fed through the production RX path, so every frame runs the
 * real per-frame slot reset (rv_slot_by_tmstamp -> rv_tp_slot_init) and is judged
 * by the meta the session reports to the application.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxTimingParserTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
/* Any epoch whose expected RTP timestamp is non-zero. */
constexpr uint64_t kEpoch = 1000;
/* First epoch whose expected RTP timestamp is 0 again: epoch * frame_time_sampling
 * wraps to 0 only on a multiple of 2^32. Epoch 0 itself is unusable, as
 * rv_tp_on_packet reads cur_epochs == 0 as "first packet" and re-enters its
 * first-packet block on every packet of that frame. */
constexpr uint64_t kZeroTmstampEpoch = 1ull << 32;
/* Arrival of a frame's first packet after its epoch, well inside the geometry's
 * tr_offset and latency_max so a paced frame is NARROW. */
constexpr uint64_t kNarrowFptNs = 500000;
/* How far ahead of its epoch a latency case puts the frame's RTP timestamp. It
 * caps the deficit feed_latency can ask for, as fpt must stay non-negative;
 * three ticks leaves room for the 1.1 ticks the cases below use. */
constexpr int32_t kRtpOffsetTicks = 3;
}  // namespace

class St20RxTimingParserTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }
  /* enough packets that trs, vrx and cinst are measured across a frame */
  int pkts_per_frame() const override {
    return 8;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_), 0);
  }

  uint32_t tmstamp_of(uint64_t epoch) {
    return ut20_tp_epoch_tmstamp(ctx_, epoch);
  }

  uint64_t tick_ns() {
    return ut20_tp_tick_ns(ctx_);
  }

  /* Feed one full frame and return the verdict the session reported for it.
   * ut20_tp_last_meta() hands back the copy taken at frame notify, so the frame
   * must have been delivered for the verdict to be this frame's, and every
   * packet must have reached the parser -- no case can pass by measuring
   * nothing. */
  const struct st20_rx_tp_meta* feed_tp(uint64_t epoch, uint32_t ts,
                                        uint64_t fpt_ns = kNarrowFptNs) {
    const int delivered = frames_received();
    ut20_feed_tp_frame(ctx_, epoch, ts, fpt_ns);
    EXPECT_EQ(frames_received(), delivered + 1) << "frame was not delivered";
    const struct st20_rx_tp_meta* tp = ut20_tp_last_meta(ctx_);
    EXPECT_EQ(tp->pkts_cnt, (uint32_t)pkts_per_frame()) << "frame was not measured";
    return tp;
  }

  /* Feed a frame whose first packet lands `latency_ns` short of where its RTP
   * timestamp claims, i.e. one the parser measures that latency for. */
  const struct st20_rx_tp_meta* feed_latency(uint64_t epoch, int64_t latency_ns) {
    const int64_t fpt_ns = kRtpOffsetTicks * (int64_t)tick_ns() + latency_ns;
    EXPECT_GE(fpt_ns, 0) << "deficit exceeds kRtpOffsetTicks";
    return feed_tp(epoch, tmstamp_of(epoch) + kRtpOffsetTicks, (uint64_t)fpt_ns);
  }

  /* Feed a frame that meets every pass criterion, i.e. one whose RTP timestamp
   * is the one its epoch expects. */
  const struct st20_rx_tp_meta* feed_narrow(uint64_t epoch) {
    const struct st20_rx_tp_meta* tp = feed_tp(epoch, tmstamp_of(epoch));
    EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_NARROW) << tp->failed_cause;
    return tp;
  }

  /* Feed a full frame received inside a burst, i.e. one whose every packet the
   * untrusted-pkt filter declines to measure, so its slot reaches
   * rv_tp_slot_parse_result holding nothing but rv_tp_slot_init sentinels. */
  const struct st20_rx_tp_meta* feed_unmeasured(uint64_t epoch) {
    const int delivered = frames_received();
    ut20_ctx_set_continuous_burst(ctx_, MTL_SESSION_PORT_P, true);
    ut20_feed_tp_frame(ctx_, epoch, tmstamp_of(epoch), kNarrowFptNs);
    ut20_ctx_set_continuous_burst(ctx_, MTL_SESSION_PORT_P, false);
    EXPECT_EQ(frames_received(), delivered + 1) << "frame was not delivered";
    const struct st20_rx_tp_meta* tp = ut20_tp_last_meta(ctx_);
    EXPECT_EQ(tp->pkts_cnt, 0u) << "parser measured a packet";
    return tp;
  }

  uint32_t window_cnt(enum st_rx_tp_compliant compliant) {
    return ut20_tp_stat_compliant_cnt(ctx_, MTL_SESSION_PORT_P, compliant);
  }

  int32_t window_fpt_min() {
    return ut20_tp_stat_fpt_min(ctx_, MTL_SESSION_PORT_P);
  }
};

/* A session's first frame has no predecessor timestamp, so no delta can be
 * measured and none may be charged against it. */
TEST_F(St20RxTimingParserTest, FirstFrameNarrowWithoutMeasuredDelta) {
  const struct st20_rx_tp_meta* tp = feed_narrow(kEpoch);

  EXPECT_EQ(tp->rtp_ts_delta, 0) << "no delta was measurable";
}

/* One frame period between two frames is the delta the parser expects. */
TEST_F(St20RxTimingParserTest, ExpectedDeltaStaysNarrow) {
  feed_narrow(kEpoch);

  const struct st20_rx_tp_meta* tp = feed_narrow(kEpoch + 1);
  EXPECT_EQ(tp->rtp_ts_delta, (int32_t)(tmstamp_of(kEpoch + 1) - tmstamp_of(kEpoch)));
}

TEST_F(St20RxTimingParserTest, DeltaOneTickShortFails) {
  feed_narrow(kEpoch);

  const struct st20_rx_tp_meta* tp = feed_tp(kEpoch + 1, tmstamp_of(kEpoch + 1) - 1);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "rtp_ts_delta exceed min");
}

TEST_F(St20RxTimingParserTest, DeltaTwoTicksLongFails) {
  feed_narrow(kEpoch);

  const struct st20_rx_tp_meta* tp = feed_tp(kEpoch + 1, tmstamp_of(kEpoch + 1) + 2);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "rtp_ts_delta exceed max");
}

/* The smallest delta the RX path can actually deliver: a transmitter whose
 * timestamp advanced one tick instead of one frame period. It was measured, so
 * the guard that spares a first frame must not spare it. A repeated timestamp
 * would give a delta of 0, but rv_slot_by_tmstamp answers the slot already
 * holding it, whose cur_epochs is set, so rv_tp_on_packet's first-packet block
 * never re-enters and no second delta is computed. */
TEST_F(St20RxTimingParserTest, SingleTickDeltaFails) {
  feed_narrow(kEpoch);

  const struct st20_rx_tp_meta* tp = feed_tp(kEpoch + 1, tmstamp_of(kEpoch) + 1);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "rtp_ts_delta exceed min");
  EXPECT_EQ(tp->rtp_ts_delta, 1);
}

/* 0 is a legal RTP timestamp, not "no predecessor". A frame carrying it must
 * still leave the next frame's delta measurable. */
TEST_F(St20RxTimingParserTest, ZeroTimestampStillMeasuresNextDelta) {
  ASSERT_EQ(tmstamp_of(kZeroTmstampEpoch), 0u);
  feed_narrow(kZeroTmstampEpoch);

  const struct st20_rx_tp_meta* tp =
      feed_tp(kZeroTmstampEpoch + 1, tmstamp_of(kZeroTmstampEpoch + 1) + 2);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "rtp_ts_delta exceed max");
}

/* An RTP timestamp names an instant on a 90 kHz grid, so a frame arriving
 * exactly on time still measures a latency anywhere within one tick below zero.
 * Such a frame is compliant. */
TEST_F(St20RxTimingParserTest, LatencyInsideOneRtpTickStaysNarrow) {
  const int32_t tick = (int32_t)tick_ns();

  const struct st20_rx_tp_meta* tp = feed_latency(kEpoch, -tick + tick / 10);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_NARROW) << tp->failed_cause;
  EXPECT_LT(tp->latency, 0) << "case no longer measures a negative latency";
  EXPECT_GE(tp->latency, -tick);
}

/* A tenth of a tick past the quantum the deficit is no longer quantisation, so
 * the criterion must still charge it. Together with the case above this pins
 * the floor to within a tenth of one tick, and only the floor moved --
 * latency_max is unchanged. */
TEST_F(St20RxTimingParserTest, LatencyBeyondOneRtpTickFails) {
  const int32_t tick = (int32_t)tick_ns();

  const struct st20_rx_tp_meta* tp = feed_latency(kEpoch, -tick - tick / 10);
  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "latency exceed min");
}

/* A frame the parser measured no packet of carries no observation of the sender,
 * so the window may not count it against any verdict. */
TEST_F(St20RxTimingParserTest, UnmeasuredFrameNotCountedInWindow) {
  feed_narrow(kEpoch);
  ASSERT_EQ(window_cnt(ST_RX_TP_COMPLIANT_NARROW), 1u);

  feed_unmeasured(kEpoch + 1);
  EXPECT_EQ(window_cnt(ST_RX_TP_COMPLIANT_FAILED), 0u);
  EXPECT_EQ(window_cnt(ST_RX_TP_COMPLIANT_WIDE), 0u);
  EXPECT_EQ(window_cnt(ST_RX_TP_COMPLIANT_NARROW), 1u);
}

/* Nor may its fpt: 0 is what memset left, not an arrival the parser timed, and
 * the window takes its FPT MIN with RTE_MIN. */
TEST_F(St20RxTimingParserTest, UnmeasuredFrameLeavesWindowFptMin) {
  feed_narrow(kEpoch);
  const int32_t fpt_min = window_fpt_min();
  ASSERT_EQ(fpt_min, (int32_t)kNarrowFptNs);

  feed_unmeasured(kEpoch + 1);
  EXPECT_EQ(window_fpt_min(), fpt_min);
}

/* The frame is still reported to the application, so its verdict must name what
 * happened instead of a criterion only the sentinels tripped. */
TEST_F(St20RxTimingParserTest, UnmeasuredFrameReportsNoMeasurement) {
  const struct st20_rx_tp_meta* tp = feed_unmeasured(kEpoch);

  EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(tp->failed_cause, "no packet measured");
}

/* An unmeasured frame observed no timestamp to become the next frame's
 * predecessor, so the frame measured after such a run has no delta to measure.
 * Charging it the run's worth of frame periods would fail a sender that paced
 * every frame correctly. */
TEST_F(St20RxTimingParserTest, MeasuredFrameAfterUnmeasuredRunStaysNarrow) {
  feed_narrow(kEpoch);
  feed_unmeasured(kEpoch + 1);
  feed_unmeasured(kEpoch + 2);

  const struct st20_rx_tp_meta* tp = feed_narrow(kEpoch + 3);
  EXPECT_EQ(tp->rtp_ts_delta, 0) << "no delta was measurable";
  EXPECT_EQ(window_cnt(ST_RX_TP_COMPLIANT_FAILED), 0u);
}
