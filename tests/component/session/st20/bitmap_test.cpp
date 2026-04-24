/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Sequence-id and bitmap edge cases:
 * 32-bit seq wrap, packet index outside the bitmap, intra-frame ordering,
 * cross-port interleaving to fill a frame.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxBitmapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxBitmapTest : public St20RxBaseTest {};

/* 32-bit seq_id wrap: base = 0xFFFFFFFF, next seq = 0 must take the wrap
 * branch (seq_id_u32 < seq_id_base_u32) and resolve to pkt_idx == 1. */
TEST_F(St20RxBitmapTest, SeqIdWrapAround32) {
  feed_seq(0, 0xFFFFFFFF, 1000, MTL_SESSION_PORT_P);
  feed_seq(1, 0x00000000, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
}

/* Wrap with a forward gap: base = 0xFFFFFFFE, then jump straight to seq = 0
 * (skipping 0xFFFFFFFF). With UT20 geometry (2 pkts/frame) we feed
 * pkt_idx 0 and pkt_idx 1, but make pkt_idx 1's seq wrap past one missing
 * value. Wrap formula yields pkt_idx == 2 internally — but our bitmap is
 * 8 bits so it stays in range and the gap arithmetic is exercised across
 * the 32-bit boundary. We use ut20_feed_pkt directly to keep line_num
 * within the frame regardless of the resolved pkt_idx. */
TEST_F(St20RxBitmapTest, SeqIdWrapAround32WithGap) {
  /* First pkt: seq=0xFFFFFFFE, line 0 → establishes base_u32 = 0xFFFFFFFE. */
  ut20_feed_pkt(ctx_, 0xFFFFFFFE, 1000, 0, 0, 40, MTL_SESSION_PORT_P);
  /* Second pkt: seq=0x00000000 → wrap branch, pkt_idx = 0 + (0xFFFFFFFF -
   * 0xFFFFFFFE) + 1 = 2. Use line 1 so the offset stays inside the frame. */
  ut20_feed_pkt(ctx_, 0x00000000, 1000, 1, 0, 40, MTL_SESSION_PORT_P);

  /* Both packets accepted; the missing seq=0xFFFFFFFF (pkt_idx 1) counted lost. */
  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
}

/* Reorder across the wrap: pkt_idx 1 (seq 0) arrives before pkt_idx 0
 * (seq 0xFFFFFFFF). The first-packet path computes base = seq - pkt_idx,
 * so base = 0 - 1 = 0xFFFFFFFF, and the late seq 0xFFFFFFFF then resolves
 * via the non-wrap branch to pkt_idx 0. Both packets must be accepted. */
TEST_F(St20RxBitmapTest, ReorderAcrossWrapAccepted) {
  feed_seq(1, 0x00000000, 1000, MTL_SESSION_PORT_P); /* base = 0xFFFFFFFF */
  feed_seq(0, 0xFFFFFFFF, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(redundant(), 0u);
}

/* Packet index outside the bitmap range is rejected and counted in
 * stat_pkts_idx_oo_bitmap. The bitmap is 1 byte = 8 bits, so pkt_idx >= 8
 * is out of range. */
TEST_F(St20RxBitmapTest, PktIdxOutOfBitmap) {
  /* First pkt establishes seq_id_base */
  feed_seq(0, 100, 1000, MTL_SESSION_PORT_P);

  /* Now send a pkt whose seq gives pkt_idx = 100 (base=100, seq=200 → idx=100).
   * Use line 0 / offset 0 / length 40 — the line fields don't matter for this
   * test, only the seq_id distance from base matters. */
  ut20_feed_pkt(ctx_, 200, 1000, 0, 0, 40, MTL_SESSION_PORT_P);

  EXPECT_GE(idx_oo_bitmap(), 1u) << "Out-of-bitmap pkt_idx should be rejected";
}

/* Out-of-order packets within a frame: pkt 1 arrives before pkt 0.
 * Both must be accepted (bitmap allows any order). The backward arrival
 * must be counted as intra-frame reorder, must not inflate lost or
 * redundant counters, and must not cause OOO underflow from negative gap. */
TEST_F(St20RxBitmapTest, OutOfOrderWithinFrame) {
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(ooo(), 0u)
      << "Backward pkt_idx should not cause OOO underflow or double-counting";
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u)
      << "pkt_idx 0 arriving after pkt_idx 1 must count as intra-frame reorder";
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
}

/* Interleaved ports fill one frame: P sends pkt 0, R sends pkt 1.
 * The frame should complete as reconstructed from both ports. */
TEST_F(St20RxBitmapTest, InterleavedPortsFillFrame) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(redundant(), 0u);
}
