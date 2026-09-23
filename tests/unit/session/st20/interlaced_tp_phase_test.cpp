/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the interlaced field-phase check in `rv_tp_slot_parse_result()`
 * (st_rx_timing_parser.c): per the ST 2110-21:2022 6.2 frame grid an even epoch
 * slot carries the first field and an odd one the second. A stream running a field
 * out of phase keeps a conforming FPT -- FPT is measured against the *field* epoch
 * -- so no other gate of the timing parser can see it. Why the check is keyed to
 * the arrival epoch rather than the RTP timestamp: doc/design.md 6.6.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest \
 *          --gtest_filter='St20RxInterlacedTpPhaseTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

namespace {
/* Must match the harness session geometry: ST_FPS_P30 read as a FIELD rate, so
 * one epoch slot is one field and the ST 2110-21 frame grid is every second. */
constexpr uint64_t kFieldPeriodNs = 33333333;
constexpr uint32_t kTicksPerField = 3003;
/* First-packet offset into the slot. Well inside TROFFSET (~1.3ms here), so the
 * fpt gate passes and the parity check is the only thing that can fail. */
constexpr uint64_t kFirstPktOffsetNs = 1000;
constexpr uint64_t kEvenEpoch = 1000;
constexpr uint64_t kOddEpoch = kEvenEpoch + 1;
constexpr char kParityCause[] = "field parity off frame grid";
}  // namespace

class St20RxInterlacedTpPhaseTest : public St20RxBaseTest {
 protected:
  int num_port() const override {
    return 1;
  }

  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_hw_timestamp(ctx_, MTL_SESSION_PORT_P);
    ASSERT_EQ(ut20_ctx_enable_timing_parser(ctx_, /*interlaced=*/true), 0);
  }

  /* Feeds one complete first field (no ST20_SECOND_FIELD in the row number)
   * arriving inside the given epoch slot, with the RTP timestamp on that slot's
   * boundary so rtp_offset and latency both stay compliant. */
  void FeedFirstFieldInEpoch(uint64_t epoch) {
    const uint64_t arrival_ns = epoch * kFieldPeriodNs + kFirstPktOffsetNs;
    const uint32_t rtp_timestamp = (uint32_t)(epoch * kTicksPerField);
    for (int i = 0; i < pkts_per_frame(); i++) {
      ut20_feed_frame_pkt_hw_ts(ctx_, i, rtp_timestamp, MTL_SESSION_PORT_P, arrival_ns);
    }
  }

  /* The parser reports rtp_ts_delta against the previous frame, so the very
   * first frame of a stream structurally fails ("rtp_ts_delta exceed min").
   * Every case therefore primes with the preceding slot and asserts on the
   * second frame. */
  void PrimeThenFeedFirstFieldInEpoch(uint64_t epoch) {
    FeedFirstFieldInEpoch(epoch - 1);
    ASSERT_EQ(frames_received(), 1);
    FeedFirstFieldInEpoch(epoch);
    ASSERT_EQ(frames_received(), 2);
  }
};

/* A first field on an even slot is on the frame grid. Asserted as a positive
 * NARROW verdict, not merely "not FAILED", so neither this case nor the odd-slot
 * one below can pass just because no verdict was delivered. */
TEST_F(St20RxInterlacedTpPhaseTest, FirstFieldOnEvenEpochIsCompliant) {
  PrimeThenFeedFirstFieldInEpoch(kEvenEpoch);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_NARROW)
      << "cause: " << ut20_last_tp_failed_cause(ctx_);
  EXPECT_STRNE(ut20_last_tp_failed_cause(ctx_), kParityCause);
}

/* The same field one slot later is half a frame out of phase, yet every other gate
 * still passes -- fpt included -- so without this check the stream reads as
 * compliant. */
TEST_F(St20RxInterlacedTpPhaseTest, FirstFieldOnOddEpochFailsParity) {
  PrimeThenFeedFirstFieldInEpoch(kOddEpoch);

  EXPECT_EQ(ut20_last_tp_compliant(ctx_), ST_RX_TP_COMPLIANT_FAILED);
  EXPECT_STREQ(ut20_last_tp_failed_cause(ctx_), kParityCause);
}
