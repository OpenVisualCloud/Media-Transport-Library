/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Out-of-frame packet indices in the ST 2110-20 / -22 RX path, for every
 * handler that writes the slot bitmap: rv_handle_frame_pkt, rv_handle_rtp_pkt
 * and rv_handle_st22_pkt.
 *
 * Each handler derives the index from the RTP sequence number, so a jumped or
 * forged sequence points past the frame. Such a packet must be refused, counted
 * in stat_pkts_idx_oo_bitmap, and must not write a bit outside the bitmap. The
 * harness keeps guard bytes after every slot bitmap, so a write outside is
 * visible.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxBitmapBoundsTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxBitmapBoundsTest : public St20RxBaseTest {
 protected:
  bool guard_intact() {
    return ut20_bitmap_guard_intact(ctx_);
  }
  uint64_t oo_bitmap() {
    return ut20_stat_idx_oo_bitmap(ctx_);
  }
};

/* Baseline: a full frame writes only inside the bitmap. */
TEST_F(St20RxBitmapBoundsTest, FullFrameStaysInsideBitmap) {
  ut20_feed_full_frame(ctx_, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(received(), (uint64_t)ut20_pkts_per_frame(ctx_));
  EXPECT_EQ(oo_bitmap(), 0u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_frame_pkt, seq far ahead of the base: the index leaves the frame. */
TEST_F(St20RxBitmapBoundsTest, FramePktIdxPastFrameRefused) {
  ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P); /* base = 0 */
  ut20_feed_frame_pkt_seq(ctx_, 1, 5000, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_EQ(received(), 1u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_frame_pkt, 32-bit wrap of the sequence distance: seq below the base
 * resolves to a near-4G index. */
TEST_F(St20RxBitmapBoundsTest, FramePktWrappedIdxPastFrameRefused) {
  ut20_feed_frame_pkt_seq(ctx_, 0, 1000000, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_seq(ctx_, 1, 1, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_EQ(received(), 1u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_frame_pkt, first packet of a frame: a line past the frame is caught
 * by the offset check before the index check, so it never seeds the bitmap. */
TEST_F(St20RxBitmapBoundsTest, FramePktFirstPktLinePastFrameRefused) {
  const uint16_t line_past_frame = 4000;
  ut20_feed_pkt(ctx_, 0, 1000, line_past_frame, 0, 40, MTL_SESSION_PORT_P);

  EXPECT_EQ(ut20_stat_offset_dropped(ctx_), 1u);
  EXPECT_EQ(received(), 0u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_frame_pkt, first packet of a frame, the shape of the reported
 * one-packet attack on v26.01: the offset stays inside the frame while
 * row_length shrinks to 1, so pkt_idx = offset / payload_length leaves the
 * bitmap. The reported packet gave index 6480 against an 810-byte bitmap. The
 * harness geometry gives index 40 against a 1-byte bitmap. */
TEST_F(St20RxBitmapBoundsTest, FramePktFirstPktShortRowLengthIdxPastFrame) {
  const uint16_t line_num = 1;   /* offset = 1 * 40 = 40, inside the frame */
  const uint16_t row_length = 1; /* pkt_idx = 40 / 1 = 40, bitmap holds 8 */
  ut20_feed_pkt(ctx_, 0, 1000, line_num, 0, row_length, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_EQ(received(), 0u);
  EXPECT_TRUE(guard_intact());
}

/* Same branch with row_length 0: payload_length is the divisor of that pkt_idx,
 * so a zero must be refused before the modulo. */
TEST_F(St20RxBitmapBoundsTest, FramePktFirstPktZeroRowLengthRefused) {
  ut20_feed_pkt(ctx_, 0, 1000, 1, 0, 0, MTL_SESSION_PORT_P);

  EXPECT_EQ(ut20_stat_wrong_len(ctx_), 1u);
  EXPECT_EQ(received(), 0u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_rtp_pkt: same jump in RTP passthrough mode. */
TEST_F(St20RxBitmapBoundsTest, RtpPktIdxPastFrameRefused) {
  ut20_ctx_enable_rtp(ctx_);
  ut20_feed_rtp_pkt(ctx_, 0, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_rtp_pkt(ctx_, 1, 5000, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_st22_pkt: the codestream handler indexes by sequence too. */
TEST_F(St20RxBitmapBoundsTest, St22PktIdxPastFrameRefused) {
  ut20_ctx_enable_st22(ctx_);
  uint8_t boxes[60];
  uint16_t box_len = ut20_st22_build_boxes(boxes, 42, 18);
  ut20_feed_st22_pkt(ctx_, 0, 1000, 0, false, boxes, box_len, MTL_SESSION_PORT_P);
  ut20_feed_st22_pkt(ctx_, 5000, 1000, 1, false, boxes, box_len, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_TRUE(guard_intact());
}

/* rv_handle_st22_pkt, first packet: the index is the packet counter itself. */
TEST_F(St20RxBitmapBoundsTest, St22FirstPktCounterPastFrameRefused) {
  ut20_ctx_enable_st22(ctx_);
  uint8_t boxes[60];
  uint16_t box_len = ut20_st22_build_boxes(boxes, 42, 18);
  ut20_feed_st22_pkt(ctx_, 0, 1000, 5000, false, boxes, box_len, MTL_SESSION_PORT_P);

  EXPECT_EQ(oo_bitmap(), 1u);
  EXPECT_TRUE(guard_intact());
}
