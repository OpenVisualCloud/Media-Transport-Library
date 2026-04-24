/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Slot management:
 * slot allocation/reuse, frame-gone path, threshold bypass for past timestamps,
 * incomplete-frame and frame-completion accounting.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxSlotTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxSlotTest : public St20RxBaseTest {};

/* Single port sends a complete 2-packet frame. Expects 1 frame received. */
TEST_F(St20RxSlotTest, SinglePortFullFrame) {
  feed_full(1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(frames_received(), 1);
}

/* Two consecutive frames occupy two separate slots. Both delivered. */
TEST_F(St20RxSlotTest, TwoFramesTwoSlots) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2);
  EXPECT_EQ(redundant(), 0u);
}

/* Three consecutive frames with 2 slots: slot 0 is reused for the third frame.
 * Frame 1 must be complete before the slot is recycled. */
TEST_F(St20RxSlotTest, SlotReuse) {
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);
  feed_full(3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(redundant(), 0u);
}

/* After both slots hold newer timestamps, a packet with an older timestamp
 * is rejected because no slot can accept it. */
TEST_F(St20RxSlotTest, OldTimestampAfterSlotReuse) {
  feed_full(2000, MTL_SESSION_PORT_P);
  feed_full(3000, MTL_SESSION_PORT_P);

  uint64_t recv_before = received();
  feed(0, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), recv_before) << "Old-timestamp packet should be rejected";
}

/* Redundancy error threshold bypass for video. ST20 has two threshold paths:
 *  (a) rv_slot_by_tmstamp: old ts rejected when all slots are newer
 *  (b) frame-gone: exist_ts && !slot->frame → redundant_error_cnt++
 * Path (b) increments the counter. After both ports exceed threshold=20,
 * path (a) lets the old-timestamp packet allocate a new slot. */
TEST_F(St20RxSlotTest, ThresholdBypassVideo) {
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
TEST_F(St20RxSlotTest, FrameGoneRedundant) {
  feed_full(1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 1);

  uint64_t red_before = redundant();
  feed(0, 1000, MTL_SESSION_PORT_R);

  EXPECT_GT(redundant(), red_before)
      << "Pkt for completed frame should be counted as redundant";
}

/* Incomplete frame evicted by slot reuse. Only pkt 0 of frame ts=1000
 * is sent, then two full frames fill both slots. The third frame forces
 * slot reuse, evicting the incomplete frame 1. */
TEST_F(St20RxSlotTest, IncompleteFrameOnSlotReuse) {
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
TEST_F(St20RxSlotTest, FrameCompletionResetsBitmap) {
  /* Frame 1 and 2 fill both slots */
  feed_full(1000, MTL_SESSION_PORT_P);
  feed_full(2000, MTL_SESSION_PORT_P);

  /* Frame 3 reuses slot 0 (previously held frame 1) */
  feed_full(3000, MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 3);
  EXPECT_EQ(redundant(), 0u) << "No false duplicates from stale bitmap";
}

/* Frame-gone path must not double-count as received. After completing a
 * frame, redundant packets for the same timestamp hit the frame-gone path
 * and must be counted as redundant ONLY, not also as received.
 * Invariant: received + redundant == total accepted packets. */
TEST_F(St20RxSlotTest, FrameGoneNoDoubleCount) {
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
TEST_F(St20RxSlotTest, MultipleFrameDelivery) {
  for (uint32_t ts = 1000; ts < 1010; ts++) {
    feed_full(ts, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(frames_received(), 10);
  EXPECT_EQ(redundant(), 0u);
}

/* Slot allocation across the 32-bit RTP timestamp wrap boundary.
 * Slot 0 holds ts near 0xFFFFFFF0, slot 1 holds ts near 0xFFFFFFFE.
 * A new packet with ts wrapping to 0x00000010 must be ranked as
 * "newer" by `mt_seq32_greater` and recycle the oldest slot, NOT be
 * rejected as "timestamp in the past". */
TEST_F(St20RxSlotTest, SlotTimestampWrapAroundAdvance) {
  feed_full(0xFFFFFFF0, MTL_SESSION_PORT_P);
  feed_full(0xFFFFFFFE, MTL_SESSION_PORT_P);

  uint64_t recv_before = received();
  int frames_before = frames_received();
  feed_full(0x00000010, MTL_SESSION_PORT_P); /* wraps over UINT32_MAX */

  EXPECT_EQ(received() - recv_before, 2u)
      << "wrapped timestamp must allocate a new slot, not be dropped as past";
  EXPECT_EQ(frames_received() - frames_before, 1) << "the wrapped frame must complete";
}
