/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-frame corruption status: a post-redundancy session_seq_id gap charged
 * to the frame under assembly must close that frame as
 * ST_FRAME_STATUS_CORRUPTED, while clean / reordered / redundancy-recovered
 * streams must stay ST_FRAME_STATUS_COMPLETE (no false positive).
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxCorruptionTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"
#include "st_api.h"

class St30RxCorruptionTest : public St30RxBaseTest {
 protected:
  void use_single_port() {
    ut30_ctx_destroy(ctx_);
    ctx_ = ut30_ctx_create(1);
    ASSERT_NE(ctx_, nullptr);
  }

  uint64_t complete() {
    return ut30_frames_complete(ctx_);
  }
  uint64_t corrupted() {
    return ut30_frames_corrupted(ctx_);
  }
  int last_status() {
    return ut30_last_frame_status(ctx_);
  }
};

/* (a) Clean contiguous stream: every frame closes COMPLETE, none corrupted. */
TEST_F(St30RxCorruptionTest, CleanStreamNoFalseCorruption) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(complete(), 3u);
  EXPECT_EQ(corrupted(), 0u);
  EXPECT_EQ(last_status(), ST_FRAME_STATUS_COMPLETE);
}

/* (b) Single mid-frame gap: the frame absorbing the gap closes CORRUPTED. */
TEST_F(St30RxCorruptionTest, MidFrameGapCorruptsFrame) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  int fed = 0;
  /* feed n accepted packets but skip seq 5 mid-frame */
  while (fed < n) {
    if (seq == 5) {
      seq++; /* drop seq 5 */
      continue;
    }
    feed(seq++, ts++, MTL_SESSION_PORT_P);
    fed++;
  }

  EXPECT_EQ(corrupted(), 1u);
  EXPECT_EQ(complete(), 0u);
  EXPECT_EQ(last_status(), ST_FRAME_STATUS_CORRUPTED);
}

/* (c) Boundary straddle: the gap surfaces on the packet that opens the next
 * frame, so the NEW frame is CORRUPTED while the PRIOR frame stays COMPLETE. */
TEST_F(St30RxCorruptionTest, BoundaryStraddleCorruptsNewFrameOnly) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;

  /* frame 1: clean */
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);
  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);

  /* frame 2: first packet skips one seq, revealing the gap */
  seq++; /* drop the seq that would open frame 2 contiguously */
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u) << "prior frame must stay COMPLETE";
  EXPECT_EQ(corrupted(), 1u) << "new frame must be CORRUPTED";
}

/* (d) Full-frame loss: a whole frame's worth of contiguous seq is dropped.
 * The lost frame is absorbed (never emitted COMPLETE); exactly one CORRUPTED
 * frame carries the gap. */
TEST_F(St30RxCorruptionTest, FullFrameLossEmitsOneCorrupted) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;

  /* frame 1: clean */
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);

  /* drop a whole frame's worth of contiguous seq */
  seq += n;

  /* next accepted frame carries the entire gap */
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u) << "no frame falsely COMPLETE";
  EXPECT_EQ(corrupted(), 1u) << "exactly one CORRUPTED frame";
}

/* (e) Reorder / same-seq duplicate that never advances session_seq_id must
 * not raise a false gap, nor double-count corruption. */
TEST_F(St30RxCorruptionTest, ReorderDoesNotFalselyCorrupt) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  int fed;

  feed(seq++, ts++, MTL_SESSION_PORT_P); /* 0 */
  feed(seq++, ts++, MTL_SESSION_PORT_P); /* 1 */
  feed(seq++, ts++, MTL_SESSION_PORT_P); /* 2 */
  feed(seq++, ts++, MTL_SESSION_PORT_P); /* 3 */
  feed(1, ts++, MTL_SESSION_PORT_P);     /* backward seq, fresh ts */
  fed = 5;
  while (fed < n) {
    feed(seq++, ts++, MTL_SESSION_PORT_P);
    fed++;
  }

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (f) First packet of the session seeds session_seq_id, so a non-zero start
 * seq must not register a false opening gap. */
TEST_F(St30RxCorruptionTest, FirstPacketSeedingNoFalseGap) {
  use_single_port();
  int n = ppf();
  uint16_t seq = 1000;
  uint32_t ts = 1000;
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (f) Session reset clears the loss accumulator: a gap charged before reset
 * must not leak corruption into a clean frame fed afterwards. */
TEST_F(St30RxCorruptionTest, ResetClearsAccumulator) {
  use_single_port();
  uint32_t ts = 1000;

  /* partial frame with a gap, never closed */
  feed(0, ts++, MTL_SESSION_PORT_P);
  feed(5, ts++, MTL_SESSION_PORT_P); /* gap, accumulator > 0 */

  ut30_reset(ctx_);

  /* clean full frame after reset */
  int n = ppf();
  uint16_t seq = 0;
  for (int i = 0; i < n; i++) feed(seq++, ts++, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (g) Redundancy: a per-port loss covered by the other port leaves no session
 * gap, so the frame stays COMPLETE. */
TEST_F(St30RxCorruptionTest, RedundancyCoveredLossComplete) {
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  for (int i = 0; i < n; i++) {
    /* seq 5 arrives only on R, every other seq only on P */
    enum mtl_session_port port = (seq == 5) ? MTL_SESSION_PORT_R : MTL_SESSION_PORT_P;
    feed(seq++, ts++, port);
  }

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (g) Redundancy: loss on both ports leaves a session gap, so the frame
 * closes CORRUPTED. */
TEST_F(St30RxCorruptionTest, RedundancyBothPortsLostCorrupted) {
  int n = ppf();
  uint16_t seq = 0;
  uint32_t ts = 1000;
  int fed = 0;
  while (fed < n) {
    if (seq == 5) { /* dropped on both ports */
      seq++;
      continue;
    }
    feed(seq++, ts++, MTL_SESSION_PORT_P);
    fed++;
  }

  EXPECT_EQ(corrupted(), 1u);
  EXPECT_EQ(complete(), 0u);
}
