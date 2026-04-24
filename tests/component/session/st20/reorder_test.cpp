/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Intra-frame reorder accounting:
 * the per-port reordered_packets counter, in-order baseline, per-port
 * isolation, frame-boundary leak, and reorder-then-duplicate.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxReorderTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxReorderTest : public St20RxBaseTest {};

/* Each new frame (new RTP timestamp) opens a fresh slot with last_pkt_idx
 * starting at 0 — in-order delivery must not be misclassified as reorder,
 * neither on the active port nor on the inactive port. */
TEST_F(St20RxReorderTest, InOrderDoesNotBumpReorder) {
  feed_full(1000, MTL_SESSION_PORT_P); /* in-order, frame 1 */
  feed_full(1001, MTL_SESSION_PORT_P); /* in-order, frame 2 */

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
}

/* Reorder followed by a duplicate of the late packet must count exactly one
 * reorder and one redundant — never a second reorder. */
TEST_F(St20RxReorderTest, ReorderThenDuplicateNotDoubleCounted) {
  feed(1, 1000, MTL_SESSION_PORT_P); /* sets last_pkt_idx=1 */
  feed(0, 1000, MTL_SESSION_PORT_P); /* reorder: bit not yet set */
  feed(0, 1000, MTL_SESSION_PORT_P); /* duplicate: bit already set */

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(redundant(), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
}

/* Regression: after a reorder, slot->last_pkt_idx must NOT regress, otherwise
 * the next forward jump re-counts already-lost packets as new losses.
 *
 * Sequence on a single port (line_num=0 for all so the frame offset check
 * does not interfere; pkt_idx is derived purely from RTP seq):
 *   seq 1000 → pkt_idx 0, base established
 *   seq 1005 → pkt_idx 5, forward jump → gap = 4 lost (pkts 1..4)
 *   seq 1003 → pkt_idx 3, reorder (recovers one of the "lost" pkts)
 *   seq 1006 → pkt_idx 6, only pkt 4 is a NEW loss between the prior
 *              high-water mark (5) and 6
 *
 * Expected port_lost == 4 (the four lost in the first forward jump; the
 * implementation does not decrement on reorder recovery, but it must NOT
 * inflate by another 2 just because last_pkt_idx regressed to 3). */
TEST_F(St20RxReorderTest, ReorderDoesNotRegressLastPktIdx) {
  /* Payload 10 bytes/pkt so 4 pkts (40B) fit in the 80B harness frame
   * without auto-closing the slot at frame_recv_size >= frame_size. */
  ut20_feed_pkt(ctx_, 1000, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1005, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1003, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1006, 1000, 0, 0, 10, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 4u)
      << "If last_pkt_idx regresses on reorder, pkt 6 over-counts lost as "
         "6-3-1=2 instead of 0, inflating port_lost from 4 to 6";
}

/* Cross-port variant: slot->last_pkt_idx is per-slot (shared across P+R for
 * the same RTP timestamp). A reorder arriving on one port must not poison
 * the gap arithmetic for the next forward packet on the OTHER port.
 *   1. P sends pkt 0 (last_pkt_idx = 0)
 *   2. R sends pkt 5 (gap = 4 → port_lost(R) += 4, last_pkt_idx = 5)
 *   3. P sends pkt 3 (reorder; last_pkt_idx must STAY 5, not regress)
 *   4. R sends pkt 6 (in-order continuation: gap = 0)
 * Without the high-water-mark guard, step 3 regresses last_pkt_idx to 3,
 * and step 4 computes gap = 6 - 3 - 1 = 2, falsely inflating port_lost(R) to 6. */
TEST_F(St20RxReorderTest, ReorderOnOnePortDoesNotPoisonOtherPort) {
  ut20_feed_pkt(ctx_, 1000, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1005, 1000, 0, 0, 10, MTL_SESSION_PORT_R);
  ut20_feed_pkt(ctx_, 1003, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1006, 1000, 0, 0, 10, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 4u)
      << "Reorder on P must not inflate lost count for the next forward pkt on R";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
}

/* Multi-step reorder: large initial gap, then several reorders fill it in
 * out of order, then resume forward. Ensures the high-water-mark fix holds
 * across a sequence of regressions, and that lost is counted exactly once
 * per missing slot (not re-added on each subsequent forward step). */
TEST_F(St20RxReorderTest, MultipleReordersDoNotInflateLost) {
  /* 8 pkts of 10 bytes each fit the 80B harness frame. */
  ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* pkt 0 */
  ut20_feed_pkt(ctx_, 106, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* pkt 6, gap=5 */
  ut20_feed_pkt(ctx_, 102, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* reorder */
  ut20_feed_pkt(ctx_, 104, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* reorder */
  ut20_feed_pkt(ctx_, 101, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* reorder */
  ut20_feed_pkt(ctx_, 107, 1000, 0, 0, 10, MTL_SESSION_PORT_P); /* in-order, gap=0 */

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 3u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 5u)
      << "Lost must be counted exactly once when pkt 6 arrived; the three "
         "subsequent reorders must not inflate it, and pkt 7 must compute "
         "gap from the high-water mark (6), not the last reordered idx";
}
