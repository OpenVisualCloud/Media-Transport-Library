/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Sequence and timestamp boundary behavior:
 * 16-bit seq wrap (with and without redundancy), 32-bit timestamp wrap,
 * backward-timestamp rejection, back-to-back frames.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxTimestampTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxTimestampTest : public St40RxBaseTest {};

/* 16-bit sequence number wraparound (65533 → 65535 → 0 → 1).
 * All packets in order on a single port. Expects zero unrecovered. */
TEST_F(St40RxTimestampTest, SeqWrapAround) {
  constexpr uint32_t ts1 = 1000;
  constexpr uint32_t ts2 = 2000;

  /* first frame ends at 65535 */
  feed(65533, ts1, false, MTL_SESSION_PORT_P);
  feed(65534, ts1, false, MTL_SESSION_PORT_P);
  feed(65535, ts1, true, MTL_SESSION_PORT_P);

  /* second frame wraps to 0 */
  feed(0, ts2, false, MTL_SESSION_PORT_P);
  feed(1, ts2, false, MTL_SESSION_PORT_P);
  feed(2, ts2, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(received(), 6u);
}

/* Sequence wraparound with redundancy: both ports send the same packets
 * across the 16-bit seq_id boundary. Expects correct redundant count. */
TEST_F(St40RxTimestampTest, SeqWrapAroundRedundant) {
  constexpr uint32_t ts = 1000;

  /* port 0: seq 65534,65535,0 */
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  feed(0, ts, true, MTL_SESSION_PORT_P);

  /* port 1: same packets */
  feed(65534, ts, false, MTL_SESSION_PORT_R);
  feed(65535, ts, false, MTL_SESSION_PORT_R);
  feed(0, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 3u);
}

/* 32-bit timestamp wraparound from near UINT32_MAX to near zero.
 * Expects all packets received with zero unrecovered. */
TEST_F(St40RxTimestampTest, TimestampWrapAround) {
  constexpr uint32_t ts1 = 0xFFFFFFF0;
  constexpr uint32_t ts2 = 0x00000010; /* wraps around */

  feed_burst(0, 4, ts1, true, MTL_SESSION_PORT_P);
  feed_burst(4, 4, ts2, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(received(), 8u);
}

/* Backward timestamp with advancing seq: a newer-ts frame is followed by
 * a packet with an older timestamp but higher seq_id.
 * The filter must reject based on the stale timestamp. */
TEST_F(St40RxTimestampTest, OldTimestampNewSeq) {
  constexpr uint32_t ts_new = 2000;
  constexpr uint32_t ts_old = 1000;

  /* first frame with newer timestamp */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);

  /* second "frame" has older timestamp but advancing seq — should be filtered */
  int rc = feed(4, ts_old, true, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Packet with old timestamp should be rejected";
  EXPECT_EQ(redundant(), 1u);
}

/* Back-to-back frames on a single port: 10 frames × 4 packets each.
 * No gaps, no redundancy. Expects exact received count of 40. */
TEST_F(St40RxTimestampTest, BackToBackFrames) {
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int f = 0; f < 10; f++) {
    feed_burst(seq, 4, ts, true, MTL_SESSION_PORT_P);
    seq += 4;
    ts += 1501;
  }

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 40u);
}
