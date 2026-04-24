/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * RTP marker bit preservation across the redundancy filter:
 * single port, mid-frame switchover, cross-port reorder, threshold bypass
 * and the bitmap late-arrival path.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxMarkerTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxMarkerTest : public St40RxBaseTest {};

/* Marker preservation when P advances timestamp before R delivers marker.
 *
 * Scenario:
 *   1. P delivers frame N body: seq 0-4, ts=1000, no marker
 *   2. P delivers frame N+1 first packet: seq 7, ts=2000 — tmstamp advances
 *   3. R delivers frame N tail: seq 5-6, ts=1000, marker on seq 6
 *
 * The redundancy filter must accept R's late packets for the immediately
 * previous timestamp and preserve the marker bit through to the ring. */
TEST_F(St40RxMarkerTest, MarkerPreservedAfterTimestampAdvance) {
  ut40_drain_paused _drain_guard;
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
}

/* Marker bit on the last packet of a single-port burst survives into the ring. */
TEST_F(St40RxMarkerTest, MarkerPreservedSinglePort) {
  ut40_drain_paused _drain_guard;
  /* 6 packets, marker on last */
  feed_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_TRUE(has_marker) << "Marker bit must survive into the ring";
}

/* Mid-frame port switchover: P sends the body, R sends the tail including marker.
 * A complete prior frame establishes session history, then P sends seq 7-11
 * and R sends seq 12-13 with marker on seq 13. The marker-bearing packet from R
 * must pass the redundancy filter and be enqueued with the marker bit intact. */
TEST_F(St40RxMarkerTest, MarkerPreservedMidFrameSwitchover) {
  /* Complete prior frame on P (7 packets) to establish session history */
  feed_burst(0, 7, 1000, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_drain_paused _drain_guard;

  uint32_t ts = 2000;
  /* P sends body: seq 7-11, no marker */
  for (int i = 7; i <= 11; i++) feed(i, ts, false, MTL_SESSION_PORT_P);
  /* R sends tail: seq 12-13, marker on seq 13 */
  feed(12, ts, false, MTL_SESSION_PORT_R);
  feed(13, ts, true, MTL_SESSION_PORT_R);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 7);
  EXPECT_TRUE(has_marker) << "Marker from R port must survive mid-frame switchover";
}

/* Cross-port reorder with bitmap: R delivers seq 7-11 (marker on 11) before
 * P's late-arriving seq 5-6.  A warm-up frame establishes session_seq so the
 * bitmap base covers the late arrivals.  The marker from R must survive. */
TEST_F(St40RxMarkerTest, MarkerPreservedCrossPortReorder) {
  /* Previous frame to set session_seq_id=4, so bitmap base for next frame = 5 */
  feed_burst(0, 5, 2999, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_drain_paused _drain_guard;

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
}

/* Negative test: no marker set on any packet, verify has_marker is false.
 * Guards against false positives in the test infrastructure. */
TEST_F(St40RxMarkerTest, MarkerAbsentWhenNotSet) {
  ut40_drain_paused _drain_guard;
  /* 6 packets, NO marker on any */
  feed_burst(0, 6, 1000, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_FALSE(has_marker) << "No packet had marker=1, has_marker must be false";
}

/* Marker on the first packet (not last) of a frame.  Verifies position-
 * independence: the handler must never strip the marker regardless of
 * where it appears in the sequence. */
TEST_F(St40RxMarkerTest, MarkerOnFirstPacket) {
  ut40_drain_paused _drain_guard;
  uint32_t ts = 4000;
  /* First packet carries the marker */
  feed(0, ts, true, MTL_SESSION_PORT_P);
  for (int i = 1; i < 6; i++) feed(i, ts, false, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 6);
  EXPECT_TRUE(has_marker) << "Marker on first packet must survive into ring";
}

/* Late arrival via bitmap carries the marker.  R advances the session seq,
 * then P's late packet (carrying the marker) is accepted through the bitmap
 * path (goto accept_pkt).  The marker must survive this alternate code path. */
TEST_F(St40RxMarkerTest, MarkerOnBitmapLateArrival) {
  /* Warm-up frame to establish session_seq = 4 */
  feed_burst(0, 5, 4999, true, MTL_SESSION_PORT_P);
  ut40_drain_ring();
  ut40_drain_paused _drain_guard;

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
}

/* Both P and R send the same marker packet (same seq, same ts, both with marker=1).
 * P's packet is accepted, R's duplicate is filtered as redundant.
 * The accepted packet must carry the marker into the ring. */
TEST_F(St40RxMarkerTest, MarkerDuplicateFromBothPorts) {
  ut40_drain_paused _drain_guard;
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
}

/* Marker on a packet accepted via the threshold bypass path.
 * After 20+ consecutive redundant rejections on ALL ports, the filter
 * force-accepts the next packet.  The marker must survive this path. */
TEST_F(St40RxMarkerTest, MarkerSurvivesThresholdBypass) {
  constexpr uint32_t ts_new = 6000;
  constexpr uint32_t ts_old = 5000;

  /* Establish state with newer timestamp */
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_P);
  feed_burst(0, 4, ts_new, true, MTL_SESSION_PORT_R);
  ut40_drain_ring();
  ut40_drain_paused _drain_guard;

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
}

/* Frame consisting of only a single marker packet (degenerate but
 * legal — e.g. an ANC frame with one ADF payload).  Must be delivered
 * cleanly to the ring with marker bit preserved. */
TEST_F(St40RxMarkerTest, SinglePacketFrameMarkerOnly) {
  ut40_drain_paused _drain_guard;
  feed(0, 1000, true, MTL_SESSION_PORT_P);

  int count = 0;
  bool has_marker = false;
  ASSERT_EQ(ut40_ring_dequeue_markers(&count, &has_marker), 0);
  EXPECT_EQ(count, 1);
  EXPECT_TRUE(has_marker);
  EXPECT_EQ(unrecovered(), 0u);
}
