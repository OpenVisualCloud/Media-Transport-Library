/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * RFC 8331 F-bit / interlace handling:
 * progressive vs interlaced, field counters, cross-port F-bit divergence,
 * interlace-auto detection.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxFBitsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxFBitsTest : public St40RxBaseTest {};

/* Invalid F-bits packets are dropped and counted in wrong_interlace stat. */
TEST_F(St40RxFBitsTest, InvalidFBitsDropped) {
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x1);

  EXPECT_EQ(wrong_interlace(), 2u);
  EXPECT_EQ(received(), 0u);
}

/* Progressive F-bits (0b00) are accepted without incrementing interlace counters. */
TEST_F(St40RxFBitsTest, ProgressiveFBits) {
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x0);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x0);

  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(interlace_first(), 0u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_EQ(received(), 2u);
}

/* Interlaced fields: F=0b10 (first) and F=0b11 (second) are accepted
 * and tracked in their respective interlace counters. */
TEST_F(St40RxFBitsTest, InterlaceFieldCounting) {
  /* F=0x2 → first field */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x2);
  /* F=0x3 → second field */
  ut40_feed_pkt_fbits(ctx_, 2, 1002, 0, MTL_SESSION_PORT_P, 0x3);

  EXPECT_EQ(interlace_first(), 2u);
  EXPECT_EQ(interlace_second(), 1u);
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(received(), 3u);
}

/* Producer-side defect: port P emits F=0x2 (first field) while port R
 * emits F=0x3 (second field) for the same logical frame.  This is a
 * SMPTE 2110-40 violation.  MTL currently accepts both polarities
 * silently; this test pins that behaviour. */
TEST_F(St40RxFBitsTest, AsymmetricFieldBitsBetweenPorts) {
  /* P sends F=0x2, R sends F=0x3 for the same ts — disagreement. */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1000, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_R, 0x3);
  ut40_feed_pkt_fbits(ctx_, 1, 1000, 1, MTL_SESSION_PORT_R, 0x3);

  /* Current behaviour: both polarities are counted, no drop. */
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_GE(interlace_first(), 2u);  /* F=0x2 from P */
  EXPECT_GE(interlace_second(), 2u); /* F=0x3 from R */
}

/* Mid-stream field-bit flip in interlace_auto mode: producer suddenly
 * starts emitting progressive packets after a run of interlaced ones.
 * Detection must re-fire (interlace_detected reset) and progressive
 * packets must not be counted in either field bucket. */
TEST_F(St40RxFBitsTest, InterlaceAutoFieldBitFlip) {
  ut40_ctx_set_interlace_auto(ctx_, true);
  /* warm up with interlaced first-field (F=0x2) */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x2);
  EXPECT_EQ(interlace_first(), 2u);
  /* now flip to progressive (F=0x0) */
  ut40_feed_pkt_fbits(ctx_, 2, 1002, 0, MTL_SESSION_PORT_P, 0x0);
  ut40_feed_pkt_fbits(ctx_, 3, 1003, 0, MTL_SESSION_PORT_P, 0x0);
  /* progressive packets must NOT bump field counters */
  EXPECT_EQ(interlace_first(), 2u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(received(), 4u);
}

/* Cross-port F-bit divergence: same timestamp, same seq on each port, but
 * different F bits. Producer violation of SMPTE 2110-40. */
TEST_F(St40RxFBitsTest, FieldBitMismatchDetected) {
  constexpr uint32_t ts = 1000;
  /* P sends F=0x2, R sends F=0x3 for the same frame (same tmstamp). */
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_R, 0x3);

  EXPECT_GE(field_bit_mismatch(), 1u) << "Cross-port F-bit divergence must be detected";
}

/* Matching F bits on both ports must NOT register a mismatch. */
TEST_F(St40RxFBitsTest, FieldBitsMatchNoMismatch) {
  constexpr uint32_t ts = 1000;
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_R, 0x2);

  EXPECT_EQ(field_bit_mismatch(), 0u)
      << "Matching F bits must not trigger the mismatch counter";
}

/* F-bit mismatches must be counted per packet — one mismatch per packet. */
TEST_F(St40RxFBitsTest, FieldBitMismatchMultiple) {
  uint32_t ts = 1000;
  for (uint16_t i = 0; i < 5; i++) {
    ut40_feed_pkt_fbits(ctx_, i, ts, 1, MTL_SESSION_PORT_P, 0x2);
    ut40_feed_pkt_fbits(ctx_, i, ts, 1, MTL_SESSION_PORT_R, 0x3);
    ts += 3600;
  }
  EXPECT_GE(field_bit_mismatch(), 5u) << "One mismatch per offending packet expected";
}

/* F-bit check must be scoped to the *same* timestamp: different ts on each
 * port (intentional skew test) must not produce a mismatch. */
TEST_F(St40RxFBitsTest, FieldBitMismatchOnlyWhenSameTs) {
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, 2000, 1, MTL_SESSION_PORT_R, 0x3);

  EXPECT_EQ(field_bit_mismatch(), 0u)
      << "Different timestamps: F bits belong to different frames, no divergence";
}
