/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Unit tests for ST2110-30 (audio) RX redundancy filter.
 * ST30 uses TIMESTAMP-ONLY redundancy: packets with non-increasing
 * timestamps are filtered, regardless of sequence number.
 */

#include <gtest/gtest.h>

#include "session/st30_harness.h"

/* ── fixture ───────────────────────────────────────────────────────── */

class St30RxRedundancyTest : public ::testing::Test {
 protected:
  ut30_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut30_init(), 0) << "EAL init failed";
    ctx_ = ut30_ctx_create(2); /* 2 ports = redundant */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut30_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  int feed(uint16_t seq, uint32_t ts, enum mtl_session_port port) {
    return ut30_feed_pkt(ctx_, seq, ts, port);
  }

  void feed_burst(uint16_t seq_start, int count, uint32_t ts,
                  enum mtl_session_port port) {
    ut30_feed_burst(ctx_, seq_start, count, ts, port);
  }

  uint64_t unrecovered() {
    return ut30_stat_unrecovered(ctx_);
  }
  uint64_t redundant() {
    return ut30_stat_redundant(ctx_);
  }
  uint64_t received() {
    return ut30_stat_received(ctx_);
  }
  uint64_t ooo() {
    return ut30_stat_out_of_order(ctx_);
  }
  int session_seq() {
    return ut30_session_seq_id(ctx_);
  }
  int frames_done() {
    return ut30_frames_received(ctx_);
  }
  int ppf() {
    return ut30_pkts_per_frame(ctx_);
  }

  uint64_t port_pkts(enum mtl_session_port p) {
    return ut30_stat_port_pkts(ctx_, p);
  }
  uint64_t port_bytes(enum mtl_session_port p) {
    return ut30_stat_port_bytes(ctx_, p);
  }
  uint64_t port_ooo(enum mtl_session_port p) {
    return ut30_stat_port_ooo(ctx_, p);
  }
  uint64_t wrong_pt() {
    return ut30_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut30_stat_wrong_ssrc(ctx_);
  }
  uint64_t len_mismatch() {
    return ut30_stat_len_mismatch(ctx_);
  }
};

/* Same packets on both ports. ST30 filters by timestamp: R's packets
 * have timestamps not strictly greater than P's, so all are filtered.
 * feed_burst auto-increments ts per packet (real ST30 behavior). */
TEST_F(St30RxRedundancyTest, NormalRedundancy) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_R);

  feed_burst(4, 4, 2000, MTL_SESSION_PORT_P);
  feed_burst(4, 4, 2000, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 8u);
  EXPECT_EQ(received(), 8u);
}

/* Timestamp-only filter: R's packets are filtered even with higher seq_ids,
 * because their timestamps are not strictly greater than P's latest.
 * This is the key ST30 difference from ST40 (which checks both ts and seq). */
TEST_F(St30RxRedundancyTest, TimestampOnlyFilter) {
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);

  /* port 1 has HIGHER seq_ids but same/old timestamps → filtered */
  for (int i = 0; i < 4; i++) {
    int rc = feed(4 + i, 1000 + i, MTL_SESSION_PORT_R);
    EXPECT_LT(rc, 0) << "seq " << (4 + i)
                     << " should be filtered (ts not greater, ST30 ignores seq)";
  }

  EXPECT_EQ(redundant(), 4u);
  EXPECT_EQ(received(), 4u);
}

/* New timestamp always accepted even if seq goes backward.
 * R has lower seq_ids but strictly newer timestamps, so packets are accepted.
 * A source switch with new timestamps must not produce phantom unrecovered. */
TEST_F(St30RxRedundancyTest, NewTimestampAlwaysAccepted) {
  feed_burst(10, 4, 2000, MTL_SESSION_PORT_P);
  feed_burst(0, 4, 3000, MTL_SESSION_PORT_R);

  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 8u);
  /* Session seq after P = 13. R brings seq 0. The seq gap wraps to 65522
   * via uint16_t arithmetic. This is phantom loss from source switch,
   * not real packet loss. */
  EXPECT_EQ(unrecovered(), 0u)
      << "Source switch with new timestamp should not produce phantom unrecovered";
}

/* Same timestamp, different seq on second port. Even though R has a
 * higher seq_id, it is filtered because the timestamp is not strictly
 * greater. */
TEST_F(St30RxRedundancyTest, SameTimestampDiffSeqFiltered) {
  feed(5, 1000, MTL_SESSION_PORT_P);

  int rc = feed(6, 1000, MTL_SESSION_PORT_R);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered even with newer seq";
  EXPECT_EQ(redundant(), 1u);
}

/* Burst switchover for audio: R has newer timestamps and is processed
 * first, then P arrives with old timestamps. P's late arrivals have
 * stale timestamps and must be filtered. */
TEST_F(St30RxRedundancyTest, BurstSwitchoverAudio) {
  /* establish history */
  feed_burst(0, 4, 1000, MTL_SESSION_PORT_P);

  /* port 1 arrives first with NEW timestamps */
  feed_burst(6, 4, 2000, MTL_SESSION_PORT_R);

  /* port 0 late arrivals with OLD timestamps */
  feed(4, 1004, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  /* both late packets should be filtered (old timestamp < 2003) */
  EXPECT_EQ(redundant(), 2u);
  /* the 4 packets from port 1 advance seq from 3 to 6 — gap of 2 */
  EXPECT_EQ(unrecovered(), 2u);
}

/* Multiple packets with the same timestamp on the same port. Only the
 * first packet per timestamp is accepted; subsequent ones are filtered. */
TEST_F(St30RxRedundancyTest, SameTimestampFiltered) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  /* same ts again — filtered */
  int rc = feed(1, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered";
  /* a third one too */
  rc = feed(5, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Same timestamp should be filtered";
  EXPECT_EQ(redundant(), 2u);
  EXPECT_EQ(received(), 1u);
}

/* Audio frame boundary: feed exactly pkts_per_frame packets with
 * incrementing timestamps and verify exactly one frame is completed. */
TEST_F(St30RxRedundancyTest, AudioFrameBoundary) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total_pkts = ppf();
  ASSERT_GT(total_pkts, 0);

  /* each packet needs a unique (increasing) timestamp */
  for (int i = 0; i < total_pkts; i++) {
    feed(i, 1000 + i, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(received(), (uint64_t)total_pkts);
  EXPECT_EQ(frames_done(), 1);
  EXPECT_EQ(unrecovered(), 0u);
}

/* Redundancy error threshold bypass: 21 old-timestamp packets on both
 * ports should force-accept once both exceed threshold=20. */
TEST_F(St30RxRedundancyTest, ThresholdBypass) {
  /* establish state with a newer timestamp */
  feed_burst(0, 4, 5000, MTL_SESSION_PORT_P);
  /* also init port 1 with same timestamps so its error counter resets */
  feed_burst(0, 4, 5004, MTL_SESSION_PORT_R);

  uint64_t recv_before = received();

  /* send 21 old-timestamp packets on BOTH ports, alternating */
  for (int i = 0; i < 21; i++) {
    feed(50 + i, 1000 + i, MTL_SESSION_PORT_P);
    feed(50 + i, 1000 + i, MTL_SESSION_PORT_R);
  }

  /* After both ports exceed threshold (20), the 21st pair should be accepted.
   * Requires redundant_error_cnt to be incremented in the ST30 handler. */
  EXPECT_GT(received(), recv_before)
      << "After exceeding threshold on both ports, packets should be accepted";
}

/* 32-bit timestamp wraparound from near UINT32_MAX to near zero.
 * The wrapped timestamps should be accepted as strictly greater. */
TEST_F(St30RxRedundancyTest, TimestampWrapAround) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed_burst(0, 4, 0xFFFFFFF0, MTL_SESSION_PORT_P);
  feed_burst(4, 4, 0x00000010, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(received(), 8u);
}

/* Large timestamp jump: packets are accepted but the sequence gap between
 * them is correctly counted as unrecovered. */
TEST_F(St30RxRedundancyTest, RapidTimestampAdvance) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(100, 999000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(unrecovered(), 99u);
}

/* Backward timestamp with newer seq_id. ST30 filters on timestamp ONLY,
 * so this must be rejected even though the seq_id advances. */
TEST_F(St30RxRedundancyTest, OldTimestampNewSeq) {
  feed_burst(0, 4, 5000, MTL_SESSION_PORT_P);

  /* newer seq but older timestamp — must be rejected */
  int rc = feed(4, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Packet with old timestamp must be rejected";
  EXPECT_GE(redundant(), 1u);
}

/* Single-port baseline: only one port configured, no redundancy filtering.
 * All packets accepted, zero redundant. */
TEST_F(St30RxRedundancyTest, SinglePortBaseline) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  /* 8 packets, each with a unique increasing timestamp */
  for (int i = 0; i < 8; i++) {
    feed(i, 1000 + i, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 8u);
}

/* Interleaved ports with increasing timestamps. P and R alternate, each
 * packet with a strictly newer timestamp. All packets accepted. */
TEST_F(St30RxRedundancyTest, InterleavedPortsIncreasingTs) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_R);
  feed(2, 1002, MTL_SESSION_PORT_P);
  feed(3, 1003, MTL_SESSION_PORT_R);
  feed(4, 1004, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_R);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 6u);
}

/* Duplicate packet on the same port: same seq+ts resent.
 * The duplicate must be filtered as redundant. */
TEST_F(St30RxRedundancyTest, DuplicatePacketSamePort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1001, MTL_SESSION_PORT_P);
  feed(2, 1002, MTL_SESSION_PORT_P);

  /* exact duplicate of last packet */
  int rc = feed(2, 1002, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Duplicate timestamp packet should be filtered";
  EXPECT_EQ(redundant(), 1u);
}

/* Back-to-back monotonic timestamps on a single port, no gaps.
 * All packets accepted, zero unrecovered and redundant. */
TEST_F(St30RxRedundancyTest, BackToBackMonotonic) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  for (int i = 0; i < 40; i++) {
    feed(i, 1000 + i, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 40u);
}

/* ── Return values, validation filters, per-port statistics ────────── */

/* Accepted packet must return 0. */
TEST_F(St30RxRedundancyTest, ReturnValueAccepted) {
  int rc = feed(0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St30RxRedundancyTest, ReturnValueWrongPT) {
  ut30_ctx_set_pt(ctx_, 96);
  int rc = ut30_feed_pkt_pt(ctx_, 0, 1000, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St30RxRedundancyTest, ReturnValueWrongSSRC) {
  ut30_ctx_set_ssrc(ctx_, 1234);
  int rc = ut30_feed_pkt_ssrc(ctx_, 0, 1000, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong payload length must return -EINVAL. */
TEST_F(St30RxRedundancyTest, ReturnValueWrongLen) {
  int rc = ut30_feed_pkt_len(ctx_, 0, 1000, MTL_SESSION_PORT_P, 100);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St30RxRedundancyTest, WrongPayloadTypeDropped) {
  ut30_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++) ut30_feed_pkt_pt(ctx_, i, 1000 + i, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St30RxRedundancyTest, WrongSSRCDropped) {
  ut30_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut30_feed_pkt_ssrc(ctx_, i, 1000 + i, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong payload length packets are dropped and counted in len_mismatch stat. */
TEST_F(St30RxRedundancyTest, WrongPacketLenDropped) {
  ut30_feed_pkt_len(ctx_, 0, 1000, MTL_SESSION_PORT_P, 50);
  ut30_feed_pkt_len(ctx_, 1, 1001, MTL_SESSION_PORT_P, 300);

  EXPECT_EQ(len_mismatch(), 2u);
  EXPECT_EQ(received(), 0u);
}

/* Per-port packet counters track packets received on each port independently. */
TEST_F(St30RxRedundancyTest, PortPacketCount) {
  /* feed 5 pkts on P with increasing ts */
  for (int i = 0; i < 5; i++) feed(i, 1000 + i, MTL_SESSION_PORT_P);
  /* feed 3 pkts on R with further increasing ts */
  for (int i = 0; i < 3; i++) feed(5 + i, 2000 + i, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_P), 5u);
  EXPECT_EQ(port_pkts(MTL_SESSION_PORT_R), 3u);
}

/* Per-port OOO: gap on P only, R is sequential. OOO must be isolated. */
TEST_F(St30RxRedundancyTest, PortOOOPerPort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P); /* gap of 4 */
  feed(6, 2000, MTL_SESSION_PORT_R);
  feed(7, 2001, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), 4u);
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_R), 0u);
}

/* Single complete frame: exactly pkts_per_frame packets produce 1 frame. */
TEST_F(St30RxRedundancyTest, FrameCompletionCount) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total = ppf();
  for (int i = 0; i < total; i++) feed(i, 1000 + i, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_done(), 1);
  EXPECT_EQ(received(), (uint64_t)total);
}

/* Three complete frames: 3×pkts_per_frame packets produce 3 frames. */
TEST_F(St30RxRedundancyTest, MultipleFrames) {
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
TEST_F(St30RxRedundancyTest, SeqGapExactCount) {
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
TEST_F(St30RxRedundancyTest, PerPortOOOBackwardSeq) {
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
TEST_F(St30RxRedundancyTest, DuplicateSeqPortOOOWrapping) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(5, 1005, MTL_SESSION_PORT_P);

  uint64_t ooo_before = port_ooo(MTL_SESSION_PORT_P);

  /* re-feed seq 5 with same ts — filtered as redundant */
  feed(5, 1005, MTL_SESSION_PORT_P);

  /* Duplicate seq should not inflate OOO counter */
  EXPECT_EQ(port_ooo(MTL_SESSION_PORT_P), ooo_before)
      << "Duplicate seq on same port should not inflate OOO counter";
}
