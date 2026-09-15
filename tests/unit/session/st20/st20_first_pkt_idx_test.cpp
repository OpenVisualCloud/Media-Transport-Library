/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * First-pkt index derivation in rv_handle_frame_pkt (ST 2110-20 frame path), the
 * ST20 twin of the ST22 pkt_counter hole in st22_pkt_counter_test.cpp. The
 * seq_id branch bounds its bitmap index; the first-pkt branch of the same
 * function derives one from `offset / payload_length` -- both wire-controlled --
 * and bounds nothing. The frame-buffer guard above it bounds the offset, not the
 * quotient, and the divisor may be zero.
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

/* 32 rows of 40 bytes: frame 1280 bytes, bitmap (32 + 7) / 8 = 4 bytes = 32
 * bits. Wide enough that an in-frame offset divided by a 1 byte row lands far
 * outside the bitmap. */
class St20RxFirstPktIdxTest : public St20RxBaseTest {
 protected:
  int pkts_per_frame() const override {
    return 32;
  }

  uint64_t wrong_len() const {
    return ut20_stat_wrong_len(ctx_);
  }
};

/* offset 31 * 40 = 1240 passes the frame guard (1280 <= 1280), then 1240 / 1
 * indexes bit 1240 of a 32 bit bitmap -- a read-modify-write 155 bytes past a
 * 4 byte allocation, after which the pkt is fully accepted, so unfixed nothing
 * anywhere records that memory was corrupted. */
TEST_F(St20RxFirstPktIdxTest, FirstPktBitmapIndexPastBitmapRejected) {
  EXPECT_LT(ut20_feed_pkt(ctx_, 100, 1000, 31, 0, 1, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(idx_oo_bitmap(), 1u);
  EXPECT_EQ(received(), 0u);
}

/* row_length 0 on a datagram with no payload: pkt_payload_len == payload_length
 * == 0 satisfies the only length check, and the first-pkt branch then uses
 * payload_length as a divisor. One such pkt raises SIGFPE. */
TEST_F(St20RxFirstPktIdxTest, ZeroRowLengthNotUsedAsDivisor) {
  EXPECT_LT(ut20_feed_pkt(ctx_, 100, 1000, 0, 0, 0, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(wrong_len(), 1u);
  EXPECT_EQ(received(), 0u);
}

/* A frame whose leading pkts were lost: the first pkt the receiver sees describes
 * the last row. Its index is in range and it must be accepted, so the bound
 * cannot be tightened to "the first pkt must be row 0". */
TEST_F(St20RxFirstPktIdxTest, FirstPktWithLostLeadingPktsAccepted) {
  EXPECT_EQ(ut20_feed_pkt(ctx_, 100, 1000, 31, 0, 40, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(wrong_len(), 0u);
}

/* The bound sits after both arms of the derivation, and the cases above all take
 * the quotient arm. A row_length that does not divide the offset selects the
 * GPM_SL arm, which indexes from line1_number and line1_offset instead. Both
 * feeds carry line1_offset 64, so both derive offset 160 and differ only in the
 * row_length that picks the arm: 160 % 3 != 0 -> GPM_SL, index 64 / 16 = 4, in
 * range, accepted -- the quotient of the same offset, 160 / 5 = 32, is one past
 * the 32 bit bitmap and refused. So the first feed passes only if the guarded
 * value came from the GPM_SL arm. */
TEST_F(St20RxFirstPktIdxTest, GpmSlFirstPktIdxAccepted) {
  EXPECT_EQ(ut20_feed_pkt(ctx_, 100, 1000, 0, 64, 3, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(received(), 1u);
  EXPECT_EQ(idx_oo_bitmap(), 0u);
  EXPECT_EQ(offset_dropped(), 0u);
  EXPECT_EQ(wrong_len(), 0u);

  EXPECT_LT(ut20_feed_pkt(ctx_, 200, 2000, 0, 64, 5, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(idx_oo_bitmap(), 1u);
  EXPECT_EQ(received(), 1u);
}
