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

class St30RxTimestampTest : public St30RxBaseTest {};

/* Audio frame boundary: feed exactly pkts_per_frame packets with
 * incrementing timestamps and verify exactly one frame is completed. */
TEST_F(St30RxTimestampTest, AudioFrameBoundary) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  int total_pkts = ppf();
  ASSERT_GT(total_pkts, 0);

  /* each packet needs a unique (increasing) timestamp */
  for (int i = 0; i < total_pkts; i++) {
    feed(i, 1000 + i, MTL_SESSION_PORT_P);
  }

  EXPECT_EQ(received(), (uint64_t)total_pkts);
  EXPECT_EQ(frames_done(), 1);
  EXPECT_EQ(unrecovered(), 0u);
}

/* 32-bit timestamp wraparound from near UINT32_MAX to near zero.
 * The wrapped timestamps should be accepted as strictly greater. */
TEST_F(St30RxTimestampTest, TimestampWrapAround) {
  ut30_ctx_destroy(ctx_);
  ctx_ = ut30_ctx_create(1);
  ASSERT_NE(ctx_, nullptr);

  feed_burst(0, 4, 0xFFFFFFF0, MTL_SESSION_PORT_P);
  feed_burst(4, 4, 0x00000010, MTL_SESSION_PORT_P);

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
    feed(i, 1000 + i, MTL_SESSION_PORT_P);
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