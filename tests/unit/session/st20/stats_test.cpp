/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-port and session-wide stats invariants for ST 2110-20:
 * per-port packet counters, lost-packets composition invariant, and the
 * received/redundant accounting invariant under cross-port redundancy.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxStatsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxStatsTest : public St20RxBaseTest {};

/* Per-port packet counters (port_user_stats.common.port[].packets) are
 * bumped by the _handle_mbuf wrapper. Tests below feed via the wrapper
 * to exercise that accounting path; the non-wrapper helpers in the base
 * fixture skip the wrapper layer and are therefore not used here. */

/* Bitmap-redundant arrivals on the secondary port still bump per-port
 * counters: per-port stats represent wire-level RX, not the merged stream. */
TEST_F(St20RxStatsTest, RedundantStillCountsPerPort) {
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(redundant(), 2u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 2u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 2u);
}

/* Each accepted packet is counted as EITHER received OR redundant — never
 * both. Holds across redundancy, reorder, and frame-gone paths. */
TEST_F(St20RxStatsTest, ReceivedPlusRedundantInvariant) {
  /* frame 1: P delivers fully; R duplicates → all R pkts hit frame-gone */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_R);
  /* frame 2: clean P-only */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 2000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 2000, MTL_SESSION_PORT_P);

  /* total accepted into the session: 2 (P f1) + 2 (R f1 dup) + 2 (P f2) = 6 */
  EXPECT_EQ(received() + redundant(), 6u)
      << "every accepted packet must land in exactly one of {received, redundant}";
  EXPECT_EQ(frames_received(), 2);
}

/* lost_packets invariant: stat_lost_packets equals the sum of per-port
 * lost counters. Inducing real loss on each wire (a one-packet gap on
 * the primary and a one-packet gap on the secondary, each filled by the
 * other wire) makes the invariant non-trivial. Uses a 4-pkt frame so
 * intra-frame gaps are expressible. */
TEST_F(St20RxStatsTest, LostPacketsInvariant) {
  ut20_test_ctx* wide = ut20_ctx_create_geom(2, 4);
  ASSERT_NE(wide, nullptr);

  ut20_feed_frame_pkt(wide, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt(wide, 1, 1000, MTL_SESSION_PORT_R); /* R first; R_last=1 */
  ut20_feed_frame_pkt(wide, 3, 1000, MTL_SESSION_PORT_R); /* R skipped pkt 2 */
  ut20_feed_frame_pkt(wide, 2, 1000, MTL_SESSION_PORT_P); /* P jumped 0->2 */

  EXPECT_EQ(ut20_frames_received(wide), 1);
  EXPECT_EQ(ut20_stat_port_lost(wide, MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(ut20_stat_port_lost(wide, MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(ut20_stat_lost_pkts(wide), ut20_stat_port_lost(wide, MTL_SESSION_PORT_P) +
                                           ut20_stat_port_lost(wide, MTL_SESSION_PORT_R))
      << "stat_lost_packets must equal the sum of per-port lost_packets";

  ut20_ctx_destroy(wide);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wide-frame stat invariants.
 *
 * Per-port loss isolation and the unrecovered-packets counter cannot be
 * properly exercised with the default 2-pkt geometry because intra-frame
 * gaps collapse into the trivial first/last cases. These tests use a
 * 4-pkt geometry to express real intra-frame gaps on each wire.
 * ───────────────────────────────────────────────────────────────────────── */

class St20RxStatsWideTest : public ::testing::Test {
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
  uint64_t frames_incomplete() {
    return ut20_stat_frames_incomplete(ctx_);
  }
  uint64_t lost_session() {
    return ut20_stat_lost_pkts(ctx_);
  }
  uint64_t pkts_unrecovered() {
    return ut20_stat_pkts_unrecovered(ctx_);
  }
  uint64_t port_lost(enum mtl_session_port p) {
    return ut20_stat_port_lost(ctx_, p);
  }
};

/* P jumps from pkt 0 to pkt 3 in its own sequence, leaving a 2-pkt hole;
 * R fills the hole in order. The loss attributed to P equals the size of
 * P's own gap; R records no loss. */
TEST_F(St20RxStatsWideTest, PortLossCountsOwnGapsOnly) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P); /* P jumped 0 -> 3 on its wire */

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u)
      << "P's wire skipped pkts 1 and 2 between its own observations";
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 0u) << "R sent pkts 1 and 2 in order";
}

/* Both wires misbehave concurrently: R reorders its own packets while P
 * skips two indices. The two per-port counters must not contaminate each
 * other and reordering on one wire must not be classified as loss on
 * either wire. */
TEST_F(St20RxStatsWideTest, PerPortLossIsIndependent) {
  feed(0, 1000, MTL_SESSION_PORT_P); /* P_last = 0 */
  feed(2, 1000, MTL_SESSION_PORT_R); /* R first; R_last = 2 */
  feed(1, 1000, MTL_SESSION_PORT_R); /* R reorder of its own packet */
  feed(3, 1000, MTL_SESSION_PORT_P); /* P jumps 0 -> 3 */

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_R), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 2u);
}

/* Per-wire loss with full cross-wire recovery. P skips indices that R
 * supplies. stat_lost_packets reflects the wire-level loss, but
 * stat_pkts_unrecovered must stay zero because reconstruction succeeded.
 * Pins the documented invariant
 *     stat_pkts_unrecovered <= stat_lost_packets. */
TEST_F(St20RxStatsWideTest, UnrecoveredZeroWhenReconstructionSucceeds) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R); /* recovers P's gap */
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_GE(lost_session(), 1u) << "P's wire really skipped packets";
  EXPECT_EQ(pkts_unrecovered(), 0u)
      << "every gap on one wire was filled by the other wire";
  EXPECT_LE(pkts_unrecovered(), lost_session()) << "documented invariant";
}

/* Genuine post-redundancy loss: both wires drop the same packet, so the
 * missing index cannot be filled from either source. The frame must be
 * reported as incomplete and stat_pkts_unrecovered must record the
 * unrecoverable packet. Two further complete frames are fed so the
 * incomplete slot is reclaimed and finalised by the slot-window
 * manager. */
TEST_F(St20RxStatsWideTest, UnrecoveredWhenBothPortsMissSamePacket) {
  /* Neither wire delivers pkt 2 for ts = 1000. */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_P);
  feed(3, 1000, MTL_SESSION_PORT_R);
  /* Drive two more complete frames so the incomplete slot is reclaimed. */
  for (int i = 0; i < kPktsPerFrame; i++) feed(i, 2000, MTL_SESSION_PORT_P);
  for (int i = 0; i < kPktsPerFrame; i++) feed(i, 3000, MTL_SESSION_PORT_P);

  EXPECT_GE(frames_incomplete(), 1u) << "no port delivered pkt 2";
  EXPECT_GE(pkts_unrecovered(), 1u) << "the missing pkt is post-redundancy loss";
  EXPECT_LE(pkts_unrecovered(), lost_session())
      << "documented invariant: unrecovered <= lost";
}
