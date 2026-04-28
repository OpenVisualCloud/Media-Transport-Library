/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Bitmap-based dedup and cross-port redundancy:
 * duplicate detection on same and cross ports, normal redundancy, burst
 * switchover (clean and reordered).
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxRedundancyTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxRedundancyTest : public St20RxBaseTest {};

/* Same packet sent twice on same port: the second is a bitmap duplicate. */
TEST_F(St20RxRedundancyTest, DuplicatePacketSamePort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(redundant(), 1u);
}

/* Same packet sent on P then R: the cross-port duplicate is rejected by bitmap. */
TEST_F(St20RxRedundancyTest, DuplicatePacketCrossPort) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(redundant(), 1u);
}

/* Full frame on P, duplicate full frame on R. The first frame completes
 * and is delivered; R's packets hit the frame-gone path (slot exists but
 * frame is NULL after delivery). */
TEST_F(St20RxRedundancyTest, NormalRedundancyTwoPorts) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(redundant(), 2u) << "Both pkts from port R should be redundant";
}

/* Clean burst switchover: frame 1 entirely from P, frame 2 entirely from R.
 * Both frames should be delivered. */
TEST_F(St20RxRedundancyTest, BurstSwitchoverVideoClean) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(redundant(), 0u);
}

/* Reordered burst switchover: frame 2 starts on R while frame 1's late
 * packets arrive on P. ST20's slot architecture keeps frame 1's slot
 * active while frame 2 occupies the other slot.
 *   1. P sends pkt 0 of frame ts=1000 (slot 0 allocated)
 *   2. R sends full frame ts=2000 (slot 1 allocated, completes)
 *   3. P sends pkt 1 of frame ts=1000 (goes to slot 0, completing frame 1)
 * Verifies late arrivals from P are accepted when the other slot advances. */
TEST_F(St20RxRedundancyTest, BurstSwitchoverVideoReordered) {
  /* pkt 0 of frame 1 from P */
  feed(0, 1000, MTL_SESSION_PORT_P);

  /* full frame 2 from R */
  feed_full(2000, MTL_SESSION_PORT_R);

  /* late pkt 1 of frame 1 from P — should complete frame 1 in its slot */
  feed(1, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2) << "Both frames should be delivered";
  EXPECT_EQ(received(), 4u) << "All 4 packets (2+2) should be received";
  EXPECT_EQ(redundant(), 0u) << "No packets should be filtered as redundant";
}

/* Bitmap duplicate returns 0, not a negative error code. */
TEST_F(St20RxRedundancyTest, ReturnValueRedundantBitmap) {
  ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0) << "Bitmap duplicate returns 0, not error";
}

/* ─────────────────────────────────────────────────────────────────────────
 * Cross-port reconstruction edge cases.
 * ───────────────────────────────────────────────────────────────────────── */

/* First arrival is NOT pkt 0: base must be derived from the packet's
 * frame offset, not from its seq. Otherwise R's later pkt 0 would map to
 * a wrap-around pkt_idx and be rejected as out-of-bitmap.
 *
 * Since `last_pkt_idx` is per-port, R's first packet (pkt 0) does not
 * count as reorder against P's high-water mark. */
TEST_F(St20RxRedundancyTest, FirstPktNonZeroBaseFromOffset) {
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u)
      << "R's first packet must not count as reorder against P's high-water mark";
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u)
      << "P delivered exactly one packet on its wire; cannot have self-reordered";
}

/* ST 2022-7 Class A (PD ≤ 10ms ≈ <1 frame @60fps): P advances to the next
 * frame while R is still finishing the previous one. Both frames complete. */
TEST_F(St20RxRedundancyTest, OneFrameOffsetBothComplete) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed(0, 2000, MTL_SESSION_PORT_P);
  feed_full(1000, MTL_SESSION_PORT_R);
  feed(1, 2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(no_slot(), 0u);
}

/* Wire corruption (RFC 3550 violates ST 2022-7 §6 but physically possible):
 * one port's seq is corrupted mid-frame; redundancy must recover via the
 * healthy port and surface the corrupt pkt as out-of-bitmap. */
TEST_F(St20RxRedundancyTest, WireCorruptedSeqRecoveredByPeer) {
  ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 40, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 50, 1000, 1, 0, 40, MTL_SESSION_PORT_P); /* corrupt */
  ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 40, MTL_SESSION_PORT_R);
  ut20_feed_pkt(ctx_, 101, 1000, 1, 0, 40, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_GE(idx_oo_bitmap(), 1u);
}

/* Per-frame seq base reset: a huge inter-frame seq jump (NIC reset, sender
 * restart) is absorbed because the new RTP timestamp opens a fresh slot. */
TEST_F(St20RxRedundancyTest, SeqJumpAtFrameBoundaryAbsorbed) {
  ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 40, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 101, 1000, 1, 0, 40, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 99999, 2000, 0, 0, 40, MTL_SESSION_PORT_P);
  ut20_feed_pkt(ctx_, 100000, 2000, 1, 0, 40, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
}

/* Non-conformant TX (violates ST 2022-7 §6: identical RTP headers required):
 * P and R use different RTP timestamps for the "same" frame. No cross-port
 * dedup happens — each port's frame completes independently. Pinned to
 * catch a future regression that mis-merges the slots. */
TEST_F(St20RxRedundancyTest, MismatchedTimestampsNoMerge) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1001, MTL_SESSION_PORT_R);
  feed(1, 1001, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(redundant(), 0u);
}

/* Late R duplicate of a completed frame: must hit the
 * `exist_ts && !slot->frame` fast-path and count as redundant, not as
 * no_slot or as bitmap dups against an unrelated slot. */
TEST_F(St20RxRedundancyTest, LateRDuplicateOfCompletedFrameIsRedundant) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);

  uint64_t red_before = redundant();
  feed_full(1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(redundant() - red_before, 2u);
  EXPECT_EQ(no_slot(), 0u);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-port frame accounting (frame-mode ST20).
 *
 * Contract for every completed frame in a redundant (num_port == 2) session
 * — see `rv_frame_notify` in `lib/src/st2110/st_rx_video_session.c`:
 *
 *   stat_frames_received      : always +1
 *   port[i].frames             : +1 IFF this port delivered every pkt of the
 *                                completed frame on its own
 *                                (slot->pkts_recv_per_port[i] >= pkts_received)
 *   frames_partial[i]          : +1 OTHERWISE
 *
 * Per-session invariant (per port i):
 *   port[i].frames + frames_partial[i] == stat_frames_received
 *
 * Reconstructed frames (one port short, the other short, union completes)
 * are intentionally NOT credited to either port[i].frames. This is the case
 * the user reported: "frame count was not increased on either port even
 * though reconstructed frames were good." It is by design — the session-wide
 * `stat_frames_received` carries the "frame happened" signal; the per-port
 * `frames_partial[i]` carries "this port could not have completed it alone".
 *
 * Note on the harness geometry: the synthetic frame is 2 packets. With only
 * 2 packets per frame it is impossible to construct a sequence in which BOTH
 * ports' counters reach pkts_received before completion (the completing
 * packet always arrives last on exactly one port). Tests that need both
 * ports credited would require a 3+ pkt/frame harness.
 * ───────────────────────────────────────────────────────────────────────── */

/* Single-port traffic into a redundant session. P delivers a full frame, R
 * sends nothing. Pins the somewhat surprising semantic that an idle R port
 * still bumps `frames_partial[R]` for every frame: completion logic is
 * unconditional on num_port. Also pins that an idle wire produces no
 * cross-port duplicates and no post-redundancy loss. */
TEST_F(St20RxRedundancyTest, PerPortFramesPrimaryOnly) {
  feed_full(1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 0u);
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_R), 1u)
      << "R was silent — per-port partial counter records the absence";
  EXPECT_EQ(redundant(), 0u) << "idle R wire produces no cross-port duplicates";
  EXPECT_EQ(pkts_unrecovered(), 0u)
      << "P delivered every packet alone; nothing post-redundancy missing";
}

/* P completes the frame, then R delivers a full duplicate after the slot
 * has already been released. R's packets hit the redundant fast-path
 * (slot->frame == NULL) and never reach the per-port frame-credit branch.
 * Therefore R's frame counter stays 0 even though R delivered every pkt. */
TEST_F(St20RxRedundancyTest, PerPortFramesPrimaryFirstSecondaryDuplicates) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 0u)
      << "R arrived after slot release; it never reaches the credit branch";
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(redundant(), 2u);
}

/* The user's reported case: half the frame from P, the other half from R.
 * Together they reconstruct one good frame. By design NEITHER port is
 * credited in port[i].frames; both bump frames_partial[i]. */
TEST_F(St20RxRedundancyTest, PerPortFramesReconstructedAcrossPorts) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1) << "reconstructed frame still counts session-wide";
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 0u)
      << "P delivered only 1 of 2 pkts — not credited";
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 0u)
      << "R delivered only 1 of 2 pkts — not credited";
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(pkts_unrecovered(), 0u)
      << "frame is complete; no packets missing post-redundancy";
}

/* Asymmetric split: P delivers pkt 0, R delivers pkt 0 (bitmap dup but its
 * per-port counter still bumps) then pkt 1. R's pkts_recv_per_port reaches
 * pkts_received at completion, so R is credited; P is not. */
TEST_F(St20RxRedundancyTest, PerPortFramesAsymmetricSplit) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R); /* bitmap dup; bumps R's per-port count */
  feed(1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 1u)
      << "R's per-port counter (incl. dup) reached pkts_received";
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_R), 0u);
  EXPECT_EQ(redundant(), 1u);
}

/* Three frames covering all three completion patterns (P-only, reconstructed,
 * R-only) and the per-port invariant
 *   port[i].frames + frames_partial[i] == stat_frames_received
 * holds at the end. */
TEST_F(St20RxRedundancyTest, PerPortFramesAcrossMultipleFramesInvariant) {
  /* frame A: P-only */
  feed_full(1000, MTL_SESSION_PORT_P);
  /* frame B: reconstructed (P pkt 0, R pkt 1) */
  feed(0, 2000, MTL_SESSION_PORT_P);
  feed(1, 2000, MTL_SESSION_PORT_R);
  /* frame C: R-only */
  feed_full(3000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_P), 1u) << "frame A only";
  EXPECT_EQ(port_frames(MTL_SESSION_PORT_R), 1u) << "frame C only";
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_P), 2u) << "frames B + C";
  EXPECT_EQ(frames_partial(MTL_SESSION_PORT_R), 2u) << "frames A + B";

  for (auto p : {MTL_SESSION_PORT_P, MTL_SESSION_PORT_R}) {
    EXPECT_EQ(port_frames(p) + frames_partial(p),
              static_cast<uint64_t>(frames_received()))
        << "invariant: per-port frames + partial == frames_received (port " << p << ")";
  }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Cross-wire reconstruction under realistic IP-network conditions.
 *
 * Tests below exercise scenarios in which the union of packets across both
 * wires covers a frame even though no individual wire delivered a complete
 * frame: cross-wire arrival reordering, intra-frame burst handover, mid-
 * frame primary failure, partitioning (non-conformant) transmitter, and
 * sustained cross-wire interleaving across many frames.
 * ───────────────────────────────────────────────────────────────────────── */

/* The trailing packet arrives on R before the leading packet arrives on P.
 * Cross-wire arrival order is a network property; it must not block
 * reconstruction. */
TEST_F(St20RxRedundancyTest, ReverseArrivalOrderReconstructs) {
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(frames_incomplete(), 0u);
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

/* Sustained cross-wire reconstruction: every frame is split (P pkt 0,
 * R pkt 1) for many consecutive frames. Verifies that prolonged cross-
 * wire cooperation does not leak slot state from one frame into the next
 * and never produces post-redundancy loss. */
TEST_F(St20RxRedundancyTest, EveryFrameStraddlesCrossPortHandover) {
  constexpr int N = 8;
  for (int i = 0; i < N; i++) {
    uint32_t ts = 1000 + 1000u * static_cast<uint32_t>(i);
    feed(0, ts, MTL_SESSION_PORT_P);
    feed(1, ts, MTL_SESSION_PORT_R);
  }
  EXPECT_EQ(frames_received(), N);
  EXPECT_EQ(pkts_unrecovered(), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Wide-frame cross-wire reconstruction (4 packets per frame).
 *
 * The default 2-pkt geometry cannot express an intra-frame gap on one
 * wire that the other wire fills, because gaps and reorders collapse
 * into the trivial first/last cases. These tests use a 4-pkt geometry.
 * ───────────────────────────────────────────────────────────────────────── */

class St20RxRedundancyWideTest : public ::testing::Test {
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
  uint64_t redundant() {
    return ut20_stat_redundant(ctx_);
  }
  uint64_t pkts_unrecovered() {
    return ut20_stat_pkts_unrecovered(ctx_);
  }
};

/* P drops every other packet on its wire; R fills exactly the dropped
 * indices. Verifies that cross-wire gap-filling works at arbitrary
 * positions inside a frame, not only at the boundaries. */
TEST_F(St20RxRedundancyWideTest, WideFrameReconstructsAcrossPorts) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R); /* P dropped pkt 1 on its wire */
  feed(2, 1000, MTL_SESSION_PORT_P);
  feed(3, 1000, MTL_SESSION_PORT_R); /* P dropped pkt 3 too */

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

/* Intra-frame port handover: P delivers the first half, R delivers the
 * second half. Models real-world failover patterns (link flip, upstream
 * failover) where a frame straddles the handover instant. */
TEST_F(St20RxRedundancyWideTest, IntraFrameHandoverReconstructs) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_P);
  /* Handover. */
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

/* Asymmetric failure: P delivers a single packet then goes silent
 * mid-frame (cable disconnected); R delivers the full frame. The single
 * cross-wire duplicate must be accounted as redundant. */
TEST_F(St20RxRedundancyWideTest, OnePortDiesAfterOnePacketReconstructs) {
  feed(0, 1000, MTL_SESSION_PORT_P); /* P then dies */
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(2, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(redundant(), 1u) << "R's pkt 0 is the duplicate of P's pkt 0";
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

/* Non-conformant transmitter: the two paths carry disjoint subsets of
 * the frame rather than identical copies. Neither wire ever observes a
 * complete frame, but their union does. The receiver must reconstruct
 * regardless of whether the transmitter respected the redundancy
 * contract. */
TEST_F(St20RxRedundancyWideTest, NonConformantTxPartitionReconstructs) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(2, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(3, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1)
      << "redundancy must reconstruct even from a partitioning TX";
  EXPECT_EQ(pkts_unrecovered(), 0u);
}

/* Both wires deliver every packet but interleaved at single-packet
 * granularity (P0, R0, P1, R1, ...). Verifies that duplicate detection
 * is based on pkt_idx rather than burst pattern: every R packet still
 * counts as a cross-wire duplicate. */
TEST_F(St20RxRedundancyWideTest, InterleavedDuplicatesAccounted) {
  for (int i = 0; i < kPktsPerFrame; i++) {
    feed(i, 1000, MTL_SESSION_PORT_P);
    feed(i, 1000, MTL_SESSION_PORT_R);
  }
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(redundant(), static_cast<uint64_t>(kPktsPerFrame));
}
