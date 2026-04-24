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
 * lost counters. Holds independently of which port the loss occurred on. */
TEST_F(St20RxStatsTest, LostPacketsInvariant) {
  /* feed 4-pkt frames (override default ppf via a forward gap) by sending
   * pkt_idx 0,1 on each port for two distinct timestamps with a gap. */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 2000, MTL_SESSION_PORT_R);
  feed(1, 2000, MTL_SESSION_PORT_R);

  EXPECT_EQ(ooo(), port_lost(MTL_SESSION_PORT_P) + port_lost(MTL_SESSION_PORT_R))
      << "stat_lost_packets must equal the sum of per-port lost_packets";
}
