/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Previous-timestamp acceptance window:
 * late packets for the immediately previous frame are accepted; packets
 * two frames back are rejected; bitmap dedup still applies.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxPrevWindowTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxPrevWindowTest : public St40RxBaseTest {};

/* After tmstamp advances from N to N+1, packets for timestamp N (the immediately
 * previous frame) must still be accepted by the redundancy filter.  This enables
 * the pipeline layer to receive late-arriving packets from the redundant port
 * and resolve pending frames with the correct marker bit. */
TEST_F(St40RxPrevWindowTest, PrevTimestampPacketsAccepted) {
  ut40_drain_paused _drain_guard;
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
}

/* Packets two frames back (tmstamp - 2) must still be rejected.
 * Only the immediately previous timestamp gets the acceptance window. */
TEST_F(St40RxPrevWindowTest, TwoFramesBackRejected) {
  ut40_drain_paused _drain_guard;
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
}

/* Late prev_tmstamp duplicates (same seq already accepted from P) must
 * still be filtered as redundant.  The prev_tmstamp acceptance window
 * must not bypass the bitmap deduplication. */
TEST_F(St40RxPrevWindowTest, PrevTimestampDuplicatesFiltered) {
  ut40_drain_paused _drain_guard;
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
}

/* Port R lags by more than the prev_tmstamp window (1 frame).  Once R
 * catches up, its packets for the long-stale frame should be filtered
 * as redundant — neither delivered nor counted as unrecovered.  This
 * documents the current 1-frame acceptance window depth. */
TEST_F(St40RxPrevWindowTest, PortRLagsBeyondPrevTimestampWindow) {
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

/* The prev-frame acceptance window matches via
 *   `tmstamp == (uint32_t)s->prev_tmstamp`
 * which must work across the 32-bit RTP timestamp wrap. Frame N at
 * ts=0xFFFFFFF0 advances to frame N+1 at ts=0x00000010 (a real wrap). A
 * late marker for frame N on the redundant port must still be accepted
 * via the prev_window path; a wrap-broken comparison would reject it. */
TEST_F(St40RxPrevWindowTest, PrevTimestampWindowAcrossTsWrap) {
  constexpr uint32_t ts_n = 0xFFFFFFF0;
  constexpr uint32_t ts_n1 = 0x00000010; /* wraps over UINT32_MAX */

  /* frame N: only seqs 0 and 1 on P (no marker yet); seq 2 is missing */
  feed(0, ts_n, false, MTL_SESSION_PORT_P);
  feed(1, ts_n, false, MTL_SESSION_PORT_P);

  /* frame N+1 starts on P, advances session ts past the wrap */
  feed(3, ts_n1, false, MTL_SESSION_PORT_P);
  feed(4, ts_n1, true, MTL_SESSION_PORT_P);

  /* late marker for frame N (seq 2, never delivered) arrives on R — must be
   * accepted via prev_window despite prev_tmstamp having wrapped */
  uint64_t recv_before = received();
  int rc = feed(2, ts_n, true, MTL_SESSION_PORT_R);

  EXPECT_GE(rc, 0)
      << "late prev-frame marker across the 32-bit ts wrap must be accepted, not -EIO";
  EXPECT_EQ(received(), recv_before + 1)
      << "prev_window match must work when prev_tmstamp wrapped via UINT32_MAX";
}
