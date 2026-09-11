/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * Timestamp filter edge cases:
 * 32-bit wrap, rapid advance, old-vs-new seq with stale ts, audio frame
 * boundary and seq-wrap accounting against reorder.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxTimestampTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"
#include "st_api.h"

class St30RxTimestampTest : public St30RxBaseTest {};

/* Audio frame boundary: feed exactly pkts_per_frame packets with
 * incrementing timestamps and verify exactly one frame is completed. */
TEST_F(St30RxTimestampTest, AudioFrameBoundary) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total_pkts = ppf();
  ASSERT_GT(total_pkts, 0);

  /* each packet lands in its own positional slot */
  uint32_t s = spp();
  for (int i = 0; i < total_pkts; i++) {
    feed(i, 1000 + (uint32_t)i * s, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(received(), (uint64_t)total_pkts);
  EXPECT_EQ(frames_done(), 1);
  EXPECT_EQ(unrecovered(), 0u);
}

/* A loss burst ending mid-frame must not move the frame grid. Re-anchoring on
 * the arriving timestamp instead shifts every later frame by a fraction of a
 * frame, so the audio written out is no longer a frame-aligned window of what
 * was sent. */
TEST_F(St30RxTimestampTest, FrameGridSurvivesGapEndingMidFrame) {
  const int total_pkts = ppf();
  const uint32_t ticks = (uint32_t)total_pkts * spp();
  const uint32_t first = 1000;

  uint16_t seq = 0;
  for (int i = 0; i < total_pkts; i++)
    feed(seq++, first + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  ASSERT_EQ(ut30_frame_log_count(ctx_), 1);
  ASSERT_EQ(ut30_frame_log_ts(ctx_, 0), first);

  /* lose all of frame 1 and the leading 3 packets of frame 2 */
  const int lost_head = 3;
  for (int i = lost_head; i < total_pkts; i++)
    feed(seq++, first + 2 * ticks + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  /* frame 3 arrives whole */
  for (int i = 0; i < total_pkts; i++)
    feed(seq++, first + 3 * ticks + (uint32_t)i * spp(), MTL_SESSION_PORT_P);

  ASSERT_EQ(ut30_frame_log_count(ctx_), 2) << "the partial frame 2 must not be handed up";
  EXPECT_EQ(ut30_frame_log_status(ctx_, 1), ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 1), first + 3 * ticks)
      << "frame after the gap must stay on the grid, not re-anchor to the first"
         " packet that got through";
}

/* A long burst from behind the floor makes the redundancy filter give up and
 * accept one (ST_SESSION_REDUNDANT_ERROR_THRESHOLD), opening a frame from a
 * timestamp the stream moved past. It must land a whole number of frames back
 * on the same grid, or every later frame is spliced mid-frame. */
TEST_F(St30RxTimestampTest, FrameGridSurvivesPacketAcceptedFromBehindTheFloor) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  const int total_pkts = ppf();
  const uint32_t ticks = (uint32_t)total_pkts * spp();
  const uint32_t first = 1000;

  uint16_t seq = 0;
  for (int f = 0; f < 2; f++)
    for (int i = 0; i < total_pkts; i++)
      feed(seq++, first + (uint32_t)f * ticks + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  ASSERT_EQ(ut30_frame_log_count(ctx_), 2);

  /* the second frame arrives all over again, long after the stream moved on */
  uint64_t recv_before = received();
  for (int i = 0; i < total_pkts; i++)
    feed(seq++, first + ticks + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  ASSERT_GT(received(), recv_before)
      << "the filter must give up on a burst this long, or this case never opens"
         " a frame from behind the floor";
  ASSERT_EQ(ut30_frame_log_count(ctx_), 2)
      << "the re-opened frame is partial and must not be handed up";

  /* the stream continues where it left off */
  for (int i = 0; i < total_pkts; i++)
    feed(seq++, first + 3 * ticks + (uint32_t)i * spp(), MTL_SESSION_PORT_P);

  ASSERT_EQ(ut30_frame_log_count(ctx_), 3);
  EXPECT_EQ(ut30_frame_log_status(ctx_, 2), ST_FRAME_STATUS_COMPLETE);
  EXPECT_EQ(ut30_frame_log_ts(ctx_, 2), first + 3 * ticks)
      << "a frame opened from behind the floor must snap to a whole frame back,"
         " leaving the grid the stream joined on unchanged";
}

/* 32-bit timestamp wraparound from near UINT32_MAX past zero. Positional
 * slots span the wrap boundary and every packet is accepted. */
TEST_F(St30RxTimestampTest, TimestampWrapAround) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed_burst(0, 8, 0xFFFFFFF0, MTL_SESSION_PORT_P);

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(received(), 8u);
}

/* Large timestamp jump: packets are accepted but the sequence gap between
 * them is correctly counted as unrecovered. */
TEST_F(St30RxTimestampTest, RapidTimestampAdvance) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed(0, 1000, MTL_SESSION_PORT_P);
  feed(100, 999000, MTL_SESSION_PORT_P);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(unrecovered(), 99u);
}

/* Backward timestamp with newer seq_id. ST30 filters on timestamp ONLY,
 * so this must be rejected even though the seq_id advances. */
TEST_F(St30RxTimestampTest, OldTimestampNewSeq) {
  feed_burst(0, 4, 5000, MTL_SESSION_PORT_P);

  /* newer seq but older timestamp — must be rejected */
  int rc = feed(4, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Packet with old timestamp must be rejected";
  EXPECT_GE(redundant(), 1u);
}

/* Back-to-back monotonic timestamps on a single port, no gaps.
 * All packets accepted, zero unrecovered and redundant. */
TEST_F(St30RxTimestampTest, BackToBackMonotonic) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  for (int i = 0; i < 40; i++) {
    feed(i, 1000 + (uint32_t)i * spp(), MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(unrecovered(), 0u);
  EXPECT_EQ(redundant(), 0u);
  EXPECT_EQ(received(), 40u);
}

/* Seq-wrap must be seen as forward, not reorder. */
TEST_F(St30RxTimestampTest, SeqWrapNotCountedAsReorder) {
  feed(65534, 1000, MTL_SESSION_PORT_P);
  feed(65535, 1001, MTL_SESSION_PORT_P);
  uint64_t reord_before = port_reordered(MTL_SESSION_PORT_P);

  feed(0, 1002, MTL_SESSION_PORT_P);
  feed(1, 1003, MTL_SESSION_PORT_P);

  EXPECT_EQ(port_reordered(MTL_SESSION_PORT_P), reord_before);
}

/* session_seq_id is updated unconditionally after the gap-detection branch
 * in the audio rx path (`s->session_seq_id = seq_id;`), unlike the ancillary
 * path which guards with `mt_seq16_greater`. If a packet with a backward seq
 * but a *new* timestamp slips past the redundancy filter, session_seq_id
 * would move backwards and a subsequent forward packet would compute a
 * massive phantom gap via uint16 wrap. This test drives that scenario and
 * verifies the unrecovered counter does NOT inflate. */
TEST_F(St30RxTimestampTest, SessionSeqDoesNotMoveBackwardOnNewTs) {
  /* normal forward burst: session_seq advances to 100 */
  for (uint16_t i = 90; i <= 100; i++) feed(i, 1000 + i, MTL_SESSION_PORT_P);
  uint64_t unrec_before = unrecovered();
  ASSERT_EQ(unrec_before, 0u);

  /* backward seq with newer ts — may occur on a misbehaving sender or after
   * a stream restart. Per the user-visible contract, the lib must not let
   * this poison session_seq_id and inflate unrecovered on the next packet. */
  feed(50, 2000, MTL_SESSION_PORT_P);

  /* now a normal forward packet: if session_seq_id was set to 50 above,
   * the gap (uint16_t)(101 - 50 - 1) = 50 phantom unrecovered. */
  feed(101, 2001, MTL_SESSION_PORT_P);

  EXPECT_LE(unrecovered() - unrec_before, 1u)
      << "session_seq_id must not move backward on a newer-ts packet;"
         " doing so inflates stat_pkts_unrecovered via uint16 wrap";
}