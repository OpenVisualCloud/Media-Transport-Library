/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 *
 * RFC 3550 header validation:
 * payload type, SSRC and packet length checks; ReturnValue* contract for the
 * underlying packet handler.
 *
 * Build: meson setup build_unit -Denable_unit_tests=true && ninja -C build_unit
 * Run:   ./build_unit/tests/unit/UnitTest --gtest_filter='St30RxHeaderValidationTest.*'
 */

#include <gtest/gtest.h>

#include "session/st30/st30_rx_test_base.h"

class St30RxHeaderValidationTest : public St30RxBaseTest {};

/* Accepted packet must return 0. */
TEST_F(St30RxHeaderValidationTest, ReturnValueAccepted) {
  int rc = feed(0, 1000, MTL_SESSION_PORT_P);
  EXPECT_EQ(rc, 0);
}

/* Wrong payload type must return -EINVAL. */
TEST_F(St30RxHeaderValidationTest, ReturnValueWrongPT) {
  ut30_ctx_set_pt(ctx_, 96);
  int rc = ut30_feed_pkt_pt(ctx_, 0, 1000, MTL_SESSION_PORT_P, 97);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong SSRC must return -EINVAL. */
TEST_F(St30RxHeaderValidationTest, ReturnValueWrongSSRC) {
  ut30_ctx_set_ssrc(ctx_, 1234);
  int rc = ut30_feed_pkt_ssrc(ctx_, 0, 1000, MTL_SESSION_PORT_P, 5678);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong payload length must return -EINVAL. */
TEST_F(St30RxHeaderValidationTest, ReturnValueWrongLen) {
  int rc = ut30_feed_pkt_len(ctx_, 0, 1000, MTL_SESSION_PORT_P, 100);
  EXPECT_EQ(rc, -EINVAL);
}

/* Wrong PT packets are dropped and counted in wrong_pt stat. */
TEST_F(St30RxHeaderValidationTest, WrongPayloadTypeDropped) {
  ut30_ctx_set_pt(ctx_, 96);
  for (int i = 0; i < 5; i++) ut30_feed_pkt_pt(ctx_, i, 1000 + i, MTL_SESSION_PORT_P, 97);

  EXPECT_EQ(wrong_pt(), 5u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong SSRC packets are dropped and counted in wrong_ssrc stat. */
TEST_F(St30RxHeaderValidationTest, WrongSSRCDropped) {
  ut30_ctx_set_ssrc(ctx_, 0xDEAD);
  for (int i = 0; i < 3; i++)
    ut30_feed_pkt_ssrc(ctx_, i, 1000 + i, MTL_SESSION_PORT_P, 0xBEEF);

  EXPECT_EQ(wrong_ssrc(), 3u);
  EXPECT_EQ(received(), 0u);
}

/* Wrong payload length packets are dropped and counted in len_mismatch stat. */
TEST_F(St30RxHeaderValidationTest, WrongPacketLenDropped) {
  ut30_feed_pkt_len(ctx_, 0, 1000, MTL_SESSION_PORT_P, 50);
  ut30_feed_pkt_len(ctx_, 1, 1001, MTL_SESSION_PORT_P, 300);

  EXPECT_EQ(len_mismatch(), 2u);
  EXPECT_EQ(received(), 0u);
}
