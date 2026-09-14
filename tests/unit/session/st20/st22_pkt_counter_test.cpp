/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * ST 2110-22 wire packet counter bounds (rv_handle_st22_pkt).
 *
 * The RFC 9134 P and Sep counters recombine into a 22 bit packet index that is
 * entirely wire-chosen, and it reaches two sinks unbounded: the dedup bitmap
 * index on a slot's first pkt, and the frame copy offset as
 * pkt_counter * st22_payload_length, a product that wraps 32 bit arithmetic
 * together with the guard meant to bound it (CWE-190 -> CWE-787).
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

/* Widest value the two wire counter fields can express. */
static constexpr uint32_t kMaxWireCounter = (1u << 22) - 1; /* 4194303 */

/* kMaxWireCounter * 1024 == 0xFFFFFC00, so the 32 bit guard sum wraps to 0. */
static constexpr uint16_t kOverflowPayload = 1024;

class St22RxPktCounterTest : public St20RxBaseTest {
 protected:
  void SetUp() override {
    St20RxBaseTest::SetUp();
    ut20_ctx_enable_st22(ctx_);
  }

  uint64_t st22_frames_ready() const {
    return ut20_st22_frames_ready(ctx_);
  }
};

/* Single port means slot_max == 1, as for a non-redundant session without RTCP:
 * every new RTP timestamp recycles the one slot, which keeps the st22_ lengths. */
class St22RxSlotRecycleTest : public St22RxPktCounterTest {
 protected:
  int num_port() const override {
    return 1;
  }
};

/* Frame 8 * 40 = 320 bytes against an 8 bit bitmap, so the last in-bitmap index
 * ends exactly on the frame end and acceptance there is observable. */
class St22RxBitmapEdgeTest : public St22RxPktCounterTest {
 protected:
  int pkts_per_frame() const override {
    return 8;
  }
};

/* ── the defects ───────────────────────────────────────────────────────── */

/* Pkt 1 publishes st22_payload_length 1024; pkt 2 multiplies it by the max wire
 * counter, wrapping the product to 0xFFFFFC00 and the 32 bit guard sum to 0, so
 * unfixed rv_frame_memcpy writes 1024 bytes ~4 GiB past the frame buffer. The
 * externally reported pair, 3355443 * 1280, wraps the same guard the same way. */
TEST_F(St22RxPktCounterTest, PktCounterOverflowingCopyOffsetRejected) {
  uint8_t payload[kOverflowPayload] = {0};

  /* pkt 1 is itself refused (1024 > the 80 byte frame) but only AFTER it has
   * published the payload length that pkt 2 multiplies */
  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 100, 1000, 0, false, payload, sizeof(payload),
                               MTL_SESSION_PORT_P),
            0);
  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 101, 1000, kMaxWireCounter, false, payload,
                               sizeof(payload), MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(offset_dropped(), 2u);
  EXPECT_EQ(received(), 0u);
}

/* First pkt of a slot: the seq_id branch's own range check has not armed yet, so
 * this is the one path that indexes the dedup bitmap straight off the wire --
 * unfixed, a read-modify-write ~512 KB past a 1 byte bitmap. */
TEST_F(St22RxPktCounterTest, FirstPktCounterOutOfBitmapRejected) {
  uint8_t payload[40] = {0};

  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 100, 1000, kMaxWireCounter, false, payload,
                               sizeof(payload), MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(idx_oo_bitmap(), 1u);
  EXPECT_EQ(received(), 0u);
}

/* The box length outlives the pkt that measured it: frame A declares 64 bytes of
 * boxes, then frame B recycles the slot and its first pkt carries a non-zero
 * counter, so the boxes are never re-parsed and a fresh 40 byte payload length
 * pairs with the stale 64. Counter 2 satisfies a floor on the product
 * (2 * 40 >= 64) and still misplaces the copy, so only a floor on the payload
 * length rejects it; counter 1 underflows the offset outright. */
TEST_F(St22RxSlotRecycleTest, StaleBoxHdrLengthRejected) {
  uint8_t boxed[100] = {0};
  /* lbox 42 (the real jpvs size, so colr is found where it was written) plus 22 */
  ASSERT_LE(ut20_st22_build_boxes(boxed, 42, 22), sizeof(boxed));
  ASSERT_EQ(ut20_feed_st22_pkt(ctx_, 100, 1000, 0, false, boxed, sizeof(boxed),
                               MTL_SESSION_PORT_P),
            0);
  ASSERT_EQ(ut20_stat_st22_boxes(ctx_), 1u);
  ASSERT_EQ(received(), 1u);

  uint8_t plain[40] = {0};
  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 200, 2000, 2, false, plain, sizeof(plain),
                               MTL_SESSION_PORT_P),
            0);
  /* 1 * 40 - 64 underflows, and the wrapped sum lands back inside the frame, so
   * unfixed the copy goes 24 bytes before frame->addr */
  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 300, 3000, 1, false, plain, sizeof(plain),
                               MTL_SESSION_PORT_P),
            0);
  /* the length guard, not the frame-size check, is what refused both */
  EXPECT_EQ(ut20_stat_wrong_len(ctx_), 2u);
  EXPECT_EQ(offset_dropped(), 0u);
  /* only frame A's legitimate box pkt was ever copied */
  EXPECT_EQ(received(), 1u);
}

/* ── the false-rejection guards ────────────────────────────────────────── */

/* Either side of the bitmap capacity, on a frame whose leading pkts were lost.
 * Index 7 is the last in-bitmap pkt and must be accepted and copied, so the
 * bound is neither an off-by-one nor a "first pkt must be counter 0"; index 8
 * must be refused, and by the bitmap bound rather than the frame-size guard. */
TEST_F(St22RxBitmapEdgeTest, BitmapEdgeAcceptedAndPastEdgeRejected) {
  uint8_t payload[40] = {0};
  /* 8 rows -> bitmap (8 + 7) / 8 = 1 byte = 8 bits */
  const uint32_t bits = 8;

  EXPECT_EQ(ut20_feed_st22_pkt(ctx_, 100, 1000, bits - 1, false, payload, sizeof(payload),
                               MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(offset_dropped(), 0u);

  EXPECT_LT(ut20_feed_st22_pkt(ctx_, 200, 2000, bits, false, payload, sizeof(payload),
                               MTL_SESSION_PORT_P),
            0);
  EXPECT_EQ(idx_oo_bitmap(), 1u);
  EXPECT_EQ(received(), 1u);
}

/* End to end: an ordinary two pkt ST 2110-22 frame with a box header still
 * assembles at the right size. Any bound that rejects real traffic fails here. */
TEST_F(St22RxPktCounterTest, LegitimateBoxedFrameStillAssembles) {
  uint8_t first[100] = {0};
  const uint16_t boxes = ut20_st22_build_boxes(first, 42, 18);
  ASSERT_EQ(boxes, 60u);

  /* pkt 0 carries 60 bytes of boxes + 40 bytes of codestream */
  ASSERT_EQ(ut20_feed_st22_pkt(ctx_, 100, 1000, 0, false, first, sizeof(first),
                               MTL_SESSION_PORT_P),
            0);
  ASSERT_EQ(ut20_stat_st22_boxes(ctx_), 1u);

  /* pkt 1 lands at 1 * 100 - 60 = 40 and carries the final 40 bytes */
  uint8_t second[100] = {0};
  ASSERT_EQ(ut20_feed_st22_pkt(ctx_, 101, 1000, 1, true, second, 40, MTL_SESSION_PORT_P),
            0);

  EXPECT_EQ(received(), 2u);
  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(st22_frames_ready(), 1u);
  EXPECT_EQ(ut20_st22_last_frame_size(ctx_), 80u);
}
