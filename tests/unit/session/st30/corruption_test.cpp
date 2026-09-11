/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Per-frame corruption status: a post-redundancy session_seq_id gap charged
 * to the frame under assembly must close that frame as
 * ST_FRAME_STATUS_CORRUPTED, while clean / reordered / redundancy-recovered
 * streams must stay ST_FRAME_STATUS_COMPLETE (no false positive). Plus the
 * delivery policy for such a frame: suppressed unless the app opts in with
 * ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME, and once delivered its missing slots
 * read as silence rather than as the recycled buffer's previous audio.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxCorruptionTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"
#include "st30_api.h"
#include "st_api.h"

class St30RxCorruptionTest : public St30RxBaseTest {
 protected:
  void use_single_port() {
    ut30_ctx_destroy(ctx_);
    ctx_ = ut30_ctx_create(1);
    ASSERT_NE(ctx_, nullptr);
    /* a partial frame only reaches the app when this flag is set */
    ut30_ctx_set_flags(ctx_, ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME);
  }

  /* Feed one frame's worth of packets, every payload byte set to `fill`. A
   * `skip_slot` of -1 feeds the whole frame. */
  void feed_frame(uint16_t* seq, uint32_t base, uint8_t fill, int skip_slot) {
    for (int i = 0; i < ppf(); i++) {
      if (i == skip_slot) continue;
      ut30_feed_pkt_fill(ctx_, (*seq)++, base + (uint32_t)i * spp(), MTL_SESSION_PORT_P,
                         fill);
    }
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

  ::testing::AssertionResult SlotIs(int i, int slot, uint8_t expect) {
    const uint8_t* p =
        (const uint8_t*)ut30_frame_log_addr(ctx_, i) + (size_t)slot * ut30_pkt_len(ctx_);
    for (uint32_t b = 0; b < ut30_pkt_len(ctx_); b++) {
      if (p[b] != expect)
        return ::testing::AssertionFailure()
               << "slot " << slot << " byte " << b << " is " << (unsigned)p[b] << " want "
               << (unsigned)expect;
    }
    return ::testing::AssertionSuccess();
  }
};

/* (a) Clean contiguous stream: every frame closes COMPLETE, none corrupted. */
TEST_F(St30RxCorruptionTest, CleanStreamNoFalseCorruption) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint16_t seq = 0;
  uint32_t base = 1000;
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
    base += (uint32_t)n * s;
  }

  EXPECT_EQ(complete(), 3u);
  EXPECT_EQ(corrupted(), 0u);
  EXPECT_EQ(last_status(), ST_FRAME_STATUS_COMPLETE);
}

/* Intra-frame reorder: packets arriving out of timestamp order are placed
 * positionally and the frame still closes COMPLETE. */
TEST_F(St30RxCorruptionTest, IntraFrameReorderCompletes) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  /* slot 0 establishes the frame base, then later slots arrive pair-swapped */
  feed(seq++, base + 0u * s, MTL_SESSION_PORT_P);
  for (int i = 1; i < n; i += 2) {
    int hi = (i + 1 < n) ? i + 1 : i;
    feed(seq++, base + (uint32_t)hi * s, MTL_SESSION_PORT_P);
    if (hi != i) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
  EXPECT_EQ(last_status(), ST_FRAME_STATUS_COMPLETE);
}

/* (b) Single mid-frame gap: the partial frame closes CORRUPTED when the next
 * frame's first packet forward-jumps the timestamp. */
TEST_F(St30RxCorruptionTest, MidFrameGapCorruptsFrame) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  for (int i = 0; i < n; i++) {
    if (i == 5) continue; /* drop slot 5 */
    feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }
  /* forward jump closes the partial frame CORRUPTED */
  feed(seq++, base + (uint32_t)n * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(corrupted(), 1u);
  EXPECT_EQ(complete(), 0u);
  EXPECT_EQ(last_status(), ST_FRAME_STATUS_CORRUPTED);
}

/* (c) The prior frame stays COMPLETE while a later partial frame closes
 * CORRUPTED on the forward jump that opens the frame after it. */
TEST_F(St30RxCorruptionTest, BoundaryStraddleCorruptsNewFrameOnly) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  /* frame 1: clean */
  for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);

  /* frame 2: missing slot 5 */
  uint32_t base2 = base + (uint32_t)n * s;
  for (int i = 0; i < n; i++) {
    if (i == 5) continue;
    feed(seq++, base2 + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }
  /* frame 3 first packet forward-jumps, closing frame 2 CORRUPTED */
  feed(seq++, base2 + (uint32_t)n * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u) << "prior frame must stay COMPLETE";
  EXPECT_EQ(corrupted(), 1u) << "partial frame must be CORRUPTED";
}

/* (e) Reorder / duplicate that lands on a filled slot must not corrupt the
 * frame nor double-count. */
TEST_F(St30RxCorruptionTest, ReorderDoesNotFalselyCorrupt) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  feed(seq++, base + 0u * s, MTL_SESSION_PORT_P);
  feed(seq++, base + 1u * s, MTL_SESSION_PORT_P);
  feed(seq++, base + 2u * s, MTL_SESSION_PORT_P);
  feed(seq++, base + 3u * s, MTL_SESSION_PORT_P);
  feed(seq++, base + 1u * s, MTL_SESSION_PORT_P); /* duplicate slot 1 */
  for (int i = 4; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (f) First packet of the session seeds the base, so a non-zero start seq must
 * not register a false gap. */
TEST_F(St30RxCorruptionTest, FirstPacketSeedingNoFalseGap) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint16_t seq = 1000;
  uint32_t base = 1000;
  for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (f) Session reset discards the open partial frame: a gap before reset must
 * not leak corruption into a clean frame fed afterwards. */
TEST_F(St30RxCorruptionTest, ResetClearsAccumulator) {
  use_single_port();
  uint32_t s = spp();
  uint32_t base = 1000;

  /* partial frame with a gap, never closed */
  feed(0, base + 0u * s, MTL_SESSION_PORT_P);
  feed(1, base + 5u * s, MTL_SESSION_PORT_P);

  ut30_reset(ctx_);

  /* clean full frame after reset */
  int n = ppf();
  uint16_t seq = 0;
  uint32_t base2 = 9000;
  for (int i = 0; i < n; i++) feed(seq++, base2 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (g) Redundancy: a per-port loss covered by the other port fills every slot,
 * so the frame stays COMPLETE. */
TEST_F(St30RxCorruptionTest, RedundancyCoveredLossComplete) {
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;
  for (int i = 0; i < n; i++) {
    /* slot 5 arrives only on R, every other slot only on P */
    enum mtl_session_port port = (i == 5) ? MTL_SESSION_PORT_R : MTL_SESSION_PORT_P;
    feed(seq++, base + (uint32_t)i * s, port);
  }

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* (g) Redundancy: loss on both ports leaves a missing slot, so the frame
 * closes CORRUPTED on the forward jump. */
TEST_F(St30RxCorruptionTest, RedundancyBothPortsLostCorrupted) {
  ut30_ctx_set_flags(ctx_, ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME);
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;
  for (int i = 0; i < n; i++) {
    if (i == 5) continue; /* dropped on both ports */
    feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }
  feed(seq++, base + (uint32_t)n * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(corrupted(), 1u);
  EXPECT_EQ(complete(), 0u);
}

/* ── positional single-slot tests ───────────────────────────────────────
 *
 * Whole-frame loss is skipped (nothing emitted); a partially filled frame is
 * closed CORRUPTED only when a forward jump in RTP timestamp opens the next
 * frame. Intra-frame reorder is healed by positional placement.
 */

/* Whole-frame loss emits nothing: the missing frame is skipped, the frames
 * either side close COMPLETE. */
TEST_F(St30RxCorruptionTest, WholeFrameLossSkips) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  /* skip the whole middle frame: emit nothing for it */
  uint32_t base3 = base + 2u * (uint32_t)n * s;
  for (int i = 0; i < n; i++) feed(seq++, base3 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 2u);
  EXPECT_EQ(corrupted(), 0u);
}

/* A straggler for an already-closed frame arrives after the boundary and is
 * dropped by the redundancy filter; it does not resurrect the closed frame. */
TEST_F(St30RxCorruptionTest, LateAcrossBoundaryDropped) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  /* frame 1 missing slot 5 */
  for (int i = 0; i < n; i++) {
    if (i == 5) continue;
    feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }
  /* forward jump opens frame 2, closing frame 1 CORRUPTED */
  uint32_t base2 = base + (uint32_t)n * s;
  feed(seq++, base2, MTL_SESSION_PORT_P);
  ASSERT_EQ(corrupted(), 1u);

  uint64_t redundant_before = redundant();
  /* late straggler for frame 1's slot 5 */
  feed(seq++, base + 5u * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(redundant(), redundant_before + 1u) << "late straggler dropped";
  EXPECT_EQ(corrupted(), 1u) << "closed frame not resurrected";
  EXPECT_EQ(complete(), 0u);
}

/* A duplicate of an already-placed slot is deduped by the bitmap and counted
 * redundant; the frame still completes. */
TEST_F(St30RxCorruptionTest, DuplicateSlotDedupedStillComplete) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint32_t base = 1000;
  uint16_t seq = 0;

  feed(seq++, base + 0u * s, MTL_SESSION_PORT_P);
  feed(seq++, base + 1u * s, MTL_SESSION_PORT_P);
  uint64_t redundant_before = redundant();
  feed(seq++, base + 1u * s, MTL_SESSION_PORT_P); /* duplicate slot 1 */
  EXPECT_EQ(redundant(), redundant_before + 1u);
  for (int i = 2; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(complete(), 1u);
  EXPECT_EQ(corrupted(), 0u);
}

/* Leading-packet loss must not re-anchor the media-clock grid. A frame whose
 * slot 0 is dropped is opened on the GRID base (carried by the next-frame
 * floor), so its slot 0 stays unset, the frame never reaches full size and
 * closes CORRUPTED — it must not absorb the following frame's slot 0 nor shift
 * the grid. Later clean frames stay COMPLETE on their original timestamps. */
TEST_F(St30RxCorruptionTest, LeadingPacketLossCorrupts) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint16_t seq = 0;

  uint32_t base = 1000;
  for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);

  uint32_t base2 = base + (uint32_t)n * s;
  for (int i = 1; i < n; i++) feed(seq++, base2 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  uint32_t base3 = base2 + (uint32_t)n * s;
  for (int i = 0; i < n; i++) feed(seq++, base3 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  uint32_t base4 = base3 + (uint32_t)n * s;
  for (int i = 0; i < n; i++) feed(seq++, base4 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(corrupted(), 1u) << "leading-loss frame must not absorb the next slot 0";
  EXPECT_EQ(complete(), 3u) << "frames 1,3,4 COMPLETE; grid did not drift";

  ASSERT_EQ(ut30_frame_log_count(ctx_), 4);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 0), base);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 0), ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 1), base2);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 1), ST_FRAME_STATUS_CORRUPTED);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 2), base3);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 2), ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 3), base4);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 3), ST_FRAME_STATUS_COMPLETE);
}

/* Leading-loss followed by a whole-frame skip: the partial frame closes
 * CORRUPTED, the wholly-missing frame emits nothing, and the large forward jump
 * is floored onto the grid, which the arriving timestamp already sits on. */
TEST_F(St30RxCorruptionTest, LeadingLossThenWholeFrameSkip) {
  use_single_port();
  int n = ppf();
  uint32_t s = spp();
  uint16_t seq = 0;

  uint32_t base = 1000;
  for (int i = 0; i < n; i++) feed(seq++, base + (uint32_t)i * s, MTL_SESSION_PORT_P);

  uint32_t base2 = base + (uint32_t)n * s;
  for (int i = 1; i < n; i++) feed(seq++, base2 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  /* skip frame 3 entirely, resume at frame 4 */
  uint32_t base4 = base2 + 2u * (uint32_t)n * s;
  for (int i = 0; i < n; i++) feed(seq++, base4 + (uint32_t)i * s, MTL_SESSION_PORT_P);

  EXPECT_EQ(corrupted(), 1u);
  EXPECT_EQ(complete(), 2u);

  ASSERT_EQ(ut30_frame_log_count(ctx_), 3);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 0), base);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 0), ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 1), base2);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 1), ST_FRAME_STATUS_CORRUPTED);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 2), base4);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 2), ST_FRAME_STATUS_COMPLETE);
}

/* ── delivery policy for an incomplete frame ────────────────────────────── */

/* Default policy: a frame missing a packet is never handed to the app, is
 * accounted as wire loss only, and its buffer goes back to the pool. More
 * partial frames than framebuffers, so a buffer that leaked would show up. */
TEST_F(St30RxCorruptionTest, IncompleteFramesSuppressedRecycledCountedAsWireLoss) {
  use_single_port();
  ut30_ctx_set_flags(ctx_, 0);
  uint16_t seq = 0;
  uint32_t base = 1000;

  for (int f = 0; f < 5; f++) {
    feed_frame(&seq, base, 0x11, 5);
    base += (uint32_t)ppf() * spp();
  }
  /* close the last partial frame */
  ut30_feed_pkt_fill(ctx_, seq++, base, MTL_SESSION_PORT_P, 0x11);

  EXPECT_EQ(ut30_frame_log_count(ctx_), 0)
      << "flags=0 (no ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME): app must see no frame";
  EXPECT_EQ(ut30_stat_frames_incomplete(ctx_), 5u);
  EXPECT_EQ(frames_done(), 0) << "a frame never handed up is not a frame received";
}

/* Opt-in: the partial frame is delivered, flagged CORRUPTED, and the slot that
 * never arrived reads as silence rather than as the audio the previous frame left
 * one frame earlier in the same recycled buffer. */
TEST_F(St30RxCorruptionTest, OptInGapOfReusedBufferReadsAsSilence) {
  use_single_port();
  uint16_t seq = 0;
  uint32_t base = 1000;
  uint32_t base2 = base + (uint32_t)ppf() * spp();

  feed_frame(&seq, base, 0xa5, -1);
  feed_frame(&seq, base2, 0x5a, 5);
  feed(seq++, base2 + (uint32_t)ppf() * spp(), MTL_SESSION_PORT_P);

  ASSERT_EQ(ut30_frame_log_count(ctx_), 2);
  ASSERT_EQ(ut30_frame_log_status(ctx_, 1), ST_FRAME_STATUS_CORRUPTED);
  ASSERT_EQ(ut30_frame_log_addr(ctx_, 1), ut30_frame_log_addr(ctx_, 0))
      << "the two frames must share a framebuffer for this to be the reuse case";
  EXPECT_EQ(ut30_stat_frames_incomplete(ctx_), 1u);
  EXPECT_EQ(frames_done(), 1) << "a delivered CORRUPTED frame is not a frame received";

  EXPECT_TRUE(SlotIs(1, 4, 0x5a)) << "received slots keep this frame's audio";
  EXPECT_TRUE(SlotIs(1, 5, 0x00)) << "missing slot must be silence, not stale 0xa5";
}
