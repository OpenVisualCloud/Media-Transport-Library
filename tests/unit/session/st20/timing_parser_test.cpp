/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-21 RX timing parser: which frames may be charged an rtp_ts_delta
 * violation and which have no delta to measure at all. Frames are fed through
 * the production RX path, so every frame runs the real per-frame slot reset
 * (rv_slot_by_tmstamp -> rv_tp_slot_init) and is judged by the meta the session
 * reports to the application.
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

  /* Feed one full frame and return the verdict the session reported for it.
   * ut20_tp_last_meta() hands back the copy taken at frame notify, so the frame
   * must have been delivered for the verdict to be this frame's, and every
   * packet must have reached the parser -- no case can pass by measuring
   * nothing. */
  const struct st20_rx_tp_meta* feed_tp(uint64_t epoch, uint32_t ts) {
    const int delivered = frames_received();
    ut20_feed_tp_frame(ctx_, epoch, ts);
    EXPECT_EQ(frames_received(), delivered + 1) << "frame was not delivered";
    const struct st20_rx_tp_meta* tp = ut20_tp_last_meta(ctx_);
    EXPECT_EQ(tp->pkts_cnt, (uint32_t)pkts_per_frame()) << "frame was not measured";
    return tp;
  }

  /* Feed a frame that meets every pass criterion, i.e. one whose RTP timestamp
   * is the one its epoch expects. */
  const struct st20_rx_tp_meta* feed_narrow(uint64_t epoch) {
    const struct st20_rx_tp_meta* tp = feed_tp(epoch, tmstamp_of(epoch));
    EXPECT_EQ(tp->compliant, ST_RX_TP_COMPLIANT_NARROW) << tp->failed_cause;
    return tp;
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
