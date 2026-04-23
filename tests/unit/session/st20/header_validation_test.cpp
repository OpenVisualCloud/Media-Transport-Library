/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * RFC 4175 header validation:
 * payload type, SSRC, interlace and the ReturnValue* contract for the
 * underlying packet handler.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St20RxHeaderValidationTest.*'
 */

#include <gtest/gtest.h>

#include "session/st20/st20_rx_test_base.h"

class St20RxHeaderValidationTest : public St20RxBaseTest {};

/* Accepted packet must return 0. */
TEST_F(St20RxHeaderValidationTest, ReturnValueAccepted) {
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St20RxHeaderValidationTest, ReturnValueWrongPT) {
  ut20_ctx_set_pt(ctx_, 96);
  int rc = ut20_feed_pkt_pt(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St20RxHeaderValidationTest, ReturnValueWrongSSRC) {
  ut20_ctx_set_ssrc(ctx_, 1234);
  int rc = ut20_feed_pkt_ssrc(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* No slot available must return -EIO. */
TEST_F(St20RxHeaderValidationTest, ReturnValueNoSlot) {
  /* Fill both slots with completed frames */
  feed_full(5000, MTL_SESSION_PORT_P);
  feed_full(6000, MTL_SESSION_PORT_P);

  /* Old timestamp, both slots hold newer ts → no slot */
  int rc = ut20_feed_frame_pkt(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  EXPECT_LT(rc, 0) << "Should reject when no slot available";
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St20RxHeaderValidationTest, WrongPayloadTypeDropped) {
  ut20_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++)
    ut20_feed_pkt_pt(ctx_, i, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St20RxHeaderValidationTest, WrongSSRCDropped) {
  ut20_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut20_feed_pkt_ssrc(ctx_, i, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* Second-field bit on a progressive stream triggers wrong_interlace. */
TEST_F(St20RxHeaderValidationTest, WrongInterlaceDropped) {
  /* ops.interlaced = false by default; send row_number with second_field bit set */
  int rc = ut20_feed_pkt(ctx_, 0, 1000, 0x8000, 0, 40, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, -EINVAL);
  EXPECT_EQ(wrong_interlace(), 1u);
}
