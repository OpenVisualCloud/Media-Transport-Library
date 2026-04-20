/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Unit tests for ST2110-40 (ancillary) RX redundancy filter.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest
 */

#include <gtest/gtest.h>

#include "session/st40_harness.h"

/* ── fixture ───────────────────────────────────────────────────────── */

class St40RxRedundancyTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0) << "EAL init failed";
    ut40_drain_ring();
    ctx_ = ut40_ctx_create(2); /* 2 ports = redundant */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40_drain_ring();
    ut40_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  /* convenience wrappers */
  int feed(uint16_t seq, uint32_t ts, bool marker, enum mtl_session_port port) {
    return ut40_feed_pkt(ctx_, seq, ts, marker ? 1 : 0, port);
  }

  void feed_burst(uint16_t seq_start, int count, uint32_t ts, bool last_marker,
                  enum mtl_session_port port) {
    ut40_feed_burst(ctx_, seq_start, count, ts, last_marker ? 1 : 0, port);
  }

  uint64_t unrecovered() {
    return ut40_stat_unrecovered(ctx_);
  }
  uint64_t redundant() {
    return ut40_stat_redundant(ctx_);
  }
  uint64_t received() {
    return ut40_stat_received(ctx_);
  }
  uint64_t ooo() {
    return ut40_stat_lost_pkts(ctx_);
  }
  int session_seq() {
    return ut40_session_seq_id(ctx_);
  }

  uint64_t port_pkts(enum mtl_session_port p) {
    return ut40_stat_port_pkts(ctx_, p);
  }
  uint64_t port_bytes(enum mtl_session_port p) {
    return ut40_stat_port_bytes(ctx_, p);
  }
  uint64_t port_ooo(enum mtl_session_port p) {
    return ut40_stat_port_lost(ctx_, p);
  }
  uint64_t port_frames(enum mtl_session_port p) {
    return ut40_stat_port_frames(ctx_, p);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut40_stat_port_reordered(ctx_, p);
  }
  uint64_t port_duplicates(enum mtl_session_port p) {
    return ut40_stat_port_duplicates(ctx_, p);
  }
  uint64_t field_bit_mismatch() {
    return ut40_stat_field_bit_mismatch(ctx_);
  }
  uint64_t wrong_pt() {
    return ut40_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut40_stat_wrong_ssrc(ctx_);
  }
  uint64_t wrong_interlace() {
    return ut40_stat_wrong_interlace(ctx_);
  }
  uint64_t interlace_first() {
    return ut40_stat_interlace_first(ctx_);
  }
  uint64_t interlace_second() {
    return ut40_stat_interlace_second(ctx_);
  }
  uint64_t enqueue_fail() {
    return ut40_stat_enqueue_fail(ctx_);
  }
  int frames_received() {
    return ut40_frames_received(ctx_);
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
  ut40_ctx_destroy(ctx_);
  ctx_ = ut40_ctx_create(1);
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
  ut40_ctx_set_pt(ctx_, 96);
  int rc = ut40_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St40RxRedundancyTest, ReturnValueWrongSSRC) {
  ut40_ctx_set_ssrc(ctx_, 1234);
  int rc = ut40_feed_pkt_ssrc(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Invalid F-bits (0b01) must return -EINVAL. */
TEST_F(St40RxRedundancyTest, ReturnValueInvalidFBits) {
  int rc = ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
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
  ut40_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++)
    ut40_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Correct PT packets are accepted with zero wrong_pt. */
TEST_F(St40RxRedundancyTest, CorrectPayloadTypeAccepted) {
  ut40_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 4; i++)
    ut40_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 96);

  EXPECT_EQ(wrong_pt(), 0u);
  EXPECT_EQ(received(), 4u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St40RxRedundancyTest, WrongSSRCDropped) {
  ut40_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut40_feed_pkt_ssrc(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* PT=0 disables the payload type check: any PT value is accepted. */
TEST_F(St40RxRedundancyTest, ZeroPTDisablesCheck) {
  /* PT=0 by default */
  ut40_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 99);
  ut40_feed_pkt_pt(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 111);

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
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x1);

  EXPECT_EQ(wrong_interlace(), 2u);
  EXPECT_EQ(received(), 0u);
}

/* Progressive F-bits (0b00) are accepted without incrementing interlace counters. */
TEST_F(St40RxRedundancyTest, ProgressiveFBits) {
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x0);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x0);

  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(interlace_first(), 0u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_EQ(received(), 2u);
}

/* Interlaced fields: F=0b10 (first) and F=0b11 (second) are accepted
 * and tracked in their respective interlace counters. */
TEST_F(St40RxRedundancyTest, InterlaceFieldCounting) {
  /* F=0x2 → first field */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x2);
  /* F=0x3 → second field */
  ut40_feed_pkt_fbits(ctx_, 2, 1002, 0, MTL_SESSION_PORT_P, 0x3);

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

/* ── RTP marker bit preservation ──────────────────────────────────── */

/* Marker preservation when P advances timestamp before R delivers marker.
 *
 * Scenario:
 *   1. P delivers frame N body: seq 0-4, ts=1000, no marker
 *   2. P delivers frame N+1 first packet: seq 7, ts=2000 — tmstamp advances
 *   3. R delivers frame N tail: seq 5-6, ts=1000, marker on seq 6
 *
 * The redundancy filter must accept R's late packets for the immediately
 * previous timestamp and preserve the marker bit through to the ring. */
TEST_F(St40RxRedundancyTest, MarkerPreservedAfterTimestampAdvance) {
  ut40_set_skip_drain(true);
  uint32_t ts_frame_n = 1000;
  uint32_t ts_frame_n1 = 2000;

  /* P delivers frame N body: seq 0-4, no marker */
  for (int i = 0; i < 5; i++) feed(i, ts_frame_n, false, MTL_SESSION_PORT_P);

  /* P starts frame N+1 before R delivers frame N's marker */
  feed(7, ts_frame_n1, false, MTL_SESSION_PORT_P);

  /* R delivers frame N's tail: seq 5 (no marker), seq 6 (MARKER) */
  feed(5, ts_frame_n, false, MTL_SESSION_PORT_R);
  feed(6, ts_frame_n, true, MTL_SESSION_PORT_R); /* MARKER */

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);

  EXPECT_TRUE(has_marker)
      << "Marker from R must survive when P advances to next frame first";
  ut40_set_skip_drain(false);
}

/* Marker bit on the last packet of a single-port burst survives into the ring. */
TEST_F(St40RxRedundancyTest, MarkerPreservedSinglePort) {
  ut40_set_skip_drain(true);
  /* 6 packets, marker on last */
  feed_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_TRUE(has_marker) << "Marker bit must survive into the ring";
  ut40_set_skip_drain(false);
}

/* Mid-frame port switchover: P sends the body, R sends the tail including marker.
 * Reproduces the pcap scenario where P sends seq 0-4, R sends seq 5-6 with
 * marker on seq 6.  The marker-bearing packet from R must pass the redundancy
 * filter and be enqueued with the marker bit intact. */
TEST_F(St40RxRedundancyTest, MarkerPreservedMidFrameSwitchover) {
  ut40_set_skip_drain(true);
  uint32_t ts = 2000;
  /* P sends seq 0-4, no marker, same timestamp */
  for (int i = 0; i < 5; i++) feed(i, ts, false, MTL_SESSION_PORT_P);
  /* R sends seq 5-6, marker on seq 6, same timestamp */
  feed(5, ts, false, MTL_SESSION_PORT_R);
  feed(6, ts, true, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 7);
  EXPECT_TRUE(has_marker) << "Marker from R port must survive mid-frame switchover";
  ut40_set_skip_drain(false);
}

/* Cross-port reorder with bitmap: R delivers seq 7-11 (marker on 11) before
 * P's late-arriving seq 5-6.  A warm-up frame establishes session_seq so the
 * bitmap base covers the late arrivals.  The marker from R must survive. */
TEST_F(St40RxRedundancyTest, MarkerPreservedCrossPortReorder) {
  /* Previous frame to set session_seq_id=4, so bitmap base for next frame = 5 */
  feed_burst(0, 5, 2999, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_set_skip_drain(true);

  uint32_t ts = 3000;
  /* R sends seq 7-10 (no marker), then seq 11 (marker) */
  for (int i = 7; i <= 10; i++) feed(i, ts, false, MTL_SESSION_PORT_R);
  feed(11, ts, true, MTL_SESSION_PORT_R);
  /* P's late arrivals: seq 5-6, same timestamp, no marker */
  feed(5, ts, false, MTL_SESSION_PORT_P);
  feed(6, ts, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 7);
  EXPECT_TRUE(has_marker) << "Marker must survive cross-port reorder";
  ut40_set_skip_drain(false);
}

/* Negative test: no marker set on any packet, verify has_marker is false.
 * Guards against false positives in the test infrastructure. */
TEST_F(St40RxRedundancyTest, MarkerAbsentWhenNotSet) {
  ut40_set_skip_drain(true);
  /* 6 packets, NO marker on any */
  feed_burst(0, 6, 1000, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_FALSE(has_marker) << "No packet had marker=1, has_marker must be false";
  ut40_set_skip_drain(false);
}

/* Marker on the first packet (not last) of a frame.  Verifies position-
 * independence: the handler must never strip the marker regardless of
 * where it appears in the sequence. */
TEST_F(St40RxRedundancyTest, MarkerOnFirstPacket) {
  ut40_set_skip_drain(true);
  uint32_t ts = 4000;
  /* First packet carries the marker */
  feed(0, ts, true, MTL_SESSION_PORT_P);
  for (int i = 1; i < 6; i++) feed(i, ts, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_TRUE(has_marker) << "Marker on first packet must survive into ring";
  ut40_set_skip_drain(false);
}

/* Late arrival via bitmap carries the marker.  R advances the session seq,
 * then P's late packet (carrying the marker) is accepted through the bitmap
 * path (goto accept_pkt).  The marker must survive this alternate code path. */
TEST_F(St40RxRedundancyTest, MarkerOnBitmapLateArrival) {
  /* Warm-up frame to establish session_seq = 4 */
  feed_burst(0, 5, 4999, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_set_skip_drain(true);

  uint32_t ts = 5000;
  /* R sends seq 7-10, no marker */
  for (int i = 7; i <= 10; i++) feed(i, ts, false, MTL_SESSION_PORT_R);
  /* P's late arrivals: seq 5 (no marker), seq 6 (MARKER) */
  feed(5, ts, false, MTL_SESSION_PORT_P);
  feed(6, ts, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_TRUE(has_marker) << "Marker on bitmap-accepted late arrival must survive";
  ut40_set_skip_drain(false);
}

/* Pcap-accurate scenario: a complete prior frame establishes session history,
 * then P sends the body of the next frame and R sends the tail with marker.
 * This closely mirrors the actual pcap (frame ts=0xef6fef62, P sends 5 pkts,
 * R sends 2 pkts with marker on the last). */
TEST_F(St40RxRedundancyTest, MarkerPcapMidFrameSwitchoverWithHistory) {
  /* Complete prior frame on P (7 packets) to establish session history */
  feed_burst(0, 7, 1000, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_set_skip_drain(true);

  uint32_t ts = 2000;
  /* P sends body: seq 7-11, no marker, same ts (like pcap's seq 58363-58367) */
  for (int i = 7; i <= 11; i++) feed(i, ts, false, MTL_SESSION_PORT_P);
  /* R sends tail: seq 12-13, marker on seq 13 (like pcap's seq 58368-58369) */
  feed(12, ts, false, MTL_SESSION_PORT_R);
  feed(13, ts, true, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 7);
  EXPECT_TRUE(has_marker)
      << "Marker must survive mid-frame switchover with prior history";
  ut40_set_skip_drain(false);
}

/* Single-packet frame carrying the marker.  The simplest possible frame. */
TEST_F(St40RxRedundancyTest, MarkerOnSinglePacketFrame) {
  ut40_set_skip_drain(true);
  feed(0, 1000, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 1);
  EXPECT_TRUE(has_marker) << "Marker on single-packet frame must survive";
  ut40_set_skip_drain(false);
}

/* Both P and R send the same marker packet (same seq, same ts, both with marker=1).
 * P's packet is accepted, R's duplicate is filtered as redundant.
 * The accepted packet must carry the marker into the ring. */
TEST_F(St40RxRedundancyTest, MarkerDuplicateFromBothPorts) {
  ut40_set_skip_drain(true);
  uint32_t ts = 1000;
  /* P sends 4 packets, marker on seq 3 */
  for (int i = 0; i < 3; i++) feed(i, ts, false, MTL_SESSION_PORT_P);
  feed(3, ts, true, MTL_SESSION_PORT_P);
  /* R sends same 4 packets (duplicates), marker on seq 3 too — all filtered */
  for (int i = 0; i < 3; i++) feed(i, ts, false, MTL_SESSION_PORT_R);
  feed(3, ts, true, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 4) << "Only P's packets should be accepted, R's are redundant";
  EXPECT_TRUE(has_marker) << "Marker from P's accepted packet must survive";
  ut40_set_skip_drain(false);
}

/* Marker on a packet accepted via the threshold bypass path.
 * After 20+ consecutive redundant rejections on ALL ports, the filter
 * force-accepts the next packet.  The marker must survive this path. */
TEST_F(St40RxRedundancyTest, MarkerSurvivesThresholdBypass) {
  constexpr uint32_t ts_new = 6000;
  constexpr uint32_t ts_old = 5000;

  /* Establish state with newer timestamp */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_R);
  ut40_drain_ring();
  ut40_set_skip_drain(true);

  /* Send 20 old-ts packets on BOTH ports (below threshold, all rejected) */
  for (int i = 0; i < 20; i++) {
    feed(50 + i, ts_old, false, MTL_SESSION_PORT_P);
    feed(50 + i, ts_old, false, MTL_SESSION_PORT_R);
  }

  /* 21st pair: R goes first so its marker-bearing packet triggers the bypass.
   * The bypass resets R's error counter, so P's packet will be rejected
   * (P's counter is still >= threshold, but R's is now 0 < threshold).
   * Only R's marker packet gets accepted. */
  feed(70, ts_old, true, MTL_SESSION_PORT_R); /* marker — triggers bypass */
  feed(70, ts_old, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_GT(count, 0) << "At least one packet must be accepted via threshold bypass";
  EXPECT_TRUE(has_marker)
      << "Marker on threshold-bypass-accepted packet must survive into ring";
  ut40_set_skip_drain(false);
}

/* ── Previous-timestamp acceptance window ─────────────────────────────── */

/* After tmstamp advances from N to N+1, packets for timestamp N (the immediately
 * previous frame) must still be accepted by the redundancy filter.  This enables
 * the pipeline layer to receive late-arriving packets from the redundant port
 * and resolve pending frames with the correct marker bit. */
TEST_F(St40RxRedundancyTest, PrevTimestampPacketsAccepted) {
  ut40_set_skip_drain(true);
  uint32_t ts_n = 1000;
  uint32_t ts_n1 = 2000;

  /* P delivers frame N body: seq 0-2, ts=1000 */
  for (int i = 0; i < 3; i++) feed(i, ts_n, false, MTL_SESSION_PORT_P);
  /* P advances to frame N+1: seq 3, ts=2000 → tmstamp now 2000 */
  feed(3, ts_n1, false, MTL_SESSION_PORT_P);
  /* R delivers late packets for frame N: seq 4-5, ts=1000 */
  feed(4, ts_n, false, MTL_SESSION_PORT_R);
  feed(5, ts_n, true, MTL_SESSION_PORT_R); /* marker */

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);

  /* All 6 packets should be accepted: the filter must recognize ts=1000
   * as the immediately previous frame and accept R's late packets. */
  EXPECT_EQ(count, 6) << "Late prev-frame packets from R must be accepted";
  EXPECT_TRUE(has_marker) << "Late marker from R for prev_tmstamp must reach the ring";
  ut40_set_skip_drain(false);
}

/* Packets two frames back (tmstamp - 2) must still be rejected.
 * Only the immediately previous timestamp gets the acceptance window. */
TEST_F(St40RxRedundancyTest, TwoFramesBackRejected) {
  ut40_set_skip_drain(true);
  uint32_t ts_old = 1000;
  uint32_t ts_mid = 2000;
  uint32_t ts_cur = 3000;

  /* Advance through two frames: 1000 → 2000 → 3000 */
  feed(0, ts_old, false, MTL_SESSION_PORT_P);
  feed(1, ts_mid, false, MTL_SESSION_PORT_P); /* completes ts_old */
  feed(2, ts_cur, false, MTL_SESSION_PORT_P); /* completes ts_mid */

  /* Try sending a late packet for ts_old (two frames back) */
  feed(3, ts_old, false, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  /* Only 3 packets should be accepted (one per each ts advance).
   * The late ts_old packet must be rejected — it's 2 frames behind. */
  EXPECT_EQ(count, 3) << "Packets two timestamps behind must be rejected";
  ut40_set_skip_drain(false);
}

/* Late prev_tmstamp duplicates (same seq already accepted from P) must
 * still be filtered as redundant.  The prev_tmstamp acceptance window
 * must not bypass the bitmap deduplication. */
TEST_F(St40RxRedundancyTest, PrevTimestampDuplicatesFiltered) {
  ut40_set_skip_drain(true);
  uint32_t ts_n = 1000;
  uint32_t ts_n1 = 2000;

  /* P delivers full frame N: seq 0-3, marker on 3 */
  for (int i = 0; i < 3; i++) feed(i, ts_n, false, MTL_SESSION_PORT_P);
  feed(3, ts_n, true, MTL_SESSION_PORT_P);
  /* P starts frame N+1 → tmstamp advances to 2000 */
  feed(4, ts_n1, false, MTL_SESSION_PORT_P);
  /* R sends duplicates for frame N (same seq, same ts) */
  for (int i = 0; i < 3; i++) feed(i, ts_n, false, MTL_SESSION_PORT_R);
  feed(3, ts_n, true, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  /* Only 5 unique packets: 4 from P (ts=1000) + 1 from P (ts=2000).
   * R's 4 duplicates must be filtered out by the bitmap dedup. */
  EXPECT_EQ(count, 5) << "Late prev-frame duplicates from R must be filtered";
  ut40_set_skip_drain(false);
}

/* ── Hitless-merge redundancy scenarios ──────────────────────────── */

/*
 * The following tests exercise the per-port `lost_packets` counter
 * and the session-wide `stat_lost_packets` aggregate under various
 * loss / reorder / duplication patterns on a redundant pair.
 */

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

/* Producer-side defect: port P emits F=0x2 (first field) while port R
 * emits F=0x3 (second field) for the same logical frame.  This is a
 * SMPTE 2110-40 violation.  MTL currently accepts both polarities
 * silently; this test pins that behaviour. */
TEST_F(St40RxRedundancyTest, AsymmetricFieldBitsBetweenPorts) {
  /* P sends F=0x2, R sends F=0x3 for the same ts — disagreement. */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1000, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_R, 0x3);
  ut40_feed_pkt_fbits(ctx_, 1, 1000, 1, MTL_SESSION_PORT_R, 0x3);

  /* Current behaviour: both polarities are counted, no drop. */
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_GE(interlace_first(), 2u);  /* F=0x2 from P */
  EXPECT_GE(interlace_second(), 2u); /* F=0x3 from R */
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

/* Within a single timestamp, packets may legitimately arrive on a
 * single port in non-monotonic order (mid-frame reordering).  All
 * forward packets must be delivered; the backward arrival exercises
 * the bitmap acceptance path and must not be filtered as redundant. */
TEST_F(St40RxRedundancyTest, IntraFrameReorderSingleLeg) {
  /* deliver seqs in order 0,2,1,3 on port P only */
  feed(0, 1000, false, MTL_SESSION_PORT_P);
  feed(2, 1000, false, MTL_SESSION_PORT_P);
  /* seq 2 jumped over seq 1 -> per-port gap */
  EXPECT_GE(port_ooo(MTL_SESSION_PORT_P), 1u);
  feed(1, 1000, false, MTL_SESSION_PORT_P); /* backward arrival */
  feed(3, 1000, true, MTL_SESSION_PORT_P);
  /* Backward seq 1 lands inside the per-frame bitmap window: it is a
   * legitimate same-frame late arrival.  The bitmap path undoes the
   * earlier unrecovered++ that fired when seq 2 jumped over seq 1, so
   * the final unrecovered count must be 0. */
  EXPECT_EQ(unrecovered(), 0u)
      << "intra-frame backward arrival should cancel the pending unrecovered count";
}

/* Port R lags by more than the prev_tmstamp window (1 frame).  Once R
 * catches up, its packets for the long-stale frame should be filtered
 * as redundant — neither delivered nor counted as unrecovered.  This
 * documents the current 1-frame acceptance window depth. */
TEST_F(St40RxRedundancyTest, PortRLagsBeyondPrevTimestampWindow) {
  /* P advances through frames N, N+1, N+2 */
  feed(0, 1000, true, MTL_SESSION_PORT_P);
  feed(1, 2000, true, MTL_SESSION_PORT_P);
  feed(2, 3000, true, MTL_SESSION_PORT_P);

  uint64_t redundant_before = redundant();
  /* R now arrives with frame N data — 2 frames stale, outside the
   * prev_tmstamp window.  Must be filtered as redundant. */
  feed(0, 1000, true, MTL_SESSION_PORT_R);

  EXPECT_GT(redundant(), redundant_before)
      << "stale packets beyond prev-tmstamp window must be filtered as redundant";
}

/* Frame consisting of only a single marker packet (degenerate but
 * legal — e.g. an ANC frame with one ADF payload).  Must be delivered
 * cleanly to the ring with marker bit preserved. */
TEST_F(St40RxRedundancyTest, SinglePacketFrameMarkerOnly) {
  ut40_set_skip_drain(true);
  feed(0, 1000, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 1);
  EXPECT_TRUE(has_marker);
  EXPECT_EQ(unrecovered(), 0u);
  ut40_set_skip_drain(false);
}

/* Mid-stream field-bit flip in interlace_auto mode: producer suddenly
 * starts emitting progressive packets after a run of interlaced ones.
 * Detection must re-fire (interlace_detected reset) and progressive
 * packets must not be counted in either field bucket. */
TEST_F(St40RxRedundancyTest, InterlaceAutoFieldBitFlip) {
  ut40_ctx_set_interlace_auto(ctx_, true);
  /* warm up with interlaced first-field (F=0x2) */
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 0x2);
  EXPECT_EQ(interlace_first(), 2u);
  /* now flip to progressive (F=0x0) */
  ut40_feed_pkt_fbits(ctx_, 2, 1002, 0, MTL_SESSION_PORT_P, 0x0);
  ut40_feed_pkt_fbits(ctx_, 3, 1003, 0, MTL_SESSION_PORT_P, 0x0);
  /* progressive packets must NOT bump field counters */
  EXPECT_EQ(interlace_first(), 2u);
  EXPECT_EQ(interlace_second(), 0u);
  EXPECT_EQ(wrong_interlace(), 0u);
  EXPECT_EQ(received(), 4u);
}

/* Long accumulation: feed > 1000 packets with a regular gap pattern
 * to ensure the per-port OOO counter accumulates monotonically and
 * does not wrap.  Logically: the counter equals the number of
 * injected gap events.  Regression for `Fix: per-port OOO counter
 * uint16 wraparound`. */
TEST_F(St40RxRedundancyTest, PerPortOOOLargeAccumulation) {
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

/* ------------------------------------------------------------------------- */
/* New counters: per-port reordered_packets, per-port duplicates_same_port,  */
/* added in the proposal §3 implementation.                                  */
/* ------------------------------------------------------------------------- */

/* Backward seq on the same port (never seen on the other port) is a real
 * intra-port reorder and must bump reordered_packets, NOT lost_packets.
 */
TEST_F(St40RxRedundancyTest, ReorderedPacketsCounted) {
  constexpr uint32_t ts = 1000;
  /* feed 0, 2, 1, 3 on port P only. Order 0-2 creates a forward gap (lost=1).
   * When seq 1 then arrives, the old "late" packet is a same-port reorder. */
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* backward — reorder */
  feed(3, ts, true, MTL_SESSION_PORT_P);

  EXPECT_GE(port_reordered(MTL_SESSION_PORT_P), 1u)
      << "Backward seq on same port must be counted as reordered";
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u) << "Reorder is not a duplicate";
}

/* Exact same seq seen twice on the *same* port is a same-port duplicate
 * (e.g. switch/cable loop). It must NOT be confused with the cross-port
 * redundant copy tracked by stat_pkts_redundant. */
TEST_F(St40RxRedundancyTest, DuplicateSamePortCounted) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* true same-port dup */
  feed(2, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}

/* Cross-port F-bit divergence: same timestamp, same seq on each port, but
 * different F bits. Producer violation of SMPTE 2110-40. */
TEST_F(St40RxRedundancyTest, FieldBitMismatchDetected) {
  constexpr uint32_t ts = 1000;
  /* P sends F=0x2, R sends F=0x3 for the same frame (same tmstamp). */
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_R, 0x3);

  EXPECT_GE(field_bit_mismatch(), 1u) << "Cross-port F-bit divergence must be detected";
}

/* ------------------------------------------------------------------------- */
/* Additional edge-case tests for new counters. Testing against the spec:    */
/* - reordered/duplicates live in pre-redundancy per-port counters           */
/* - they MUST NOT be conflated with lost_packets or stat_pkts_redundant     */
/* - F-bit mismatch is per-packet and scoped to same-timestamp cross-port     */
/* ------------------------------------------------------------------------- */

/* Backward arrival must NOT inflate lost_packets (lost is forward-gap only). */
TEST_F(St40RxRedundancyTest, ReorderDoesNotInflateLostPackets) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P);
  uint64_t lost_before = port_ooo(MTL_SESSION_PORT_P);

  /* Pure reorder: 1 arrives again-as-backward (after 2). No forward gap. */
  feed(1, ts, false, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), lost_before)
      << "A backward arrival must not bump lost_packets";
  EXPECT_GE(port_reordered(MTL_SESSION_PORT_P), 1u);
}

/* Multiple same-port duplicates must each be counted. */
TEST_F(St40RxRedundancyTest, DuplicateSamePortMultiple) {
  constexpr uint32_t ts = 1000;
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, false, MTL_SESSION_PORT_P); /* dup #1 */
  feed(1, ts, false, MTL_SESSION_PORT_P); /* dup #2 */
  feed(1, ts, true, MTL_SESSION_PORT_P);  /* dup #3 */

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 3u)
      << "Every same-port re-arrival of the same seq must be counted";
}

/* Cross-port "normal" redundancy must NOT bump reordered_packets / duplicates_same_port
 * on either port. The counter is strictly for same-port repetitions. */
TEST_F(St40RxRedundancyTest, CrossPortRedundantIsNotSamePortDuplicate) {
  constexpr uint32_t ts = 1000;
  feed_burst(0, 4, ts, true, MTL_SESSION_PORT_P);
  feed_burst(0, 4, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_R), 0u);
  EXPECT_GE(redundant(), 1u) << "Cross-port redundancy must still count as redundant";
}

/* Matching F bits on both ports must NOT register a mismatch. */
TEST_F(St40RxRedundancyTest, FieldBitsMatchNoMismatch) {
  constexpr uint32_t ts = 1000;
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, ts, 1, MTL_SESSION_PORT_R, 0x2);

  EXPECT_EQ(field_bit_mismatch(), 0u)
      << "Matching F bits must not trigger the mismatch counter";
}

/* F-bit mismatches must be counted per packet — one mismatch per packet. */
TEST_F(St40RxRedundancyTest, FieldBitMismatchMultiple) {
  uint32_t ts = 1000;
  for (uint16_t i = 0; i < 5; i++) {
    ut40_feed_pkt_fbits(ctx_, i, ts, 1, MTL_SESSION_PORT_P, 0x2);
    ut40_feed_pkt_fbits(ctx_, i, ts, 1, MTL_SESSION_PORT_R, 0x3);
    ts += 3600;
  }
  EXPECT_GE(field_bit_mismatch(), 5u) << "One mismatch per offending packet expected";
}

/* F-bit check must be scoped to the *same* timestamp: different ts on each
 * port (intentional skew test) must not produce a mismatch. */
TEST_F(St40RxRedundancyTest, FieldBitMismatchOnlyWhenSameTs) {
  ut40_feed_pkt_fbits(ctx_, 0, 1000, 1, MTL_SESSION_PORT_P, 0x2);
  ut40_feed_pkt_fbits(ctx_, 0, 2000, 1, MTL_SESSION_PORT_R, 0x3);

  EXPECT_EQ(field_bit_mismatch(), 0u)
      << "Different timestamps: F bits belong to different frames, no divergence";
}

/* Reorder at the seq-wrap boundary: a small-value seq arriving after a
 * near-max seq must NOT be counted as reorder — mt_seq16_greater treats
 * the small value as the *forward* next wrap. */
TEST_F(St40RxRedundancyTest, SeqWrapNotCountedAsReorder) {
  constexpr uint32_t ts = 1000;
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  uint64_t reord_before = port_reordered(MTL_SESSION_PORT_P);

  feed(0, ts, false, MTL_SESSION_PORT_P); /* wraps forward, not backward */
  feed(1, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), reord_before)
      << "Wrap-around must be seen as forward, not reorder";
}

/* Same-port duplicate at seq-wrap boundary must still be counted. */
TEST_F(St40RxRedundancyTest, DuplicateSamePortAtSeqWrap) {
  constexpr uint32_t ts = 1000;
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P); /* dup right at boundary */

  EXPECT_EQ(port_duplicates(MTL_SESSION_PORT_P), 1u);
}

/* Invariant: stat_lost_packets == port[0].lost + port[1].lost at all times. */
TEST_F(St40RxRedundancyTest, LostPacketsInvariant) {
  constexpr uint32_t ts = 1000;
  /* Induce forward gap on each port, interleaved. */
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(2, ts, false, MTL_SESSION_PORT_P); /* gap of 1 */
  feed(0, ts, false, MTL_SESSION_PORT_R);
  feed(3, ts, false, MTL_SESSION_PORT_R); /* gap of 2 */

  EXPECT_EQ(ooo(), port_ooo(MTL_SESSION_PORT_P) + port_ooo(MTL_SESSION_PORT_R))
      << "stat_lost_packets must equal the sum of per-port lost_packets";
}
