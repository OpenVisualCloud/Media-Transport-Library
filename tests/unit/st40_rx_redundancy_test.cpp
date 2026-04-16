/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Unit tests for ST2110-40 (ancillary) RX redundancy filter.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest (no NIC ports or sudo required)
 */

#include <gtest/gtest.h>

#include "st40_rx_redundancy_harness.h"

/* ── fixture ───────────────────────────────────────────────────────── */

class St40RxRedundancyTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut_eal_init(), 0) << "EAL init failed";
    ut_drain_ring();
    ctx_ = ut_ctx_create(2); /* 2 ports = redundant */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut_drain_ring();
    ut_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  /* convenience wrappers */
  int feed(uint16_t seq, uint32_t ts, bool marker, enum mtl_session_port port) {
    return ut_feed_pkt(ctx_, seq, ts, marker ? 1 : 0, port);
  }

  void feed_burst(uint16_t seq_start, int count, uint32_t ts, bool last_marker,
                  enum mtl_session_port port) {
    ut_feed_burst(ctx_, seq_start, count, ts, last_marker ? 1 : 0, port);
  }

  uint64_t unrecovered() {
    return ut_stat_unrecovered(ctx_);
  }
  uint64_t redundant() {
    return ut_stat_redundant(ctx_);
  }
  uint64_t received() {
    return ut_stat_received(ctx_);
  }
  uint64_t ooo() {
    return ut_stat_out_of_order(ctx_);
  }
  int session_seq() {
    return ut_session_seq_id(ctx_);
  }

  uint64_t port_pkts(enum mtl_session_port p) {
    return ut_stat_port_pkts(ctx_, p);
  }
  uint64_t port_bytes(enum mtl_session_port p) {
    return ut_stat_port_bytes(ctx_, p);
  }
  uint64_t port_ooo(enum mtl_session_port p) {
    return ut_stat_port_ooo(ctx_, p);
  }
  uint64_t port_frames(enum mtl_session_port p) {
    return ut_stat_port_frames(ctx_, p);
  }
  uint64_t wrong_pt() {
    return ut_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut_stat_wrong_ssrc(ctx_);
  }
  uint64_t wrong_interlace() {
    return ut_stat_wrong_interlace(ctx_);
  }
  uint64_t interlace_first() {
    return ut_stat_interlace_first(ctx_);
  }
  uint64_t interlace_second() {
    return ut_stat_interlace_second(ctx_);
  }
  uint64_t enqueue_fail() {
    return ut_stat_enqueue_fail(ctx_);
  }
  int frames_received() {
    return ut_frames_received(ctx_);
  }
};

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
TEST_F(St40RxRedundancyTest, BurstSwitchoverReorderedBug) {
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
  ut_ctx_destroy(ctx_);
  ctx_ = ut_ctx_create(1); /* re-create with 1 port */
  ASSERT_NE(ctx_, nullptr);

  feed_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);
  feed_burst(6, 7, 2500, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 13u);
}

/* 16-bit sequence number wraparound (65533 → 65535 → 0 → 1).
 * All packets in order on a single port. Expects zero unrecovered. */
TEST_F(St40RxRedundancyTest, SeqWrapAround) {
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
TEST_F(St40RxRedundancyTest, SeqWrapAroundRedundant) {
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

/* Large sequence gap: 99 packets missing between seq 0 and seq 100.
 * Expects unrecovered == 99. */
TEST_F(St40RxRedundancyTest, LargeSeqGap) {
  constexpr uint32_t ts1 = 1000;
  constexpr uint32_t ts2 = 2000;

  feed(0, ts1, true, MTL_SESSION_PORT_P);
  feed(100, ts2, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 99u);
  EXPECT_EQ(received(), 2u);
}

/* 32-bit timestamp wraparound from near UINT32_MAX to near zero.
 * Expects all packets received with zero unrecovered. */
TEST_F(St40RxRedundancyTest, TimestampWrapAround) {
  constexpr uint32_t ts1 = 0xFFFFFFF0;
  constexpr uint32_t ts2 = 0x00000010; /* wraps around */

  feed_burst(0, 4, ts1, true, MTL_SESSION_PORT_P);
  feed_burst(4, 4, ts2, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(received(), 8u);
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

/* Backward timestamp with advancing seq: a newer-ts frame is followed by
 * a packet with an older timestamp but higher seq_id.
 * The filter must reject based on the stale timestamp. */
TEST_F(St40RxRedundancyTest, OldTimestampNewSeq) {
  constexpr uint32_t ts_new = 2000;
  constexpr uint32_t ts_old = 1000;

  /* first frame with newer timestamp */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);

  /* second "frame" has older timestamp but advancing seq — should be filtered */
  int rc = feed(4, ts_old, true, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Packet with old timestamp should be rejected";
  EXPECT_EQ(redundant(), 1u);
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

/* Back-to-back frames on a single port: 10 frames × 4 packets each.
 * No gaps, no redundancy. Expects exact received count of 40. */
TEST_F(St40RxRedundancyTest, BackToBackFrames) {
  ut_ctx_destroy(ctx_);
  ctx_ = ut_ctx_create(1);
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

/* ── Return values, validation filters, per-port statistics ────────── */

/* Accepted packet must return 0. */
TEST_F(St40RxRedundancyTest, ReturnValueAccepted) {
  int rc = feed(0, 1000, false, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St40RxRedundancyTest, ReturnValueWrongPT) {
  ut_ctx_set_pt(ctx_, 96);
  int rc = ut_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St40RxRedundancyTest, ReturnValueWrongSSRC) {
  ut_ctx_set_ssrc(ctx_, 1234);
  int rc = ut_feed_pkt_ssrc(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Invalid F-bits (0b01) must return -EINVAL. */
TEST_F(St40RxRedundancyTest, ReturnValueInvalidFBits) {
  int rc = ut_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
  EXPECT_EQ(rc, -EINVAL);
}

/* Redundant (old-timestamp) packet must return -EIO. */
TEST_F(St40RxRedundancyTest, ReturnValueRedundant) {
  feed(0, 2000, true, MTL_SESSION_PORT_P);
  int rc = feed(0, 1000, false, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, -EIO);
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St40RxRedundancyTest, WrongPayloadTypeDropped) {
  ut_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++)
    ut_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Correct PT packets are accepted with zero wrong_pt. */
TEST_F(St40RxRedundancyTest, CorrectPayloadTypeAccepted) {
  ut_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 4; i++)
    ut_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 96);

  EXPECT_EQ(wrong_pt(), 0u);
  EXPECT_EQ(received(), 4u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St40RxRedundancyTest, WrongSSRCDropped) {
  ut_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut_feed_pkt_ssrc(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* PT=0 disables the payload type check: any PT value is accepted. */
TEST_F(St40RxRedundancyTest, ZeroPTDisablesCheck) {
  /* PT=0 by default */
  ut_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 99);
  ut_feed_pkt_pt(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 111);

  EXPECT_EQ(wrong_pt(), 0u);
  EXPECT_EQ(received(), 2u);
}

/* Sequence gap produces exact unrecovered count (3 missing between 1 and 5). */
TEST_F(St40RxRedundancyTest, SeqGapExactCount) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(1, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 3u);
}

/* Multiple sequence gaps produce correct cumulative unrecovered count. */
TEST_F(St40RxRedundancyTest, SeqGapMultipleHoles) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(3, 1001, false, MTL_SESSION_PORT_P);
  feed(6, 1002, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 4u); /* gap of 2 + gap of 2 */
}

/* Consecutive packets with no gap produce zero unrecovered. */
TEST_F(St40RxRedundancyTest, NoGapNoUnrecovered) {
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  EXPECT_EQ(unrecovered(), 0u);
}

/* Per-port packet counters track packets received on each port independently. */
TEST_F(St40RxRedundancyTest, PortPacketCount) {
  feed_burst(0, 5, 1000, false, MTL_SESSION_PORT_P);
  feed_burst(5, 3, 1001, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 3u);
}

/* Per-port OOO: gap on P only, R is sequential. OOO must be isolated. */
TEST_F(St40RxRedundancyTest, PortOOOPerPort) {
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(5, 1001, false, MTL_SESSION_PORT_P); /* gap of 4 on P */
  feed(6, 1002, false, MTL_SESSION_PORT_R);
  feed(7, 1003, false, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 4u);
  /* port R has no prior history → first pkt sets latest_seq_id, no gap */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 0u);
}

/* Invalid F-bits packets are dropped and counted in wrong_interlace stat. */
TEST_F(St40RxRedundancyTest, InvalidFBitsDropped) {
  ut_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
  ut_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x1);

  EXPECT_EQ(wrong_interlace(), 2u);
  EXPECT_EQ(received(), 0u);
}

/* Progressive F-bits (0b00) are accepted without incrementing interlace counters. */
TEST_F(St40RxRedundancyTest, ProgressiveFBits) {
  ut_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x0);
  ut_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x0);

  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(interlace_first(), 0u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_EQ(received(), 2u);
}

/* Interlaced fields: F=0b10 (first) and F=0b11 (second) are accepted
 * and tracked in their respective interlace counters. */
TEST_F(St40RxRedundancyTest, InterlaceFieldCounting) {
  /* F=0x2 → first field */
  ut_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x2);
  /* F=0x3 → second field */
  ut_feed_pkt_fbits(ctx_, 2, 1002, 0, MTL_SESSION_PORT_P, 0x3);

  EXPECT_EQ(interlace_first(), 2u);
  EXPECT_EQ(interlace_second(), 1u);
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(received(), 3u);
}

/* Backward sequence arrival on the same port must not inflate the per-port
 * OOO counter due to unsigned 16-bit wrapping. */
TEST_F(St40RxRedundancyTest, PerPortOOOBackwardSeq) {
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

/* Redundant packets must still be counted in per-port stats because
 * per-port counters are incremented before redundancy filtering. */
TEST_F(St40RxRedundancyTest, RedundantStillCountsPerPort) {
  /* 4 packets on port P accepted */
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  /* 4 duplicate packets on port R filtered as redundant */
  feed_burst(0, 4, 1000, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 4u);
  EXPECT_EQ(redundant(), 4u);
  /* per-port counts include redundant packets */
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 4u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 4u);
}

/* Duplicate seq_id on the same port must not inflate per-port OOO counter
 * due to unsigned 16-bit wrapping (gap should be 0, not 65535). */
TEST_F(St40RxRedundancyTest, DuplicateSeqPortOOOWrapping) {
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

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
