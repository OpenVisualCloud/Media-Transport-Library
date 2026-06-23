/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * RFC 8331 / RFC 3550 header validation:
 * payload type, SSRC and F-bit checks; ReturnValue* contract for the
 * underlying packet handler.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St40RxHeaderValidationTest.*'
 */

#include <gtest/gtest.h>

#include "session/st40/st40_rx_test_base.h"

class St40RxHeaderValidationTest : public St40RxBaseTest {};

/* Accepted packet must return 0. */
TEST_F(St40RxHeaderValidationTest, ReturnValueAccepted) {
  int rc = feed(0, 1000, false, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St40RxHeaderValidationTest, ReturnValueWrongPT) {
  ut40_ctx_set_pt(ctx_, 96);
  int rc = ut40_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St40RxHeaderValidationTest, ReturnValueWrongSSRC) {
  ut40_ctx_set_ssrc(ctx_, 1234);
  int rc = ut40_feed_pkt_ssrc(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Invalid F-bits (0b01) must return -EINVAL. */
TEST_F(St40RxHeaderValidationTest, ReturnValueInvalidFBits) {
  int rc = ut40_feed_pkt_fbits(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 0x1);
  EXPECT_EQ(rc, -EINVAL);
}

/* Redundant (old-timestamp) packet returns -EAGAIN (not an error). */
TEST_F(St40RxHeaderValidationTest, ReturnValueRedundant) {
  feed(0, 2000, true, MTL_SESSION_PORT_P);
  int rc = feed(0, 1000, false, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, -EAGAIN);
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St40RxHeaderValidationTest, WrongPayloadTypeDropped) {
  ut40_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++)
    ut40_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Correct PT packets are accepted with zero wrong_pt. */
TEST_F(St40RxHeaderValidationTest, CorrectPayloadTypeAccepted) {
  ut40_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 4; i++)
    ut40_feed_pkt_pt(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 96);

  EXPECT_EQ(wrong_pt(), 0u);
  EXPECT_EQ(received(), 4u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St40RxHeaderValidationTest, WrongSSRCDropped) {
  ut40_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut40_feed_pkt_ssrc(ctx_, i, 1000 + i, 0, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* PT=0 disables the payload type check: any PT value is accepted. */
TEST_F(St40RxHeaderValidationTest, ZeroPTDisablesCheck) {
  /* PT=0 by default */
  ut40_feed_pkt_pt(ctx_, 0, 1000, 0, MTL_SESSION_PORT_P, 99);
  ut40_feed_pkt_pt(ctx_, 1, 1001, 0, MTL_SESSION_PORT_P, 111);

  EXPECT_EQ(wrong_pt(), 0u);
  EXPECT_EQ(received(), 2u);
}
