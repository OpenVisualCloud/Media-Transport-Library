/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Unit tests for ST2110-20 (video) RX redundancy filter.
 * Tests the three filtering layers:
 *   1. Slot-level timestamp filter (rv_slot_by_tmstamp)
 *   2. Frame-gone check (exist_ts && !slot->frame)
 *   3. Bitmap duplicate detection (mt_bitmap_test_and_set)
 *
 * Test geometry: 16x2 YUV422-10bit → 2 packets/frame, 40 bytes/pkt.
 * No main() — shared with ST40/ST30 test binary.
 */

#include <gtest/gtest.h>

#include "session/st20_harness.h"

/* ── test fixture ─────────────────────────────────────────────────────── */

class St20RxRedundancyTest : public ::testing::Test {
 protected:
  ut20_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }

  /* convenience wrappers */
  uint64_t received() {
    return ut20_stat_received(ctx_);
  }
  uint64_t redundant() {
    return ut20_stat_redundant(ctx_);
  }
  uint64_t ooo() {
    return ut20_stat_lost_pkts(ctx_);
  }
  uint64_t port_reordered(enum mtl_session_port p) {
    return ut20_stat_port_reordered(ctx_, p);
  }
  uint64_t port_lost(enum mtl_session_port p) {
    return ut20_stat_port_lost(ctx_, p);
  }
  uint64_t no_slot() {
    return ut20_stat_no_slot(ctx_);
  }
  uint64_t idx_oo_bitmap() {
    return ut20_stat_idx_oo_bitmap(ctx_);
  }
  uint64_t frames_incomplete() {
    return ut20_stat_frames_incomplete(ctx_);
  }
  int frames_received() {
    return ut20_frames_received(ctx_);
  }
  uint64_t wrong_pt() {
    return ut20_stat_wrong_pt(ctx_);
  }
  uint64_t wrong_ssrc() {
    return ut20_stat_wrong_ssrc(ctx_);
  }
  uint64_t wrong_interlace() {
    return ut20_stat_wrong_interlace(ctx_);
  }
  uint64_t offset_dropped() {
    return ut20_stat_offset_dropped(ctx_);
  }

  void feed(int pkt_idx, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt(ctx_, pkt_idx, ts, port);
  }

  void feed_seq(int pkt_idx, uint32_t seq, uint32_t ts, enum mtl_session_port port) {
    ut20_feed_frame_pkt_seq(ctx_, pkt_idx, seq, ts, port);
  }

  void feed_full(uint32_t ts, enum mtl_session_port port) {
    ut20_feed_full_frame(ctx_, ts, port);
  }
};

/* ── Basic redundancy (bitmap dedup) ────────────────────────────────────── */

/* Single port sends a complete 2-packet frame. Expects 1 frame received. */
TEST_F(St20RxRedundancyTest, SinglePortFullFrame) {
  feed_full(1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(frames_received(), 1);
}

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

/* ── Slot management ───────────────────────────────────────────────────── */

/* Two consecutive frames occupy two separate slots. Both delivered. */
TEST_F(St20RxRedundancyTest, TwoFramesTwoSlots) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(redundant(), 0u);
}

/* Three consecutive frames with 2 slots: slot 0 is reused for the third frame.
 * Frame 1 must be complete before the slot is recycled. */
TEST_F(St20RxRedundancyTest, SlotReuse) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);
  feed_full(3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(redundant(), 0u);
}

/* After both slots hold newer timestamps, a packet with an older timestamp
 * is rejected because no slot can accept it. */
TEST_F(St20RxRedundancyTest, OldTimestampAfterSlotReuse) {
  feed_full(2000, MTL_SESSION_PORT_P);
  feed_full(3000, MTL_SESSION_PORT_P);

  uint64_t recv_before = received();
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), recv_before) << "Old-timestamp packet should be rejected";
}

/* ── Timestamp-level filtering (rv_slot_by_tmstamp) ─────────────────── */

/* Past timestamp rejected by rv_slot_by_tmstamp. Both slots are occupied
 * with newer timestamps, so no slot can accept the stale packet. */
TEST_F(St20RxRedundancyTest, PastTimestampRejected) {
  feed_full(5000, MTL_SESSION_PORT_P);
  feed_full(6000, MTL_SESSION_PORT_P);

  uint64_t recv_before = received();
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), recv_before) << "Pkt older than all slots should be rejected";
}

/* Redundancy error threshold bypass for video. ST20 has two threshold paths:
 *  (a) rv_slot_by_tmstamp: old ts rejected when all slots are newer
 *  (b) frame-gone: exist_ts && !slot->frame → redundant_error_cnt++
 * Path (b) increments the counter. After both ports exceed threshold=20,
 * path (a) lets the old-timestamp packet allocate a new slot. */
TEST_F(St20RxRedundancyTest, ThresholdBypassVideo) {
  /* Fill both slots with completed frames */
  feed_full(5000, MTL_SESSION_PORT_P);
  feed_full(6000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 2);

  uint64_t recv_before = received();

  /* Send 21 old-timestamp packets for completed-frame timestamps.
   * These hit path (b): exist_ts && !slot->frame, incrementing
   * redundant_error_cnt on each port. */
  for (int i = 0; i < 21; i++) {
    feed_seq(0, 50000 + i, 5000, MTL_SESSION_PORT_P);
    feed_seq(0, 60000 + i, 5000, MTL_SESSION_PORT_R);
  }

  /* Now send a truly old timestamp that hits path (a) in rv_slot_by_tmstamp.
   * With threshold exceeded, it should be accepted (new slot allocated). */
  feed_seq(0, 90000, 1000, MTL_SESSION_PORT_P);

  EXPECT_GT(received(), recv_before)
      << "After exceeding threshold on both ports, old-ts packet should be accepted";
}

/* Frame-gone path: complete a frame, then send same-ts packets.
 * The slot exists (exist_ts=true) but frame is NULL → counted as redundant. */
TEST_F(St20RxRedundancyTest, FrameGoneRedundant) {
  feed_full(1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1);

  uint64_t red_before = redundant();
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_GT(redundant(), red_before)
      << "Pkt for completed frame should be counted as redundant";
}

/* ── Seq ID & bitmap edge cases ───────────────────────────────────────── */

/* 32-bit seq_id near wrap boundary. The base is 0xFFFFFFFE and the second
 * packet wraps to 0xFFFFFFFF. Both must be accepted. */
TEST_F(St20RxRedundancyTest, SeqIdWrapAround32) {
  uint32_t base_seq = 0xFFFFFFFE;
  feed_seq(0, base_seq, 1000, MTL_SESSION_PORT_P);
  feed_seq(1, base_seq + 1, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
}

/* Packet index outside the bitmap range is rejected and counted in
 * stat_pkts_idx_oo_bitmap. The bitmap is 1 byte = 8 bits, so pkt_idx >= 8
 * is out of range. */
TEST_F(St20RxRedundancyTest, PktIdxOutOfBitmap) {
  /* First pkt establishes seq_id_base */
  feed_seq(0, 100, 1000, MTL_SESSION_PORT_P);

  /* Now send a pkt whose seq gives pkt_idx = 100 (base=100, seq=200 → idx=100).
   * Use line 0 / offset 0 / length 40 — the line fields don't matter for this
   * test, only the seq_id distance from base matters. */
  ut20_feed_pkt(ctx_, 200, 1000, 0, 0, 40, MTL_SESSION_PORT_P);

  EXPECT_GE(idx_oo_bitmap(), 1u) << "Out-of-bitmap pkt_idx should be rejected";
}

/* Out-of-order packets within a frame: pkt 1 arrives before pkt 0.
 * Both must be accepted (bitmap allows any order). The first packet
 * establishes the slot base (no prior reference), and the backward
 * arrival of pkt 0 must NOT inflate the counter — forward gap counting
 * already tracks reordering when a later pkt_idx is seen first. This
 * test verifies no unsigned underflow from negative gap arithmetic. */
TEST_F(St20RxRedundancyTest, OutOfOrderWithinFrame) {
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(ooo(), 0u)
      << "Backward pkt_idx should not cause OOO underflow or double-counting";
}

/* ── Cross-port interleaving ────────────────────────────────────────────── */

/* Interleaved ports fill one frame: P sends pkt 0, R sends pkt 1.
 * The frame should complete as reconstructed from both ports. */
TEST_F(St20RxRedundancyTest, InterleavedPortsFillFrame) {
  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(redundant(), 0u);
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

/* ── Frame completion ─────────────────────────────────────────────────── */

/* Incomplete frame evicted by slot reuse. Only pkt 0 of frame ts=1000
 * is sent, then two full frames fill both slots. The third frame forces
 * slot reuse, evicting the incomplete frame 1. */
TEST_F(St20RxRedundancyTest, IncompleteFrameOnSlotReuse) {
  /* partial frame 1 — only pkt 0 */
  feed(0, 1000, MTL_SESSION_PORT_P);

  /* full frame 2 → fills slot 1 */
  feed_full(2000, MTL_SESSION_PORT_P);

  /* full frame 3 → forces slot 0 reuse, evicts incomplete frame 1 */
  feed_full(3000, MTL_SESSION_PORT_P);

  /* frame 1 was notified as incomplete (dropped), frames 2 & 3 complete */
  EXPECT_EQ(frames_received(), 2) << "Only complete frames increment stat";
  EXPECT_GE(frames_incomplete(), 1u) << "Incomplete frame should be counted";
}

/* After completing a frame the bitmap is reset. A new frame reusing the
 * same slot must not get false duplicate rejections from a stale bitmap. */
TEST_F(St20RxRedundancyTest, FrameCompletionResetsBitmap) {
  /* Frame 1 and 2 fill both slots */
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);

  /* Frame 3 reuses slot 0 (previously held frame 1) */
  feed_full(3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(redundant(), 0u) << "No false duplicates from stale bitmap";
}

/* ── Return values, validation filters ────────────────────────────────── */

/* Accepted packet must return 0. */
TEST_F(St20RxRedundancyTest, ReturnValueAccepted) {
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St20RxRedundancyTest, ReturnValueWrongPT) {
  ut20_ctx_set_pt(ctx_, 96);
  int rc = ut20_feed_pkt_pt(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St20RxRedundancyTest, ReturnValueWrongSSRC) {
  ut20_ctx_set_ssrc(ctx_, 1234);
  int rc = ut20_feed_pkt_ssrc(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Bitmap duplicate returns 0, not a negative error code. */
TEST_F(St20RxRedundancyTest, ReturnValueRedundantBitmap) {
  ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0) << "Bitmap duplicate returns 0, not error";
}

/* No slot available must return -EIO. */
TEST_F(St20RxRedundancyTest, ReturnValueNoSlot) {
  /* Fill both slots with completed frames */
  feed_full(5000, MTL_SESSION_PORT_P);
  feed_full(6000, MTL_SESSION_PORT_P);

  /* Old timestamp, both slots hold newer ts → no slot */
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Should reject when no slot available";
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St20RxRedundancyTest, WrongPayloadTypeDropped) {
  ut20_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++)
    ut20_feed_pkt_pt(ctx_, i, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St20RxRedundancyTest, WrongSSRCDropped) {
  ut20_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut20_feed_pkt_ssrc(ctx_, i, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* Second-field bit on a progressive stream triggers wrong_interlace. */
TEST_F(St20RxRedundancyTest, WrongInterlaceDropped) {
  /* ops.interlaced = false by default; send row_number with second_field bit set */
  int rc = ut20_feed_pkt(ctx_, 0, 1000, 0x8000, 0, 40, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, -EINVAL);
  EXPECT_EQ(wrong_interlace(), 1u);
}

/* Frame-gone path must not double-count as received. After completing a
 * frame, redundant packets for the same timestamp hit the frame-gone path
 * and must be counted as redundant ONLY, not also as received.
 * Invariant: received + redundant == total accepted packets. */
TEST_F(St20RxRedundancyTest, FrameGoneNoDoubleCount) {
  feed_full(1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1);
  EXPECT_EQ(received(), 2u);

  /* Send duplicates for the completed frame */
  feed(0, 1000, MTL_SESSION_PORT_R);
  feed(1, 1000, MTL_SESSION_PORT_R);

  /* Received must NOT increase — these are redundant */
  EXPECT_EQ(received(), 2u) << "Frame-gone redundant packets must not increment received";
  EXPECT_EQ(redundant(), 2u);
  /* Invariant: received + redundant == total accepted packets */
  EXPECT_EQ(received() + redundant(), 4u);
}

/* Multiple frames delivered in sequence with zero redundant. */
TEST_F(St20RxRedundancyTest, MultipleFrameDelivery) {
  for (uint32_t ts = 1000; ts < 1010; ts++) {
    feed_full(ts, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(frames_received(), 10);
  EXPECT_EQ(redundant(), 0u);
}

/* ── ST20 intra-frame reorder detection ─────────────────────────────────── */

/* Within a single frame, a packet whose pkt_idx is below the highest accepted
 * pkt_idx so far is an intra-frame reorder. It must bump
 * port[s_port].reordered_packets and must NOT be counted as redundant or lost. */
TEST_F(St20RxRedundancyTest, IntraFrameReorderCounted) {
  /* Send pkt_idx 1 first (becomes last_pkt_idx=1), then pkt_idx 0 (reorder). */
  feed(1, 1000, MTL_SESSION_PORT_P);
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u)
      << "pkt_idx 0 arriving after pkt_idx 1 must count as intra-frame reorder";
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
  /* Both packets accepted, frame complete */
  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(frames_received(), 1);
}

/* In-order delivery must not trip the reorder counter. */
TEST_F(St20RxRedundancyTest, InOrderDoesNotBumpReorder) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(1001, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 0u);
}

/* Reorder on port R must be attributed to R, not P. */
TEST_F(St20RxRedundancyTest, IntraFrameReorderPerPortIsolation) {
  feed(1, 1000, MTL_SESSION_PORT_R);
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_R), 1u);
  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}

/* Each new frame (new RTP timestamp) opens a fresh slot with last_pkt_idx
 * starting at 0 — in-order delivery in the next frame must not be misclassified
 * as reorder just because the previous frame ended at pkt_idx == 1. */
TEST_F(St20RxRedundancyTest, ReorderDoesNotLeakAcrossFrames) {
  feed_full(1000, MTL_SESSION_PORT_P); /* in-order, frame 1 */
  feed_full(1001, MTL_SESSION_PORT_P); /* in-order, frame 2 */

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 0u);
}

/* Reorder followed by a duplicate of the late packet must count exactly one
 * reorder and one redundant — never a second reorder. */
TEST_F(St20RxRedundancyTest, ReorderThenDuplicateNotDoubleCounted) {
  feed(1, 1000, MTL_SESSION_PORT_P); /* sets last_pkt_idx=1 */
  feed(0, 1000, MTL_SESSION_PORT_P); /* reorder: bit not yet set */
  feed(0, 1000, MTL_SESSION_PORT_P); /* duplicate: bit already set */

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), 1u);
  EXPECT_EQ(redundant(), 1u);
  EXPECT_EQ(port_lost(MTL_SESSION_PORT_P), 0u);
}
