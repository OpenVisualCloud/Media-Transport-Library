/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Slot management:
 * slot allocation/reuse, frame-gone path, threshold bypass for past timestamps,
 * incomplete-frame and frame-completion accounting.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest \
 *          --gtest_filter='St20RxSlotTest.*:*ClockStepTest.*'
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

/* Redundancy error threshold bypass for video: rv_slot_by_tmstamp drops a ts
 * older than every slot and counts it per port; once every port reaches
 * threshold=20, the old-timestamp packet allocates a new slot. */
TEST_F(St20RxSlotTest, ThresholdBypassVideo) {
  /* Fill both slots with completed frames */
  feed_full(5000, MTL_SESSION_PORT_P);
  feed_full(6000, MTL_SESSION_PORT_P);
  EXPECT_EQ(frames_received(), 2);

  uint64_t recv_before = received();

  /* 19 old-timestamp packets on P and 20 on R: P is still below threshold. */
  for (int i = 0; i < 19; i++) {
    feed_seq(0, 50000 + i, 1000, MTL_SESSION_PORT_P);
    feed_seq(0, 60000 + i, 1000, MTL_SESSION_PORT_R);
  }
  feed_seq(0, 60019, 1000, MTL_SESSION_PORT_R);
  EXPECT_EQ(received(), recv_before);

  /* P's 20th brings every port to the threshold, so it is accepted (new slot). */
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

/* A sender clock stepping back 37 s (TAI to UTC), in 90 kHz RTP ticks at 30 fps. */
static constexpr uint32_t kFrameTicks = 3000;
static constexpr uint32_t kNowTs = 0x40000000;
static constexpr uint32_t kSteppedTs = kNowTs - 37u * 90000u;
static constexpr int kSteppedFrames = 30;
/* A port drops 19 past pkts and resyncs on the 20th, inside stepped frame 9, so
 * frames 10..29 complete. */
static constexpr int kResyncedFrames = 20;

class St20RxClockStepTest : public St20RxBaseTest {
 protected:
  /* RTP seq keeps counting across the step, and both ports carry the same seq. */
  void feed_pkt(int frame_no, int pkt_idx, uint32_t ts, enum mtl_session_port port) {
    feed_seq(pkt_idx, static_cast<uint32_t>(frame_no * pkts_per_frame() + pkt_idx), ts,
             port);
  }
  void feed_frame(int frame_no, uint32_t ts, enum mtl_session_port port) {
    for (int i = 0; i < pkts_per_frame(); i++) feed_pkt(frame_no, i, ts, port);
  }
  /* Both legs interleaved per pkt, as equal-delay 2022-7 paths deliver them. */
  void feed_redundant_frame(int frame_no, uint32_t ts) {
    for (int i = 0; i < pkts_per_frame(); i++) {
      feed_pkt(frame_no, i, ts, MTL_SESSION_PORT_P);
      feed_pkt(frame_no, i, ts, MTL_SESSION_PORT_R);
    }
  }
  static uint32_t now_ts(int frame_no) {
    return kNowTs + static_cast<uint32_t>(frame_no) * kFrameTicks;
  }
  static uint32_t stepped_ts(int frame_no) {
    return kSteppedTs + static_cast<uint32_t>(frame_no) * kFrameTicks;
  }
};

class St20RxSinglePortClockStepTest : public St20RxClockStepTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

TEST_F(St20RxSinglePortClockStepTest, SteppedBackSenderResyncs) {
  feed_frame(0, now_ts(0), MTL_SESSION_PORT_P);
  feed_frame(1, now_ts(1), MTL_SESSION_PORT_P);
  ASSERT_EQ(frames_received(), 2);

  for (int k = 0; k < kSteppedFrames; k++)
    feed_frame(2 + k, stepped_ts(k), MTL_SESSION_PORT_P);

  EXPECT_EQ(frames_received(), 2 + kResyncedFrames);
}

class St20RxSinglePortDmaClockStepTest : public St20RxSinglePortClockStepTest {
 protected:
  int pkts_per_frame() const override {
    return 4;
  }
  void SetUp() override {
    St20RxSinglePortClockStepTest::SetUp();
    ut20_ctx_enable_dma(ctx_);
  }
};

/* A resync the busy DMA engine refuses waits for another 20 old pkts, rather
 * than being retried, and warned about, on every one of them. */
TEST_F(St20RxSinglePortDmaClockStepTest, DmaBusyResyncWaitsForThreshold) {
  feed_frame(0, now_ts(0), MTL_SESSION_PORT_P);
  ut20_dma_set_stalled(ctx_, true);

  for (int k = 0; k < 10; k++) feed_frame(1 + k, stepped_ts(k), MTL_SESSION_PORT_P);

  /* the 20th and the 40th of the 40 old pkts */
  EXPECT_EQ(ut20_dma_busy_drops(ctx_), 2);
}

/* R lags 3 frames, past both slots, while P is current: R must never pull the
 * session back onto its own timeline. */
TEST_F(St20RxClockStepTest, LaggingRedundantPortDoesNotResync) {
  const int lag = 3;
  for (int f = 0; f < kSteppedFrames + lag; f++) {
    feed_frame(f, now_ts(f), MTL_SESSION_PORT_P);
    if (f >= lag) feed_frame(f - lag, now_ts(f - lag), MTL_SESSION_PORT_R);
  }

  EXPECT_EQ(frames_received(), kSteppedFrames + lag);
  EXPECT_EQ(no_slot(), static_cast<uint64_t>(kSteppedFrames * pkts_per_frame()));
}

/* Enough pkts per frame for a slot without a frame buffer to see 20 of them. */
class St20RxWideClockStepTest : public St20RxClockStepTest {
 protected:
  int pkts_per_frame() const override {
    return 32;
  }
};

/* P starves of frame buffers while R lags past the slot window: P's pkts that
 * find no buffer must not let R pull the session onto R's timeline. */
TEST_F(St20RxWideClockStepTest, LaggingPortDoesNotResyncWhileLeaderStarves) {
  const int lag = 3;
  for (int f = 0; f < 12; f++) {
    if (f == 5) ut20_set_hold_frames(ctx_, true);
    if (f == 8) ut20_set_hold_frames(ctx_, false);
    feed_frame(f, now_ts(f), MTL_SESSION_PORT_P);
    if (f >= lag) feed_frame(f - lag, now_ts(f - lag), MTL_SESSION_PORT_R);
  }

  /* the 2 buffers go to held frames 5 and 6, so only frame 7 is lost */
  EXPECT_EQ(frames_received(), 11);
}

/* RTCP retransmits are expected to be old: a run of them for a frame both slots
 * have moved past must not resync onto it and evict the two open frames. */
TEST_F(St20RxWideClockStepTest, LateRetransmitsDoNotResync) {
  const int lost = 20;
  auto feed_both = [this](int frame_no, int pkt_idx) {
    feed_pkt(frame_no, pkt_idx, now_ts(frame_no), MTL_SESSION_PORT_P);
    feed_pkt(frame_no, pkt_idx, now_ts(frame_no), MTL_SESSION_PORT_R);
  };
  for (int i = lost; i < pkts_per_frame(); i++) feed_both(0, i);
  feed_both(1, 0);
  feed_both(2, 0);
  for (int i = 0; i < lost; i++) {
    for (auto port : {MTL_SESSION_PORT_P, MTL_SESSION_PORT_R})
      ut20_feed_retransmit_pkt(ctx_, i, static_cast<uint32_t>(i), now_ts(0), port);
  }
  for (int f = 1; f <= 2; f++)
    for (int i = 1; i < pkts_per_frame(); i++) feed_both(f, i);

  EXPECT_EQ(frames_received(), 2);
}

class St20RxSinglePortWideClockStepTest : public St20RxWideClockStepTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

/* Old pkts interleaved with a frame that keeps landing pkts are not consecutive. */
TEST_F(St20RxSinglePortWideClockStepTest, InterleavedOldPktsDoNotResync) {
  feed_frame(0, now_ts(0), MTL_SESSION_PORT_P);
  for (int i = 0; i < pkts_per_frame(); i++) {
    feed_pkt(1, i, now_ts(1), MTL_SESSION_PORT_P);
    feed_pkt(1, i, stepped_ts(1), MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(frames_received(), 2);
}

/* A duplicate of a finished frame is a current pkt, so it must not count as old. */
TEST_F(St20RxSinglePortWideClockStepTest, DuplicateOfFinishedFrameDoesNotCount) {
  feed_frame(0, now_ts(0), MTL_SESSION_PORT_P);
  feed_pkt(0, 0, now_ts(0), MTL_SESSION_PORT_P);
  feed_frame(1, stepped_ts(0), MTL_SESSION_PORT_P);

  EXPECT_EQ(no_slot(), 19u);
}

TEST_F(St20RxClockStepTest, BothPortsSteppedBackResync) {
  feed_redundant_frame(0, now_ts(0));
  feed_redundant_frame(1, now_ts(1));
  ASSERT_EQ(frames_received(), 2);

  for (int k = 0; k < kSteppedFrames; k++) feed_redundant_frame(2 + k, stepped_ts(k));

  EXPECT_EQ(frames_received(), 2 + kResyncedFrames);
}

class St22RxSinglePortClockStepTest : public St20RxClockStepTest {
 protected:
  static constexpr uint16_t kPayload = 1000;

  int num_port() const override {
    return 1;
  }
  void SetUp() override {
    St20RxClockStepTest::SetUp();
    ASSERT_EQ(ut20_ctx_attach_st22(ctx_, false, 4096), 0);
  }
  void feed_st22_frame(int frame_no, uint32_t ts) {
    uint8_t payload[kPayload] = {0};
    for (int i = 0; i < pkts_per_frame(); i++)
      ut20_feed_st22_pkt(ctx_, static_cast<uint32_t>(frame_no * pkts_per_frame() + i), ts,
                         static_cast<uint32_t>(i), i == pkts_per_frame() - 1, payload,
                         sizeof(payload), MTL_SESSION_PORT_P);
  }
};

TEST_F(St22RxSinglePortClockStepTest, SteppedBackSenderResyncs) {
  feed_st22_frame(0, now_ts(0));
  feed_st22_frame(1, now_ts(1));
  ASSERT_EQ(ut20_st22_frames_ready(ctx_), 2u);

  for (int k = 0; k < kSteppedFrames; k++) feed_st22_frame(2 + k, stepped_ts(k));

  EXPECT_EQ(ut20_st22_frames_ready(ctx_), static_cast<uint64_t>(2 + kResyncedFrames));
}
