/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Per-port and session-wide stats invariants:
 * per-port packet/OOO/reorder/duplicate counters, frame completion, and
 * the global lost == sum(per-port-lost) invariant.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxStatsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxStatsTest : public St30RxBaseTest {};

/* Per-port packet counters track packets received on each port independently. */
TEST_F(St30RxStatsTest, PortPacketCount) {
  /* feed 5 pkts on P with increasing ts */
  for (int i = 0; i < 5; i++) feed(i, 1000 + i, MTL_SESSION_PORT_P);
  /* feed 3 pkts on R with further increasing ts */
  for (int i = 0; i < 3; i++) feed(5 + i, 2000 + i, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 3u);
}

/* Per-port OOO: gap on P only, R is sequential. OOO must be isolated. */
TEST_F(St30RxStatsTest, PortOOOPerPort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P); /* gap of 4 */
  feed(6, 2000, MTL_SESSION_PORT_R);
  feed(7, 2001, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 4u);
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 0u);
}

/* Three complete frames: 3×pkts_per_frame packets produce 3 frames. */
TEST_F(St30RxStatsTest, MultipleFrames) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < total; i++) {
      feed(seq++, ts++, MTL_SESSION_PORT_P);
    }
  }

  EXPECT_EQ(frames_done(), 3);
}

/* Sequence gap produces exact unrecovered count with per-packet timestamps. */
TEST_F(St30RxStatsTest, SeqGapExactCount) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P); /* gap of 3 in session_seq */

  EXPECT_EQ(unrecovered(), 3u);
}

/* Backward sequence arrival on the same port must not inflate the per-port
 * OOO counter due to unsigned 16-bit wrapping. */
TEST_F(St30RxStatsTest, PerPortOOOBackwardSeq) {
  /* establish port P latest_seq_id = 5 */
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* feed seq 3 (backward) with newer timestamp so it passes redundancy */
  feed(3, 2000, MTL_SESSION_PORT_P);

  /* Backward seq should not add ~65533 phantom OOO events */
  EXPECT_LE(port_ooo(MTL_SESSION_PORT_P), ooo_before + 10u)
      << "Backward seq arrival should not produce phantom OOO";
}

/* Duplicate seq_id on the same port must not inflate per-port OOO counter
 * due to unsigned 16-bit wrapping (gap should be 0, not 65535). */
TEST_F(St30RxStatsTest, DuplicateSeqPortOOOWrapping) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* re-feed seq 5 with same ts — filtered as redundant */
  feed(5, 1005, MTL_SESSION_PORT_P);

  /* Duplicate seq should not inflate OOO counter */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), ooo_before)
      << "Duplicate seq on same port should not inflate OOO counter";
}

/* stat_lost_packets == port[0].lost + port[1].lost invariant. */
TEST_F(St30RxStatsTest, LostPacketsInvariant) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1001, MTL_SESSION_PORT_P);
  feed(0, 1002, MTL_SESSION_PORT_R);
  feed(3, 1003, MTL_SESSION_PORT_R);

  EXPECT_EQ(ooo(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R));
}

/* Gap across the 16-bit RTP seq wrap boundary. With latest=65534 and seq=1,
 * the true forward gap is 2 (seq 65535 and 0 missed). Both per-port OOO and
 * session-level unrecovered must reflect the true gap, not ~65535. */
TEST_F(St30RxStatsTest, PortLostNotInflatedAtSeqWrap) {
  /* establish latest_seq_id[P] = 65534 */
  feed(65533, 1000, MTL_SESSION_PORT_P);
  feed(65534, 1001, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);
  uint64_t unrec_before = unrecovered();

  /* forward jump across the wrap: gap = 2 (seq 65535 and 0 missing) */
  feed(1, 1002, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P) - lost_before, 2u)
      << "per-port lost gap across seq wrap must be the true forward gap (2),"
         " not a phantom ~65535 from naive subtraction";
  EXPECT_EQ(unrecovered() - unrec_before, 2u)
      << "session-seq gap across wrap must equal the true forward gap (2)";
}
