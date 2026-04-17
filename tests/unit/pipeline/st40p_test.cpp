/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Unit tests for ST2110-40 (ancillary) RX pipeline — frame assembly layer.
 * Tests rx_st40p_rtp_ready() which sits above the session-layer redundancy
 * filter and assembles RTP packets into frames.
 */

#include <gtest/gtest.h>

#include "pipeline/st40p_harness.h"

/* ── fixture ───────────────────────────────────────────────────────── */

class St40PipelineRxTest : public ::testing::Test {
 protected:
  ut40p_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40p_init(), 0) << "EAL init failed";
    ctx_ = ut40p_ctx_create(2, 3); /* 2 ports, 3 frame buffers */
    ASSERT_NE(ctx_, nullptr);
  }

  void TearDown() override {
    ut40p_ctx_destroy(ctx_);
    ctx_ = nullptr;
  }

  /* convenience wrappers */
  int enqueue(uint16_t seq, uint32_t ts, bool marker, enum mtl_session_port port) {
    return ut40p_enqueue_pkt(ctx_, seq, ts, marker ? 1 : 0, port);
  }

  void enqueue_burst(uint16_t seq_start, int count, uint32_t ts, bool last_marker,
                     enum mtl_session_port port) {
    ut40p_enqueue_burst(ctx_, seq_start, count, ts, last_marker ? 1 : 0, port);
  }

  int process() {
    return ut40p_process(ctx_);
  }

  void process_all() {
    ut40p_process_all(ctx_);
  }

  struct st40_frame_info* get_frame() {
    return ut40p_get_frame(ctx_);
  }

  int put_frame(struct st40_frame_info* f) {
    return ut40p_put_frame(ctx_, f);
  }

  uint32_t stat_busy() {
    return ut40p_stat_busy(ctx_);
  }
};

/* ── Normal pipeline operation ────────────────────────────────────────── */

/* Single complete frame: 6 packets with marker on last. */
TEST_F(St40PipelineRxTest, SingleFrameCompletion) {
  enqueue_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 1000u);
  EXPECT_EQ(frame->pkts_total, 6u);
  EXPECT_TRUE(frame->rtp_marker);
  EXPECT_EQ(frame->status, ST_FRAME_STATUS_COMPLETE);
  put_frame(frame);
}

/* Timestamp change completes the previous frame without marker. */
TEST_F(St40PipelineRxTest, TimestampChangeCompletesFrame) {
  /* frame 1: 3 pkts, no marker */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  /* frame 2: 1 pkt with new ts → completes frame 1 */
  enqueue(3, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  auto* frame1 = get_frame();
  ASSERT_NE(frame1, nullptr);
  EXPECT_EQ(frame1->rtp_timestamp, 1000u);
  EXPECT_EQ(frame1->pkts_total, 3u);
  EXPECT_FALSE(frame1->rtp_marker)
      << "Frame completed by ts change should not have marker";
  put_frame(frame1);
}

/* Fill all frame buffers — additional packets should increment stat_busy. */
TEST_F(St40PipelineRxTest, FramebufferExhaustion) {
  /* 3 framebuffs: fill all with inflight frames via ts changes.
   * ts=1000 → ts=2000 (completes frame 1, starts frame 2)
   * ts=2000 → ts=3000 (completes frame 2, starts frame 3)
   * All 3 frames are READY or RECEIVING; no free buffers. */
  enqueue(0, 1000, false, MTL_SESSION_PORT_P);
  enqueue(1, 2000, false, MTL_SESSION_PORT_P); /* completes ts=1000 */
  enqueue(2, 3000, false, MTL_SESSION_PORT_P); /* completes ts=2000 */
  process_all();

  /* frames 1 and 2 are READY, frame 3 is RECEIVING (inflight).
   * No free framebuffs. Next ts change will try to allocate but fail. */
  enqueue(3, 4000, false, MTL_SESSION_PORT_P); /* completes ts=3000 → no free fb */
  process_all();

  EXPECT_GE(stat_busy(), 1u) << "Should hit framebuffer exhaustion";
}

/* Sequence discontinuity tracking: gap between seq 1 and 3. */
TEST_F(St40PipelineRxTest, SequenceDiscontinuity) {
  enqueue(0, 1000, false, MTL_SESSION_PORT_P);
  enqueue(1, 1000, false, MTL_SESSION_PORT_P);
  /* skip seq 2 */
  enqueue(3, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->seq_discont);
  EXPECT_EQ(frame->seq_lost, 1u);
  put_frame(frame);
}

/* Per-port sequence tracking: P has gap, R does not. */
TEST_F(St40PipelineRxTest, PerPortSequenceTracking) {
  enqueue(0, 1000, false, MTL_SESSION_PORT_P);
  /* P skips seq 1 */
  enqueue(2, 1000, false, MTL_SESSION_PORT_P);
  /* R fills in seq 1 */
  enqueue(1, 1000, false, MTL_SESSION_PORT_R);
  enqueue(3, 1000, true, MTL_SESSION_PORT_R);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->port_seq_discont[MTL_SESSION_PORT_P]);
  EXPECT_EQ(frame->port_seq_lost[MTL_SESSION_PORT_P], 1u);
  /* R sees seq 1 then 3 — gap of 1 too (since per-port tracks independently) */
  EXPECT_TRUE(frame->port_seq_discont[MTL_SESSION_PORT_R]);
  EXPECT_EQ(frame->pkts_recv[MTL_SESSION_PORT_P], 2u);
  EXPECT_EQ(frame->pkts_recv[MTL_SESSION_PORT_R], 2u);
  put_frame(frame);
}

/* Multiple frames delivered in sequence. */
TEST_F(St40PipelineRxTest, MultiFrameDelivery) {
  for (uint32_t ts = 1000; ts <= 3000; ts += 1000) {
    enqueue_burst(0 + (ts - 1000) / 1000 * 4, 4, ts, true, MTL_SESSION_PORT_P);
  }
  process_all();

  for (uint32_t ts = 1000; ts <= 3000; ts += 1000) {
    auto* frame = get_frame();
    ASSERT_NE(frame, nullptr) << "Frame ts=" << ts << " should be available";
    EXPECT_EQ(frame->rtp_timestamp, ts);
    EXPECT_TRUE(frame->rtp_marker);
    put_frame(frame);
  }

  /* no more frames */
  EXPECT_EQ(get_frame(), nullptr);
}

/* Frame put recycles the buffer for reuse. */
TEST_F(St40PipelineRxTest, FramePutRecyclesBuffer) {
  /* fill and consume one frame */
  enqueue_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  process_all();
  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  put_frame(frame);

  /* the returned slot should be reusable — send another frame */
  enqueue_burst(4, 4, 2000, true, MTL_SESSION_PORT_P);
  process_all();
  frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 2000u);
  put_frame(frame);
}

/* Packet from unmapped port is dropped. */
TEST_F(St40PipelineRxTest, UnmappedPortDropped) {
  /* Enqueue a packet with port_id=99 (not mapped to any session port) */
  ASSERT_EQ(ut40p_enqueue_pkt_port_id(ctx_, 0, 1000, 1, 99), 0);
  int rc = process();
  EXPECT_EQ(rc, -EIO);
  EXPECT_EQ(get_frame(), nullptr) << "Unmapped port packet should not produce a frame";
}

/* ── RTP marker bit tests ─────────────────────────────────────────────── */

/* Marker on last packet of a single-port frame. */
TEST_F(St40PipelineRxTest, MarkerPreservedSingleFrame) {
  enqueue_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->rtp_marker) << "Marker must survive into frame_info";
  put_frame(frame);
}

/* Pcap scenario: P sends body, R sends tail with marker, all same timestamp.
 * This matches the user's pcap (hitless_merge_anc.pcap, frame ts=0xef6fef62):
 *   P sends seq 0-4 (no marker), R sends seq 5-6 (marker on 6).
 * At the pipeline layer, both ports' packets arrive via the same packet_ring
 * (session layer already deduplicated). Marker must survive. */
TEST_F(St40PipelineRxTest, MarkerPreservedMidFrameSwitchover) {
  /* P sends body: seq 0-4, no marker */
  for (int i = 0; i < 5; i++) enqueue(i, 1000, false, MTL_SESSION_PORT_P);
  /* R sends tail: seq 5-6, marker on 6 */
  enqueue(5, 1000, false, MTL_SESSION_PORT_R);
  enqueue(6, 1000, true, MTL_SESSION_PORT_R);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 1000u);
  EXPECT_EQ(frame->pkts_total, 7u);
  EXPECT_TRUE(frame->rtp_marker)
      << "Marker from R port must survive mid-frame switchover at pipeline layer";
  put_frame(frame);
}

/* Marker preservation when timestamp advance completes frame before marker.
 *
 * Scenario:
 *   1. P delivers seq 0-4 (ts=1000, no marker) — inflight frame for ts=1000
 *   2. P delivers seq 7 (ts=2000) — timestamp change completes ts=1000
 *   3. R delivers seq 5-6 (ts=1000, marker on 6) — late marker
 *
 * With the PENDING state, step 2 puts ts=1000 into PENDING (not READY).
 * Step 3's late marker resolves the PENDING frame. */
TEST_F(St40PipelineRxTest, DISABLED_MarkerPreservedOnTimestampAdvance) {
  /* Step 1: P delivers frame body */
  for (int i = 0; i < 5; i++) enqueue(i, 1000, false, MTL_SESSION_PORT_P);

  /* Step 2: P starts next frame, triggering ts change */
  enqueue(7, 2000, false, MTL_SESSION_PORT_P);

  /* Step 3: R delivers late marker for ts=1000 */
  enqueue(5, 1000, false, MTL_SESSION_PORT_R);
  enqueue(6, 1000, true, MTL_SESSION_PORT_R); /* MARKER */

  process_all();

  /* Get the ts=1000 frame (completed by timestamp advance) */
  auto* frame1 = get_frame();
  ASSERT_NE(frame1, nullptr);
  EXPECT_EQ(frame1->rtp_timestamp, 1000u);

  /* The marker from R must be preserved in the delivered frame. */
  EXPECT_TRUE(frame1->rtp_marker)
      << "Marker from R must be preserved when P advances to next frame first";
  put_frame(frame1);
}

/* Marker preservation on framebuffer exhaustion.
 *
 * When all framebuffers are occupied and a new timestamp arrives, the pipeline
 * must still process the marker correctly when framebuffers become available. */
TEST_F(St40PipelineRxTest, MarkerPreservedOnFramebufferExhaustion) {
  /* Use ctx with only 2 framebuffs to make exhaustion easier */
  ut40p_ctx_destroy(ctx_);
  ctx_ = ut40p_ctx_create(2, 2);
  ASSERT_NE(ctx_, nullptr);

  /* Frame 1: ts=1000, 3 pkts, no marker */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  /* Frame 2: ts=2000 → completes frame 1, starts frame 2 */
  enqueue(3, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  /* Now: frame 1 is READY (slot 0), frame 2 is RECEIVING (slot 1).
   * Both framebuffers are occupied. */

  /* Frame 3: ts=3000 → completes frame 2, but no free fb for frame 3 */
  enqueue(4, 3000, false, MTL_SESSION_PORT_P);
  process_all();

  /* Frame 2 is now READY (slot 1). Frame 3 hit stat_busy.
   * The ts=3000 packet: timestamp change completed frame 2 (READY),
   * then tried to allocate new fb — none free → goto out.
   * The marker check at line 315 was never reached for this packet. */

  /* Now send a marker for ts=3000 — it also can't allocate a new fb */
  enqueue(5, 3000, true, MTL_SESSION_PORT_P); /* MARKER */
  process_all();

  EXPECT_GE(stat_busy(), 1u);

  /* Frame 1 does not have marker (completed by ts change — expected) */
  auto* frame1 = get_frame();
  ASSERT_NE(frame1, nullptr);
  EXPECT_EQ(frame1->rtp_timestamp, 1000u);
  EXPECT_FALSE(frame1->rtp_marker); /* no marker in frame 1 — correct */
  put_frame(frame1);

  /* Frame 2 does not have marker either (completed by ts change — expected) */
  auto* frame2 = get_frame();
  ASSERT_NE(frame2, nullptr);
  EXPECT_EQ(frame2->rtp_timestamp, 2000u);
  EXPECT_FALSE(frame2->rtp_marker); /* no marker in frame 2 — correct */
  put_frame(frame2);

  /* Frame 3 was never assembled (all its packets hit stat_busy).
   * Put frames 1 and 2 to free slots, then retry frame 3. */
  enqueue(10, 3000, false, MTL_SESSION_PORT_P);
  enqueue(11, 3000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* frame3 = get_frame();
  ASSERT_NE(frame3, nullptr);
  EXPECT_EQ(frame3->rtp_timestamp, 3000u);
  EXPECT_TRUE(frame3->rtp_marker) << "Frame 3 should have marker after retry";
  put_frame(frame3);
}

/* Marker absent when no packet has it set. */
TEST_F(St40PipelineRxTest, MarkerAbsentWhenNotSet) {
  /* 3 pkts, no marker. Complete frame via timestamp change. */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  enqueue(3, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(frame->rtp_marker);
  put_frame(frame);
}

/* Single-packet frame with marker. */
TEST_F(St40PipelineRxTest, MarkerOnSinglePacketFrame) {
  enqueue(0, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->pkts_total, 1u);
  EXPECT_TRUE(frame->rtp_marker);
  put_frame(frame);
}

/* Frame status is COMPLETE when no seq discontinuity, CORRUPTED when there is. */
TEST_F(St40PipelineRxTest, FrameStatusCompleteVsCorrupted) {
  /* Clean frame */
  enqueue_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  auto* clean = get_frame();
  ASSERT_NE(clean, nullptr);
  EXPECT_EQ(clean->status, ST_FRAME_STATUS_COMPLETE);
  put_frame(clean);

  /* Corrupted frame (seq gap) */
  enqueue(4, 2000, false, MTL_SESSION_PORT_P);
  enqueue(6, 2000, true, MTL_SESSION_PORT_P); /* skip seq 5 */
  process_all();

  auto* corrupted = get_frame();
  ASSERT_NE(corrupted, nullptr);
  EXPECT_EQ(corrupted->status, ST_FRAME_STATUS_CORRUPTED);
  put_frame(corrupted);
}

/* ── PENDING state: late-marker resolution ───────────────────────────── */
/*
 * Marker-primary frame assembly with N-1 fallback:
 *
 *   - Marker received → frame immediately READY (zero added latency).
 *   - Timestamp change without marker → old frame enters PENDING (not READY).
 *     PENDING frame is not visible to consumer via get_frame().
 *   - Late marker for PENDING frame → apply marker, promote to READY.
 *   - Second timestamp change → force-deliver PENDING frame without marker.
 *   - At most 2 frames buffered (inflight + pending), bounded memory cost.
 */

/* When P advances timestamp before R's marker, the old frame enters PENDING.
 * It must NOT be visible via get_frame() until resolved. */
TEST_F(St40PipelineRxTest, DISABLED_PendingFrameNotVisibleBeforeResolution) {
  /* Frame body: 4 pkts for ts=1000, no marker */
  enqueue_burst(0, 4, 1000, false, MTL_SESSION_PORT_P);
  /* P starts next frame → ts=1000 enters PENDING */
  enqueue(4, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  /* PENDING frame should not be delivered yet */
  EXPECT_EQ(get_frame(), nullptr)
      << "PENDING frame must not be visible to consumer before resolution";

  /* Resolve: send marker for ts=2000 to make it READY, which also
   * force-delivers the PENDING ts=1000 frame. */
  enqueue(5, 3000, false, MTL_SESSION_PORT_P); /* second ts change → force-deliver */
  process_all();

  auto* frame1 = get_frame();
  ASSERT_NE(frame1, nullptr);
  EXPECT_EQ(frame1->rtp_timestamp, 1000u);
  EXPECT_FALSE(frame1->rtp_marker) << "Force-delivered PENDING frame has no marker";
  put_frame(frame1);
}

/* Late marker from R resolves a PENDING frame: the frame becomes READY
 * with rtp_marker=true and the correct total packet count. */
TEST_F(St40PipelineRxTest, DISABLED_PendingResolvedByLateMarker) {
  /* P: 3 pkts for ts=1000, no marker */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  /* P advances to ts=2000 → ts=1000 enters PENDING */
  enqueue(5, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  /* Verify frame not yet visible */
  EXPECT_EQ(get_frame(), nullptr);

  /* R: late packets for ts=1000, marker on last */
  enqueue(3, 1000, false, MTL_SESSION_PORT_R);
  enqueue(4, 1000, true, MTL_SESSION_PORT_R); /* MARKER */
  process_all();

  /* PENDING frame resolved by marker → now READY */
  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 1000u);
  EXPECT_TRUE(frame->rtp_marker) << "Late marker must resolve PENDING frame";
  EXPECT_EQ(frame->pkts_total, 5u) << "Late packets must be counted in frame";
  put_frame(frame);
}

/* Second timestamp change force-delivers a PENDING frame without marker.
 *
 * Sequence: ts=1000 (no marker) → ts=2000 (ts=1000 PENDING) →
 *           ts=3000 (ts=1000 force-READY, ts=2000 PENDING) */
TEST_F(St40PipelineRxTest, DISABLED_PendingForcedBySecondTimestampChange) {
  /* Frame 1: ts=1000, 3 pkts, no marker */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  /* ts change → ts=1000 PENDING */
  enqueue(3, 2000, false, MTL_SESSION_PORT_P);
  process_all();
  EXPECT_EQ(get_frame(), nullptr) << "ts=1000 should be PENDING, not READY";

  /* Frame 2: ts=2000, 2 pkts, no marker */
  enqueue(4, 2000, false, MTL_SESSION_PORT_P);
  /* Second ts change → ts=1000 force-delivered, ts=2000 PENDING */
  enqueue(5, 3000, false, MTL_SESSION_PORT_P);
  process_all();

  auto* frame1 = get_frame();
  ASSERT_NE(frame1, nullptr);
  EXPECT_EQ(frame1->rtp_timestamp, 1000u);
  EXPECT_EQ(frame1->pkts_total, 3u);
  EXPECT_FALSE(frame1->rtp_marker) << "Force-delivered frame should not have marker";
  put_frame(frame1);

  /* ts=2000 should still be PENDING */
  EXPECT_EQ(get_frame(), nullptr) << "ts=2000 should be PENDING, not yet READY";
}

/* Normal marker path: frame with marker goes directly to READY — no PENDING.
 * This must continue working unchanged with the PENDING state logic. */
TEST_F(St40PipelineRxTest, NoPendingWhenMarkerPresent) {
  /* Frame with marker on last packet */
  enqueue_burst(0, 4, 1000, true, MTL_SESSION_PORT_P);
  process_all();

  /* Frame should be immediately available — no PENDING state */
  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 1000u);
  EXPECT_TRUE(frame->rtp_marker);
  EXPECT_EQ(frame->pkts_total, 4u);
  put_frame(frame);

  /* Second frame with marker — also immediate */
  enqueue_burst(4, 3, 2000, true, MTL_SESSION_PORT_P);
  process_all();

  frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 2000u);
  EXPECT_TRUE(frame->rtp_marker);
  put_frame(frame);
}

/* A PENDING frame accumulates late packets from R, including their count.
 * The marker resolves it and the total reflects all received packets. */
TEST_F(St40PipelineRxTest, DISABLED_PendingLatePacketsAccumulate) {
  /* P: 3 pkts for ts=1000, no marker */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  /* P advances to ts=2000 → ts=1000 PENDING */
  enqueue(6, 2000, false, MTL_SESSION_PORT_P);
  process_all();

  /* R: 3 late pkts for ts=1000 (seq 3,4,5), marker on seq 5 */
  enqueue(3, 1000, false, MTL_SESSION_PORT_R);
  enqueue(4, 1000, false, MTL_SESSION_PORT_R);
  enqueue(5, 1000, true, MTL_SESSION_PORT_R); /* MARKER */
  process_all();

  auto* frame = get_frame();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->rtp_timestamp, 1000u);
  EXPECT_TRUE(frame->rtp_marker);
  EXPECT_EQ(frame->pkts_total, 6u) << "3 from P + 3 from R";
  EXPECT_EQ(frame->pkts_recv[MTL_SESSION_PORT_P], 3u);
  EXPECT_EQ(frame->pkts_recv[MTL_SESSION_PORT_R], 3u);
  put_frame(frame);
}

/* Multiple successive frames cycling through PENDING → resolved.
 * Each frame's marker arrives late from R after P has advanced. */
TEST_F(St40PipelineRxTest, DISABLED_PendingMultiFrameCycle) {
  /* Frame 1: P sends body, advances to ts=2000 */
  enqueue_burst(0, 3, 1000, false, MTL_SESSION_PORT_P);
  enqueue(3, 2000, false, MTL_SESSION_PORT_P); /* ts=1000 → PENDING */
  /* R sends late marker for ts=1000 → resolves PENDING */
  enqueue(3, 1000, true, MTL_SESSION_PORT_R);

  /* Frame 2: P sends body at ts=2000, advances to ts=3000 */
  enqueue(4, 2000, false, MTL_SESSION_PORT_P);
  enqueue(5, 3000, false, MTL_SESSION_PORT_P); /* ts=2000 → PENDING */
  /* R sends late marker for ts=2000 */
  enqueue(5, 2000, true, MTL_SESSION_PORT_R);

  /* Frame 3: complete with marker */
  enqueue(6, 3000, true, MTL_SESSION_PORT_P); /* ts=3000 → READY via marker */

  process_all();

  auto* f1 = get_frame();
  ASSERT_NE(f1, nullptr);
  EXPECT_EQ(f1->rtp_timestamp, 1000u);
  EXPECT_TRUE(f1->rtp_marker) << "Frame 1: late marker from R resolved PENDING";
  put_frame(f1);

  auto* f2 = get_frame();
  ASSERT_NE(f2, nullptr);
  EXPECT_EQ(f2->rtp_timestamp, 2000u);
  EXPECT_TRUE(f2->rtp_marker) << "Frame 2: late marker from R resolved PENDING";
  put_frame(f2);

  auto* f3 = get_frame();
  ASSERT_NE(f3, nullptr);
  EXPECT_EQ(f3->rtp_timestamp, 3000u);
  EXPECT_TRUE(f3->rtp_marker) << "Frame 3: marker on-time, no PENDING";
  put_frame(f3);

  EXPECT_EQ(get_frame(), nullptr);
}
