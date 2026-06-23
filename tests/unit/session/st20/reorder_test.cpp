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

/* Regression: after a reorder, slot->last_pkt_idx must NOT regress, else the
 * next forward jump re-counts already-seen packets. Single port, line_num=0
 * so pkt_idx comes purely from RTP seq:
 *   seq 1000→idx 0, 1005→idx 5, 1003→idx 3 (reorder), 1006→idx 6.
 * Four distinct packets (0,3,5,6) of the eight-packet frame arrive. */
TEST_F(St20RxReorderTest, ReorderDoesNotRegressLastPktIdx) {
  /* 10 bytes/pkt so the slot does not auto-close before all four arrive. */
  ut20_feed_pkt(ctx_, 1000, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1005, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1003, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1006, 1000, 0, 0, 10, MTL_SESSION_PORT_P);

  /* Before recycle: reorder counted once, loss is deferred (still 0). */
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u) << "loss is deferred, not inline";

  /* After recycle: deficit = span(8) - recv_on_P(4) = 4, charged once. */
  flush();
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 4u);
}

/* Cross-port: each port tracks its own last_pkt_idx, so an arrival on one
 * wire never poisons gap/reorder math on the other.
 *   P→idx 0, R→idx 5, P→idx 3, R→idx 6.
 * P's forward jump is a gap (not reorder); R stays in order. */
TEST_F(St20RxReorderTest, ReorderOnOnePortDoesNotPoisonOtherPort) {
  ut20_feed_pkt(ctx_, 1000, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1005, 1000, 0, 0, 10, MTL_SESSION_PORT_R);
  ut20_feed_pkt(ctx_, 1003, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 1006, 1000, 0, 0, 10, MTL_SESSION_PORT_R);

  /* Before recycle: no reorder on either wire, no inline loss. */
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u) << "P gap, not reorder";
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u) << "R untouched by P";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 0u);

  /* After recycle: both wires delivered the same count of a symmetric pattern,
   * so each carries an identical, non-zero deficit. Asserting symmetry (not a
   * literal value tied to the harness geometry/estimator) is what proves the
   * non-poisoning property: neither wire's pattern skews the other's loss. */
  flush();
  EXPECT_GT(port_lost(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), port_lost(MTL_SESSION_PORT_R));
}

/* Multi-step reorder: large initial gap, several reorders fill it out of
 * order, then resume forward. Confirms the high-water mark holds and loss is
 * counted once per missing slot, not re-added on each forward step. */
TEST_F(St20RxReorderTest, MultipleReordersDoNotInflateLost) {
  /* idx 0,6,2,4,1,7 → six distinct packets of the 8-packet frame. */
  ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 106, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 102, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 104, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 101, 1000, 0, 0, 10, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 107, 1000, 0, 0, 10, MTL_SESSION_PORT_P);

  /* Before recycle: three reorders, loss deferred (still 0). */
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 3u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);

  /* After recycle: deficit = span(8) - recv_on_P(6) = 2, counted once
   * despite the repeated reorders. */
  flush();
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u);
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
