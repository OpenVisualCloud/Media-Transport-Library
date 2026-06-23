/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Per-frame bitmap stress (ST_RX_ANC_BITMAP_BITS = 64):
 * exact-fit, oversize fallback, cross-port interleave, prev-window late
 * marker, and reset.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxBitmapTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxBitmapTest : public St40RxBaseTest {};

/* Bitmap capacity boundary: a frame whose packet count exactly equals the
 * bitmap width must be tracked end-to-end with no off-by-one at offset 63.
 * A full replay on R must be classified entirely as redundant. */
TEST_F(St40RxBitmapTest, BitmapExactlyFull) {
  constexpr int N = 64; /* == ST_RX_ANC_BITMAP_BITS */
  constexpr uint32_t ts = 1000;

  feed_burst(0, N, ts, true, MTL_SESSION_PORT_P);
  /* R replays the same frame: every packet is a duplicate. */
  feed_burst(0, N, ts, true, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), static_cast<uint64_t>(N));
  EXPECT_EQ(redundant(), static_cast<uint64_t>(N));
  EXPECT_EQ(unrecovered(), 0u);
}

/* Frame larger than the bitmap: packets at offsets >= ST_RX_ANC_BITMAP_BITS
 * fall back to watermark-only filtering. Verify the frame is fully accepted
 * and that duplicates of an in-bitmap seq are still filtered as redundant. */
TEST_F(St40RxBitmapTest, BitmapOversizeFrameFallback) {
  constexpr int N = 80; /* exceeds bitmap width */
  constexpr uint32_t ts = 1000;

  feed_burst(0, N, ts, true, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), static_cast<uint64_t>(N));
  EXPECT_EQ(unrecovered(), 0u);

  /* A duplicate within the bitmap range must still be filtered. */
  feed(10, ts, false, MTL_SESSION_PORT_R);
  EXPECT_EQ(redundant(), 1u);
}

/* Worst-case cross-port interleave on a single frame: P delivers all even
 * seq, then R delivers all odd seq with the same timestamp. The bitmap
 * must accept every odd seq from R as a fill rather than as a duplicate. */
TEST_F(St40RxBitmapTest, BitmapCrossPortInterleave) {
  constexpr int N = 32; /* 32 evens + 32 odds = 64 packets */
  constexpr uint32_t ts = 2000;

  for (int i = 0; i < N; i++)
    feed(static_cast<uint16_t>(2 * i), ts, false, MTL_SESSION_PORT_P);
  for (int i = 0; i < N; i++) {
    bool last = (i == N - 1);
    feed(static_cast<uint16_t>(2 * i + 1), ts, last, MTL_SESSION_PORT_R);
  }

  EXPECT_EQ(redundant(), 0u)
      << "Odd-seq packets from R should fill bitmap holes, not be redundant";
  EXPECT_EQ(unrecovered(), 0u);
}

/* Late marker arriving on the redundant port after P has already advanced
 * to the next frame must be accepted via the previous-frame window and
 * decrement stat_pkts_unrecovered exactly once. */
TEST_F(St40RxBitmapTest, BitmapPrevWindowLateMarker) {
  constexpr uint32_t ts_n = 5000;
  constexpr uint32_t ts_n1 = 6500;

  /* Frame N on P delivers seq 0..4 with no marker; seq 5 (the marker) is
   * missing from P. */
  feed_burst(0, 5, ts_n, false, MTL_SESSION_PORT_P);

  /* P advances to frame N+1 without the missing seq, opening a forward gap
   * that increments stat_pkts_unrecovered. */
  feed_burst(6, 4, ts_n1, true, MTL_SESSION_PORT_P);
  uint64_t unrec_before_late = unrecovered();
  ASSERT_GE(unrec_before_late, 1u);

  /* R now delivers the late marker for frame N. */
  int rc = feed(5, ts_n, true, MTL_SESSION_PORT_R);
  EXPECT_EQ(rc, 0) << "Late prev-frame marker must be accepted via prev window";
  EXPECT_EQ(unrecovered(), unrec_before_late - 1)
      << "Accepting the late marker must decrement stat_pkts_unrecovered exactly once";
}

/* rx_ancillary_session_reset must fully clear both per-frame bitmap windows
 * (cur and prev). If reset leaves anc_window_cur populated, a subsequent
 * stream that happens to reuse the same timestamp can hit the late-accept
 * branch against a stale bitmap and deliver a duplicate to the application.
 *
 * Scenario:
 *   1. Stream 1 sends seq 0..5 at ts=1000, populating anc_window_cur.
 *   2. Session is reset.
 *   3. Stream 2 sends seq 7 at the same ts=1000; this becomes the new
 *      session head.
 *   4. seq 6 arrives on R. With reset working correctly, anc_window_cur is
 *      empty so the late-accept check fails and the packet is classified
 *      redundant. */
TEST_F(St40RxBitmapTest, BitmapResetClearsState) {
  feed_burst(0, 6, 1000, true, MTL_SESSION_PORT_P);
  ASSERT_EQ(received(), 6u);

  ut40_session_reset(ctx_);
  ASSERT_EQ(received(), 0u);
  ASSERT_EQ(redundant(), 0u);

  feed(7, 1000, false, MTL_SESSION_PORT_P);
  ASSERT_EQ(received(), 1u);

  feed(6, 1000, false, MTL_SESSION_PORT_R);

  EXPECT_EQ(received(), 1u)
      << "seq 6 on R must be classified redundant, not accepted as a late arrival";
  EXPECT_EQ(redundant(), 1u);
}

/* anc_window_cur.base_seq is uint16_t and the bitmap offset is computed as
 * `(uint16_t)(seq - base_seq)`. When the frame's first accepted seq is
 * near the 16-bit boundary (e.g. 65534), in-frame packets at 65535 and 0
 * must produce offsets 1 and 2 via modular wrap, NOT trip the
 * `offset >= ST_RX_ANC_BITMAP_BITS` OOB guard. */
TEST_F(St40RxBitmapTest, BitmapOffsetWrapsAtBaseSeq) {
  constexpr uint32_t ts = 1000;

  /* seed session_seq_id so the next frame's base_seq is 65534 */
  feed(65533, 999, true, MTL_SESSION_PORT_P);
  ASSERT_EQ(session_seq(), 65533);

  /* one frame (single ts) spanning the wrap: 65534, 65535, 0, 1 (marker) */
  feed(65534, ts, false, MTL_SESSION_PORT_P);
  feed(65535, ts, false, MTL_SESSION_PORT_P);
  feed(0, ts, false, MTL_SESSION_PORT_P);
  feed(1, ts, true, MTL_SESSION_PORT_P);

  /* All four must be received, no duplicate/redundant trigger from a wrap-broken
   * offset, no unrecovered phantom from a wrap-broken gap. */
  EXPECT_EQ(received(), 5u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(unrecovered(), 0u);
}
