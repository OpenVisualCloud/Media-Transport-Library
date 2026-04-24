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
 * a wrap-around pkt_idx and be rejected as out-of-bitmap. */
TEST_F(St20RxRedundancyTest, FirstPktNonZeroBaseFromOffset) {
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 1u);
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
