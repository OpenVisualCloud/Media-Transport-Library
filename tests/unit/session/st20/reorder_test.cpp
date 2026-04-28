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

/* Cross-port: each port's `last_pkt_idx` is tracked separately, so a
 * packet arriving on one port can never poison the gap or reorder
 * arithmetic for the next packet on the OTHER port. Every port's
 * loss/reorder counter reflects only events on its own stream.
 *
 *   1. P sends pkt 0 (P_last = 0)
 *   2. R sends pkt 5 (first pkt on R; no gap counted, R_last = 5)
 *   3. P sends pkt 3 (P_last = 0 → forward jump on P: gap = 2 lost on P)
 *   4. R sends pkt 6 (R_last = 5 → in-order on R: no gap, no reorder)
 *
 * P's forward jump must NOT register as reorder (it's a gap on P), and R's
 * counters must be untouched by anything happening on P. */
TEST_F(St20RxReorderTest, ReorderOnOnePortDoesNotPoisonOtherPort) {
  ut20_feed_pkt(ctx_, 1000, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1005, 1000, 0, 0, 10, MTL_SESSION_PORT_R);
  ut20_feed_pkt(ctx_, 1003, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1006, 1000, 0, 0, 10, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u)
      << "P sent pkts 0 then 3: forward gap on P, not reorder";
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u)
      << "P's forward jump from pkt 0 to pkt 3 implies 2 missing on P's stream";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 0u)
      << "R sent pkt 5 then pkt 6: first-pkt establishes R_last, then in-order";
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

/* Sustained cross-wire interleaving across many frames: every frame is
 * supplied by both wires with the secondary arriving first, but each
 * wire delivers its own single packet in order. Verifies that neither
 * reorder counter accumulates spurious increments under prolonged
 * cross-wire late arrivals — i.e. cross-port arrival order is never
 * classified as own-stream reorder. */
TEST_F(St20RxReorderTest, RepeatedCrossPortTransitionsDoNotInflateReorder) {
  constexpr int N = 8;
  for (int i = 0; i < N; i++) {
    uint32_t ts = 1000 + 1000u * static_cast<uint32_t>(i);
    feed(1, ts, MTL_SESSION_PORT_R);
    feed(0, ts, MTL_SESSION_PORT_P);
  }
  EXPECT_EQ(frames_received(), N);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wide-frame own-port reorder.
 *
 * Default 2-pkt geometry cannot express an intra-frame own-stream reorder
 * that does not collide with the first-pkt branch. This test uses a
 * 4-pkt geometry to send pkt 2 then pkt 1 on the same wire while the
 * other wire delivers in order.
 * ───────────────────────────────────────────────────────────────────────── */

class St20RxReorderWideTest : public ::testing::Test {
 protected:
  static constexpr int kPktsPerFrame = 4;
  ut20_test_ctx* ctx_ = nullptr;
  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create_geom(2, kPktsPerFrame);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }
  void feed(int pkt_idx, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt(ctx_, pkt_idx, ts, port);
  }
  int frames_received() {
    return ut20_frames_received(ctx_);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut20_stat_port_reordered(ctx_, p);
  }
};

/* R delivers pkt 2 then pkt 1 within the same frame on the same wire.
 * This is a true own-stream reorder on R and must be counted exactly
 * once on R, with P's reorder counter untouched. */
TEST_F(St20RxReorderWideTest, OwnPortReorderCountedOnThatPortOnly) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1000, MTL_SESSION_PORT_R); /* R first; R_last = 2 */
  feed(1, 1000, MTL_SESSION_PORT_R); /* R own-stream reorder: 1 < 2 */
  feed(3, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 1u)
      << "R received pkt_idx 1 after pkt_idx 2 on its own wire";
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}
