/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Tests for the per-port `err_packets` counter on the ST 2110-20 RX session.
 * The `_handle_mbuf` wrapper increments
 * `port_user_stats.common.port[s_port].err_packets` whenever the per-packet
 * handler returns < 0.
 *
 * Test goals:
 *   1. Every genuine error path (wrong PT, wrong SSRC, bad offset) MUST
 *      increment err_packets and the corresponding per-reason counter,
 *      exactly once each.
 *   2. err_packets MUST equal the sum of per-reason drop counters
 *      (no double-counting, no silent drops).
 *   3. Successful packets MUST NOT increment err_packets.
 *   4. ST20 bitmap-based redundancy dedup returns 0 (not -EIO), so
 *      legitimate redundant packets MUST NOT count as errors.
 */

#include <gtest/gtest.h>

#include "session/st20_harness.h"

class St20RxErrPacketsTest : public ::testing::Test {
 protected:
  ut20_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut20_init(), 0);
    ctx_ = ut20_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut20_ctx_set_pt(ctx_, 96);
    ut20_ctx_set_ssrc(ctx_, 0xDEAD);
  }
  void TearDown() override {
    if (ctx_) ut20_ctx_destroy(ctx_);
  }

  uint64_t err_p() {
    return ut20_stat_port_err_packets(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t err_r() {
    return ut20_stat_port_err_packets(ctx_, MTL_SESSION_PORT_R);
  }
  uint64_t pkts_p() {
    return ut20_stat_port_packets(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t pkts_r() {
    return ut20_stat_port_packets(ctx_, MTL_SESSION_PORT_R);
  }
};

/* Good packets via the wrapper: per-port packets++ , err_packets stays 0. */
TEST_F(St20RxErrPacketsTest, GoodPacketsNotCountedAsErr) {
  /* feed a complete frame (2 packets) on primary */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(err_p(), 0u) << "good packets must not bump err_packets";
  EXPECT_EQ(pkts_p(), 2u) << "wrapper must increment per-port packets counter";
  EXPECT_EQ(err_r(), 0u);
  EXPECT_EQ(pkts_r(), 0u);
}

/* Wrong payload type: -EINVAL → err_packets++ AND wrong_pt++. */
TEST_F(St20RxErrPacketsTest, WrongPtCountedAsErr) {
  /* pt=200 differs from session pt=96 */
  ut20_feed_pkt_via_wrapper(ctx_, /*seq=*/0, /*ts=*/1000, /*line=*/0, /*off=*/0,
                            /*len=*/40, MTL_SESSION_PORT_P, /*pt=*/200, /*ssrc=*/0xDEAD);

  EXPECT_EQ(ut20_stat_wrong_pt(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u) << "wrong-PT packet must bump err_packets[P]";
  EXPECT_EQ(pkts_p(), 0u) << "rejected packets must NOT bump good-packets counter";
  EXPECT_EQ(err_r(), 0u);
}

/* Wrong SSRC: -EINVAL → err_packets++ AND wrong_ssrc++. */
TEST_F(St20RxErrPacketsTest, WrongSsrcCountedAsErr) {
  ut20_feed_pkt_via_wrapper(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, /*pt=*/96,
                            /*ssrc=*/0xBAD);

  EXPECT_EQ(ut20_stat_wrong_ssrc(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

/* err_packets per port must equal the sum of all per-port drop reasons.
 * This is the master invariant: nothing silently bumps err_packets. */
TEST_F(St20RxErrPacketsTest, ErrPacketsEqualsSumOfReasons) {
  /* feed assorted bad packets on primary */
  ut20_feed_pkt_via_wrapper(ctx_, 0, 1000, 0, 0, 40, MTL_SESSION_PORT_P, /*pt=*/200,
                            0xDEAD); /* wrong PT */
  ut20_feed_pkt_via_wrapper(ctx_, 1, 1000, 0, 0, 40, MTL_SESSION_PORT_P, /*pt=*/200,
                            0xDEAD); /* wrong PT */
  ut20_feed_pkt_via_wrapper(ctx_, 2, 1000, 0, 0, 40, MTL_SESSION_PORT_P, 96,
                            /*ssrc=*/0xBAD); /* wrong SSRC */
  ut20_feed_pkt_via_wrapper(ctx_, 3, 1000, /*line=*/9999, /*off=*/0, /*len=*/40,
                            MTL_SESSION_PORT_P, 96, 0xDEAD); /* offset out of frame */

  uint64_t reasons = ut20_stat_wrong_pt(ctx_) + ut20_stat_wrong_ssrc(ctx_) +
                     ut20_stat_wrong_interlace(ctx_) + ut20_stat_offset_dropped(ctx_) +
                     ut20_stat_wrong_len(ctx_) + ut20_stat_no_slot(ctx_) +
                     ut20_stat_idx_oo_bitmap(ctx_);
  EXPECT_GT(reasons, 0u);
  EXPECT_EQ(err_p(), reasons)
      << "every err_packets bump must be explainable by a per-reason counter";
}

/* Mixed burst: 1 valid, 1 wrong-pt, 1 valid → exactly 1 err_packet. */
TEST_F(St20RxErrPacketsTest, MixedBurstOnlyBadCounted) {
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_pkt_via_wrapper(ctx_, 99, 1000, 0, 0, 40, MTL_SESSION_PORT_P, /*pt=*/200,
                            0xDEAD);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);

  EXPECT_EQ(err_p(), 1u) << "only the wrong-PT packet may be counted as error";
  EXPECT_EQ(pkts_p(), 2u) << "the two valid packets must be counted as good";
}

/* Duplicate-bitmap packet from redundant port: returns 0 in ST20 (bitmap
 * dedup, not the -EIO redundancy filter used by ST30/ST40), must not
 * count as error. */
TEST_F(St20RxErrPacketsTest, BitmapDuplicateRedundantNotErr) {
  /* full frame on P, then same frame on R: every R packet is a bitmap dup */
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_P);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_R);
  ut20_feed_frame_pkt_via_wrapper(ctx_, 1, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(ut20_stat_redundant(ctx_), 2u)
      << "both R packets must be classified as redundant";
  EXPECT_EQ(err_r(), 0u)
      << "ST20 bitmap-redundant packets must NOT count as errors (regression guard)";
}
