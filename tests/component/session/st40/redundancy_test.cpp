/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Core RX redundancy/dispatch:
 * normal redundancy, port switchover, threshold bypass, frame assembly,
 * sequence/timestamp wrap, hitless-merge loss patterns, intra-frame reorder.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxRedundancyTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxRedundancyTest : public St40RxBaseTest {};

/* Same packets sent on both ports, P first. Duplicates on R are filtered.
 * Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, NormalRedundancy) {
  constexpr uint32_t ts1 = 1000;
  constexpr uint32_t ts2 = 2500;

  /* frame 1: 6 packets, port 0 then port 1 */
  feed_burst(0, 6, ts1, true, MTL_SESSION_PORT_P);
  feed_burst(0, 6, ts1, true, MTL_SESSION_PORT_R);

  /* frame 2: 7 packets, port 0 then port 1 */
  feed_burst(6, 7, ts2, true, MTL_SESSION_PORT_P);
  feed_burst(6, 7, ts2, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 13u);
}

/* Clean burst switchover: P sends the first part of a frame, R sends
 * the remainder with the marker bit. P's queue drains first.
 * Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, BurstSwitchoverClean) {
  constexpr uint32_t ts_prev = 500;
  constexpr uint32_t ts_cur = 1000;

  /* establish history: one complete frame on port 0 */
  feed_burst(0, 6, ts_prev, true, MTL_SESSION_PORT_P);

  /* switchover frame: first 5 pkts from port 0, last 2 from port 1 */
  feed_burst(6, 5, ts_cur, false, MTL_SESSION_PORT_P);
  feed_burst(11, 2, ts_cur, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
}

/* Reordered burst switchover: R's newer packets are processed before P's
 * late arrivals for the same frame (same timestamp). The filter must accept
 * late packets from P that fill gaps rather than rejecting them as redundant.
 * Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, BurstSwitchoverReordered) {
  constexpr uint16_t N = 100;
  constexpr uint32_t ts_prev = 5000;
  constexpr uint32_t ts_cur = 6500;

  /* establish history: complete frame ending with seq N-1 */
  feed_burst(N - 6, 6, ts_prev, true, MTL_SESSION_PORT_P);

  ASSERT_EQ(session_seq(), N - 1);
  ASSERT_EQ(unrecovered(), 0u);

  /* Simulate reordering: R is processed first */

  /* R delivers seq N+2…N+5 while P's queue is empty */
  feed_burst(N + 2, 4, ts_cur, true, MTL_SESSION_PORT_R);

  /* P's late arrivals: seq N, N+1 */
  feed(N, ts_cur, false, MTL_SESSION_PORT_P);
  feed(N + 1, ts_cur, false, MTL_SESSION_PORT_P);

  /* All 6 packets belong to the same frame. Late arrivals from P
   * should be accepted despite R delivering its portion first. */
  EXPECT_EQ(unrecovered(), 0u)
      << "Late packets from port 0 should be accepted, not filtered as redundant.";
}

/* Multiple consecutive reordered switchovers. Each frame has its tail
 * processed from R before P's late head arrives.
 * Verifies cumulative unrecovered remains zero across all switchovers. */
TEST_F(St40RxRedundancyTest, MultipleSwitchoversReordered) {
  uint16_t seq = 0;
  uint32_t ts = 1000;
  constexpr int kFramePkts = 6;
  constexpr uint32_t kTsDelta = 1501;

  /* 10 normal frames on port 0 */
  for (int f = 0; f < 10; f++) {
    feed_burst(seq, kFramePkts, ts, true, MTL_SESSION_PORT_P);
    seq += kFramePkts;
    ts += kTsDelta;
  }
  ASSERT_EQ(unrecovered(), 0u);

  int switchovers = 0;
  constexpr int kLateCount = 2;

  /* 5 switchover frames: port 1 first, then port 0 late */
  for (int f = 0; f < 5; f++) {
    uint16_t frame_start = seq;
    feed_burst(frame_start + kLateCount, kFramePkts - kLateCount, ts, true,
               MTL_SESSION_PORT_R);
    feed_burst(frame_start, kLateCount, ts, false, MTL_SESSION_PORT_P);
    switchovers++;
    seq += kFramePkts;
    ts += kTsDelta;
  }

  EXPECT_EQ(unrecovered(), 0u)
      << "Late packets from port 0 should be accepted across all switchovers.";
}

/* Mid-frame port change without reordering: P sends the first half,
 * R sends the second half in sequence order.
 * Expects zero unrecovered and zero redundant. */
TEST_F(St40RxRedundancyTest, MidFrameSwitchoverNoReorder) {
  constexpr uint32_t ts = 1000;

  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);

  feed(3, ts, false, MTL_SESSION_PORT_R);
  feed(4, ts, false, MTL_SESSION_PORT_R);
  feed(5, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
}

/* Single-port baseline: only one port configured, no redundancy filtering.
 * All packets accepted, zero redundant. */
TEST_F(St40RxRedundancyTest, SinglePortBaseline) {
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1); /* re-create with 1 port */
  ASSERT_NE(ctx_, nullptr);

  feed_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);
  feed_burst(6, 7, 2500, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 13u);
}

/* Duplicate packet on the same port: same seq+ts resent.
 * The duplicate must be filtered as redundant. */
TEST_F(St40RxRedundancyTest, DuplicatePacketSamePort) {
  constexpr uint32_t ts = 1000;

  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  feed(3, ts, false, MTL_SESSION_PORT_P);
  feed(4, ts, false, MTL_SESSION_PORT_P);
  feed(5, ts, true, MTL_SESSION_PORT_P);

  /* duplicate: send seq 5 again with same ts on same port */
  feed(5, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(redundant(), 1u);
  EXPECT_EQ(received(), 6u);
}

/* Duplicate packet across ports: same seq+ts sent on P then R.
 * The cross-port duplicate must be filtered as redundant. */
TEST_F(St40RxRedundancyTest, DuplicatePacketCrossPort) {
  constexpr uint32_t ts = 1000;

  feed_burst(0, 6, ts, true, MTL_SESSION_PORT_P);

  /* port 1 sends the same last packet */
  feed(5, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(redundant(), 1u);
  EXPECT_EQ(unrecovered(), 0u);
}

/* Redundancy error threshold bypass: after 20+ consecutive redundant
 * rejections on ALL ports (ST_SESSION_REDUNDANT_ERROR_THRESHOLD = 20),
 * the filter force-accepts the next packet.
 * Also verifies the invariant: each packet must be counted as EITHER
 * received OR redundant, never both. */
TEST_F(St40RxRedundancyTest, ThresholdBypass) {
  constexpr uint32_t ts_new = 2000;
  constexpr uint32_t ts_old = 1000;

  /* establish state with newer timestamp */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);
  /* also feed port 1 to reset its error counter */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_R);

  /* now send 21 packets with old timestamp on BOTH ports to exceed threshold */
  for (int i = 0; i < 21; i++) {
    feed(50 + i, ts_old, false, MTL_SESSION_PORT_P);
    feed(50 + i, ts_old, false, MTL_SESSION_PORT_R);
  }

  /* the 21st pair should have been accepted (threshold = 20 per port) */
  uint64_t recv_after = received();
  EXPECT_GT(recv_after, 4u)
      << "After exceeding threshold on both ports, packets should be force-accepted";

  /* A bypass-accepted packet must be counted as EITHER received OR redundant.
   * 8 setup pkts + 42 old-ts pkts = 50 total valid pkts. */
  EXPECT_EQ(received() + redundant(), 50u)
      << "Each packet must be counted as EITHER received OR redundant, never both";
}

/* Partial threshold: only P exceeds the redundancy error threshold while
 * R remains below it. The filter must NOT bypass until ALL ports exceed
 * the threshold. Expects no extra packets accepted. */
TEST_F(St40RxRedundancyTest, ThresholdBypassPerPort) {
  constexpr uint32_t ts_new = 2000;
  constexpr uint32_t ts_old = 1000;

  /* establish state */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);
  /* feed port 1 — these 4 pkts are filtered as redundant (session_seq already 3),
   * so port R's redundant_error_cnt reaches 4, which is still below threshold=20. */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_R);

  /* send 25 old-ts packets on port 0 ONLY */
  for (int i = 0; i < 25; i++) {
    int rc = feed(50 + i, ts_old, false, MTL_SESSION_PORT_P);
    /* all should be rejected: port R counter is 4, still below threshold */
    EXPECT_LT(rc, 0)
        << "Should not bypass threshold when port R only has 4 errors (< 20)";
  }

  /* nothing extra should have been received */
  EXPECT_EQ(received(), 4u);
}

/* Interleaved ports: P sends even seqs, R sends odd seqs, all with the
 * same timestamp. No packet reordering, just alternating sources.
 * Expects zero unrecovered, zero redundant, and correct per-port OOO
 * counts reflecting the per-port sequence gaps. */
TEST_F(St40RxRedundancyTest, InterleavedPorts) {
  constexpr uint32_t ts = 1000;

  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_R);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  feed(3, ts, false, MTL_SESSION_PORT_R);
  feed(4, ts, false, MTL_SESSION_PORT_P);
  feed(5, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 6u);

  /* Per-port OOO for interleaved ports: P sees seqs 0,2,4 (two gaps of 1),
   * R sees seqs 1,3,5 (two gaps of 1). Each port should have 2 OOO events. */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 2u) << "Port P sees seq 0,2,4 — two gaps of 1";
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 2u) << "Port R sees seq 1,3,5 — two gaps of 1";
}

/* Reordered switchover with a larger gap: R delivers 5 packets before
 * P's 3 late arrivals for the same frame. Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, BurstSwitchoverLargeGap) {
  constexpr uint16_t N = 50;
  constexpr uint32_t ts_prev = 3000;
  constexpr uint32_t ts_cur = 4500;

  /* history frame */
  feed_burst(N - 4, 4, ts_prev, true, MTL_SESSION_PORT_P);
  ASSERT_EQ(session_seq(), N - 1);

  /* port 1 processed first: seq N+3..N+7 (5 pkts) */
  feed_burst(N + 3, 5, ts_cur, true, MTL_SESSION_PORT_R);
  /* port 0 late: seq N, N+1, N+2 */
  feed(N, ts_cur, false, MTL_SESSION_PORT_P);
  feed(N + 1, ts_cur, false, MTL_SESSION_PORT_P);
  feed(N + 2, ts_cur, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u)
      << "3 late packets from port 0 should be accepted, not lost.";
}

/* Full gap then late arrival: R has only the tail of a new frame,
 * P's entire contribution arrives late. The late packets must be
 * accepted. Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, FullGapThenLateArrival) {
  constexpr uint16_t N = 200;
  constexpr uint32_t ts_prev = 7000;
  constexpr uint32_t ts_cur = 8500;

  feed_burst(N - 6, 6, ts_prev, true, MTL_SESSION_PORT_P);

  /* port 1: seq N+3..N+5 (marker on N+5) — processed first */
  feed_burst(N + 3, 3, ts_cur, true, MTL_SESSION_PORT_R);

  /* port 0: seq N..N+2 — arrives late */
  feed_burst(N, 3, ts_cur, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u)
      << "First half of frame from port 0 arriving late should not be lost.";
}

/* Stale replay after switchover: after a clean port switch, R replays
 * packets from an earlier frame. All replayed packets should be filtered
 * as redundant. */
TEST_F(St40RxRedundancyTest, RedundantAfterSwitchover) {
  constexpr uint32_t ts1 = 1000;
  constexpr uint32_t ts2 = 2500;

  /* frame 1 from port 0 */
  feed_burst(0, 6, ts1, true, MTL_SESSION_PORT_P);

  /* frame 2: clean switchover, port 0 first half, port 1 second half */
  feed_burst(6, 3, ts2, false, MTL_SESSION_PORT_P);
  feed_burst(9, 3, ts2, true, MTL_SESSION_PORT_R);

  /* port 1 now replays frame 1's packets (stale) */
  feed_burst(0, 6, ts1, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 6u) << "All 6 stale frame-1 packets should be redundant";
}

/* True correlated loss: same seq missing on BOTH ports.  This is the
 * only condition that should produce unrecovered > 0 in a redundant
 * session. */
TEST_F(St40RxRedundancyTest, CorrelatedLossBothPorts) {
  /* Frame N: deliver seq 0,1,3,4 on both ports — seq 2 is lost on both. */
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(1, 1000, false, MTL_SESSION_PORT_P);
  /* gap: seq 2 missing on P */
  feed(3, 1000, false, MTL_SESSION_PORT_P);
  feed(4, 1000, true, MTL_SESSION_PORT_P);

  feed(0, 1000, false, MTL_SESSION_PORT_R);
  feed(1, 1000, false, MTL_SESSION_PORT_R);
  /* gap: seq 2 missing on R as well */
  feed(3, 1000, false, MTL_SESSION_PORT_R);
  feed(4, 1000, true, MTL_SESSION_PORT_R);

  /* Per-port gap counted on each port. */
  EXPECT_GE(port_ooo(MTL_SESSION_PORT_P), 1u);
  EXPECT_GE(port_ooo(MTL_SESSION_PORT_R), 1u);
  /* And — the meaningful number — true unrecovered loss. */
  EXPECT_GE(unrecovered(), 1u) << "seq lost on both ports must produce unrecovered > 0";
}

/* Healthy redundancy: each port loses different packets, the merged
 * stream is intact.  Asserts that the per-port loss counter rises
 * while the merged stream sees zero unrecovered loss. */
TEST_F(St40RxRedundancyTest, UncorrelatedLossOnePortAtATime) {
  /* P loses even seqs (2,4,6,8); R loses odd seqs (1,3,5,7).
   * Together every seq 0..9 is delivered exactly once. */
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(1, 1000, false, MTL_SESSION_PORT_P);
  feed(3, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1000, false, MTL_SESSION_PORT_P);
  feed(7, 1000, false, MTL_SESSION_PORT_P);
  feed(9, 1000, true, MTL_SESSION_PORT_P);

  feed(0, 1000, false, MTL_SESSION_PORT_R);
  feed(2, 1000, false, MTL_SESSION_PORT_R);
  feed(4, 1000, false, MTL_SESSION_PORT_R);
  feed(6, 1000, false, MTL_SESSION_PORT_R);
  feed(8, 1000, false, MTL_SESSION_PORT_R);

  /* Both ports report per-port loss (gap counter rises). */
  EXPECT_GT(port_ooo(MTL_SESSION_PORT_P), 0u);
  EXPECT_GT(port_ooo(MTL_SESSION_PORT_R), 0u);
  /* But redundancy covers everything -> no unrecovered loss. */
  EXPECT_EQ(unrecovered(), 0u)
      << "per-port loss covered by the other port must not show as unrecovered";
}

/* Deterministic high per-port loss with full redundancy: ~25% per-port
 * loss, complementary, so the merged stream is complete.  Every
 * per-port drop must be counted as a per-port gap exactly once, and
 * the merged stream must have zero unrecovered loss. */
TEST_F(St40RxRedundancyTest, HighPerLegLossRedundancySaves) {
  constexpr uint32_t ts = 1000;
  constexpr int kPkts = 100;
  /* P drops every 4th seq (i%4==3); R supplies those interleaved so the
   * rescue lands inside the bitmap / prev_tmstamp window. */
  int p_drops = 0;
  for (int i = 0; i < kPkts; i++) {
    if (i % 4 == 3) {
      p_drops++;
      feed(i, ts, false, MTL_SESSION_PORT_R); /* R covers immediately */
      continue;
    }
    feed(i, ts, false, MTL_SESSION_PORT_P);
  }
  /* Close the trailing per-port gap on P (i==99 was a hole that has no
   * follow-up P packet to expose it).  Forward gaps are observations —
   * a hole is only counted when a later packet on the same port arrives
   * past it, so we deliver one more packet at seq=kPkts. */
  feed(kPkts, ts, true, MTL_SESSION_PORT_P);
  /* Each per-port drop must show as a per-port gap on P. */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), (uint64_t)p_drops);
  /* The merged stream must be intact — every seq delivered once. */
  EXPECT_EQ(unrecovered(), 0u) << "complementary per-port loss must be fully recovered";
}
