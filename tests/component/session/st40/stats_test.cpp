/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Per-port and session-wide stats invariants:
 * sequence-gap accounting (lost_packets), per-port packet/OOO/reorder/duplicate
 * counters and the global lost==sum(per-port-lost) invariant.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxStatsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxStatsTest : public St40RxBaseTest {};

/* Sequence gap produces exact unrecovered count (3 missing between 1 and 5). */
TEST_F(St40RxStatsTest, SeqGapExactCount) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(1, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 3u);
}

/* Large sequence gap: 99 packets missing between seq 0 and seq 100. */
TEST_F(St40RxStatsTest, SeqGapLargeCount) {
  feed(0, 1000, true, MTL_SESSION_PORT_P);
  feed(100, 2000, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 99u);
  EXPECT_EQ(received(), 2u);
}

/* Multiple sequence gaps produce correct cumulative unrecovered count. */
TEST_F(St40RxStatsTest, SeqGapMultipleHoles) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(3, 1001, false, MTL_SESSION_PORT_P);
  feed(6, 1002, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 4u); /* gap of 2 + gap of 2 */
}

/* Consecutive packets with no gap produce zero unrecovered. */
TEST_F(St40RxStatsTest, NoGapNoUnrecovered) {
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  EXPECT_EQ(unrecovered(), 0u);
}

/* Per-port packet counters track packets received on each port independently. */
TEST_F(St40RxStatsTest, PortPacketCount) {
  feed_burst(0, 5, 1000, false, MTL_SESSION_PORT_P);
  feed_burst(5, 3, 1001, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 3u);
}

/* Per-port OOO: gap on P only, R is sequential. OOO must be isolated. */
TEST_F(St40RxStatsTest, PortOOOPerPort) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P); /* gap of 4 on P */
  feed(6, 1002, false, MTL_SESSION_PORT_R);
  feed(7, 1003, false, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 4u);
  /* port R has no prior history → first pkt sets latest_seq_id, no gap */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 0u);
}

/* Backward sequence arrival on the same port must not inflate the per-port
 * OOO counter due to unsigned 16-bit wrapping. */
TEST_F(St40RxStatsTest, PerPortOOOBackwardSeq) {
  /* establish port P latest_seq_id = 5 */
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* now feed seq 3 (backward) with a new timestamp so it's not filtered as redundant */
  feed(3, 1002, false, MTL_SESSION_PORT_P);

  /* The backward seq should NOT add ~65533 phantom OOO events.
   * A correct implementation would add 0 or at most a small value. */
  EXPECT_LE(port_ooo(MTL_SESSION_PORT_P), ooo_before + 10u)
      << "Backward seq arrival should not add ~65533 phantom OOO";
}

/* Per stats_guide.md step 3: port[i].packets is pre-redundancy and includes
 * filtered duplicates. The redundant packet must also appear in stat_pkts_redundant. */
TEST_F(St40RxStatsTest, RedundantStillCountsPerPort) {
  /* 4 packets on port P accepted */
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  /* 4 duplicate packets on port R filtered as redundant */
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 4u);
  EXPECT_EQ(redundant(), 4u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 4u);
  /* R's per-port packets count what arrived on the wire (pre-redundancy). */
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 4u);
}

/* Duplicate seq_id on the same port must not inflate per-port OOO counter
 * due to unsigned 16-bit wrapping (gap should be 0, not 65535). */
TEST_F(St40RxStatsTest, DuplicateSeqPortOOOWrapping) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* re-feed seq 5 with same ts → filtered as redundant */
  feed(5, 1001, false, MTL_SESSION_PORT_P);

  /* A duplicate seq should NOT inflate OOO. The gap is logically 0,
   * but uint16_t wrapping makes it 65535. */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), ooo_before)
      << "Duplicate seq on same port should not inflate OOO counter";
}

/* Long accumulation: feed > 1000 packets with a regular gap pattern
 * to ensure the per-port OOO counter accumulates monotonically and
 * does not wrap.  Logically: the counter equals the number of
 * injected gap events.
 * Regression guard: per-port OOO counter must not wrap at uint16 boundary. */
TEST_F(St40RxStatsTest, PerPortOOOLargeAccumulation) {
  uint16_t seq = 0;
  uint32_t ts = 1000;
  int gap_events = 0;
  for (int i = 0; i < 1000; i++) {
    feed(seq, ts, false, MTL_SESSION_PORT_P);
    seq++;
    if (i % 5 == 4) {
      seq++; /* skip one -> 1-pkt forward gap */
      gap_events++;
    }
    if (i % 50 == 49) ts++;
  }
  /* Close the trailing forward gap created on the last iteration: forward
   * gaps are only counted when a later packet exposes them, so deliver one
   * more packet at the post-skip seq. */
  feed(seq, ts, false, MTL_SESSION_PORT_P);
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), (uint64_t)gap_events);
}

/* Invariant: stat_lost_packets == port[0].lost + port[1].lost at all times. */
TEST_F(St40RxStatsTest, LostPacketsInvariant) {
  constexpr uint32_t ts = 1000;
  /* Induce forward gap on each port, interleaved. */
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P); /* gap of 1 */
  feed(0, ts, false, MTL_SESSION_PORT_R);
  feed(3, ts, false, MTL_SESSION_PORT_R); /* gap of 2 */

  EXPECT_EQ(ooo(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R))
      << "stat_lost_packets must equal the sum of per-port lost_packets";
}

/* Gap calculation across the 16-bit RTP seq wrap boundary. The lib computes
 * the per-port gap as `(uint16_t)(seq - latest - 1)`, which must wrap modulo
 * 2^16. With latest=65534 and seq=1, the true forward gap is 2 (seq 65535
 * and 0 missed). A signed-promotion or naive subtraction bug would inflate
 * the counter to ~65535. Regression for the uint16-cast contract. */
TEST_F(St40RxStatsTest, PortLostNotInflatedAtSeqWrap) {
  constexpr uint32_t ts1 = 1000;
  constexpr uint32_t ts2 = 1001;

  /* establish latest_seq_id[P] = 65534 */
  feed(65533, ts1, false, MTL_SESSION_PORT_P);
  feed(65534, ts1, true, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);

  /* forward jump across the wrap: gap = 2 (seq 65535 and 0 missing) */
  feed(1, ts2, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P) - lost_before, 2u)
      << "per-port lost gap across seq wrap must be the true forward gap (2),"
         " not a phantom ~65535 from naive subtraction";
}

/* The `LostPacketsInvariant` (stat_lost == sum-of-per-port-lost) must hold
 * across a 16-bit seq wrap. Both ports cross the wrap with independent
 * gaps; verify the merged invariant survives the modular arithmetic. */
TEST_F(St40RxStatsTest, LostPacketsInvariantAcrossSeqWrap) {
  /* P: latest=65534, then 1 (gap 2) */
  feed(65533, 1000, false, MTL_SESSION_PORT_P);
  feed(65534, 1000, true, MTL_SESSION_PORT_P);
  feed(1, 1001, true, MTL_SESSION_PORT_P);

  /* R: latest=65530, then 0 (gap 5) */
  feed(65529, 1000, false, MTL_SESSION_PORT_R);
  feed(65530, 1000, true, MTL_SESSION_PORT_R);
  feed(0, 1001, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(ooo(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R))
      << "sum-of-per-port-lost invariant must hold across seq wrap";
}

/* The `_handle_pkt` path decrements `stat_pkts_unrecovered` when a late
 * arrival fills a previously-counted gap. The decrement is guarded by
 * `if (> 0)`. Drive a sequence where multiple late arrivals could try to
 * decrement the same logical gap; the counter must never underflow. */
TEST_F(St40RxStatsTest, UnrecoveredDoesNotUnderflow) {
  constexpr uint32_t ts = 1000;

  /* P delivers seq 0 then jumps to 5 (gap of 4 -> unrecovered += 4) */
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(5, ts, true, MTL_SESSION_PORT_P);
  uint64_t unrec_after_gap = unrecovered();
  ASSERT_GE(unrec_after_gap, 4u);

  /* R now delivers seq 1,2,3,4 — same frame, same ts. Each lands inside
   * anc_window_cur and decrements unrecovered. Then re-deliver them again
   * (now bitmap bits already set, so no further decrement should happen). */
  for (int i = 1; i <= 4; i++) feed(i, ts, false, MTL_SESSION_PORT_R);
  for (int i = 1; i <= 4; i++) feed(i, ts, false, MTL_SESSION_PORT_R);

  /* unrecovered may be 0 (gap healed) or >0 (some late arrivals classified
   * as redundant), but it must NEVER wrap to a huge number. */
  EXPECT_LE(unrecovered(), unrec_after_gap)
      << "stat_pkts_unrecovered must not underflow when late arrivals re-fire the "
         "decrement path";
}
