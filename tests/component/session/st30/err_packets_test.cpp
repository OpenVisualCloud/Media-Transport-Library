/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Tests for the per-port `err_packets` counter on the ST 2110-30 RX session.
 * The `_handle_mbuf` wrapper increments
 * `port_user_stats.common.port[s_port].err_packets` whenever the per-packet
 * handler returns < 0.
 *
 * Test goals:
 *   1. Every genuine error path (wrong PT, wrong SSRC, wrong length) MUST
 *      increment err_packets and the corresponding per-reason counter,
 *      exactly once each.
 *   2. err_packets MUST equal the sum of per-reason drop counters.
 *   3. Successful packets MUST NOT increment err_packets.
 *   4. Redundancy-filtered packets (legitimate duplicates) MUST NOT
 *      count as errors.
 */

#include <gtest/gtest.h>

#include "session/st30_harness.h"

class St30RxErrPacketsTest : public ::testing::Test {
 protected:
  ut30_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut30_init(), 0);
    ctx_ = ut30_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut30_ctx_set_pt(ctx_, 97);
    ut30_ctx_set_ssrc(ctx_, 0xCAFE);
  }
  void TearDown() override {
    if (ctx_) ut30_ctx_destroy(ctx_);
  }

  uint64_t err_p() {
    return ut30_stat_port_err_packets(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t err_r() {
    return ut30_stat_port_err_packets(ctx_, MTL_SESSION_PORT_R);
  }
  uint64_t pkts_p() {
    return ut30_stat_port_pkts(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t pkts_r() {
    return ut30_stat_port_pkts(ctx_, MTL_SESSION_PORT_R);
  }
  /* defaults: pt=97, ssrc=0xCAFE, payload_len=192 */
  void feed_good(uint16_t seq, uint32_t ts, mtl_session_port port) {
    ut30_feed_pkt_via_wrapper(ctx_, seq, ts, port, 97, 0xCAFE, 192);
  }
};

TEST_F(St30RxErrPacketsTest, GoodPacketsNotCountedAsErr) {
  feed_good(0, 1000, MTL_SESSION_PORT_P);
  feed_good(1, 1001, MTL_SESSION_PORT_P);
  EXPECT_EQ(err_p(), 0u);
  EXPECT_EQ(pkts_p(), 2u);
}

TEST_F(St30RxErrPacketsTest, WrongPtCountedAsErr) {
  ut30_feed_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P, /*pt=*/200, 0xCAFE, 192);
  EXPECT_EQ(ut30_stat_wrong_pt(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St30RxErrPacketsTest, WrongSsrcCountedAsErr) {
  ut30_feed_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P, 97, /*ssrc=*/0xBAD, 192);
  EXPECT_EQ(ut30_stat_wrong_ssrc(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St30RxErrPacketsTest, WrongPayloadLenCountedAsErr) {
  ut30_feed_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P, 97, 0xCAFE,
                            /*payload_len=*/100);
  EXPECT_EQ(ut30_stat_len_mismatch(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St30RxErrPacketsTest, ErrPacketsEqualsSumOfReasons) {
  /* mix three distinct error kinds */
  ut30_feed_pkt_via_wrapper(ctx_, 0, 1000, MTL_SESSION_PORT_P, /*pt=*/200, 0xCAFE, 192);
  ut30_feed_pkt_via_wrapper(ctx_, 1, 1001, MTL_SESSION_PORT_P, 97, /*ssrc=*/0xBAD, 192);
  ut30_feed_pkt_via_wrapper(ctx_, 2, 1002, MTL_SESSION_PORT_P, 97, 0xCAFE,
                            /*len=*/100);

  uint64_t reasons = ut30_stat_wrong_pt(ctx_) + ut30_stat_wrong_ssrc(ctx_) +
                     ut30_stat_len_mismatch(ctx_);
  EXPECT_EQ(reasons, 3u);
  EXPECT_EQ(err_p(), reasons)
      << "every err_packets bump must be explainable by a per-reason counter";
}

/* Mixed burst: only the bad packet bumps err_packets. */
TEST_F(St30RxErrPacketsTest, MixedBurstOnlyBadCounted) {
  feed_good(0, 1000, MTL_SESSION_PORT_P);
  ut30_feed_pkt_via_wrapper(ctx_, 1, 1001, MTL_SESSION_PORT_P, /*pt=*/200, 0xCAFE, 192);
  feed_good(2, 1002, MTL_SESSION_PORT_P);

  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 2u);
}

/* Redundancy-filtered packets must not inflate err_packets. */
TEST_F(St30RxErrPacketsTest, RedundancyFilterDoesNotBumpErrCounter) {
  /* P delivers the frame first */
  feed_good(0, 1000, MTL_SESSION_PORT_P);
  feed_good(1, 1001, MTL_SESSION_PORT_P);
  /* R sends the same packets a beat later -- legitimate redundancy. */
  feed_good(0, 1000, MTL_SESSION_PORT_R);
  feed_good(1, 1001, MTL_SESSION_PORT_R);

  /* Sanity: the redundancy filter classified them. */
  EXPECT_EQ(ut30_stat_redundant(ctx_), 2u);
  /* Redundant packets must NOT be counted as port-level errors. */
  EXPECT_EQ(err_r(), 0u) << "redundant-filtered packets must not be classified as errors";
}

/* A healthy redundant stream must have zero unexplained err_packets. */
TEST_F(St30RxErrPacketsTest, HealthyRedundantStreamHasZeroUnexplainedErrors) {
  for (int i = 0; i < 5; i++) {
    feed_good(i, 1000 + i, MTL_SESSION_PORT_P);
    feed_good(i, 1000 + i, MTL_SESSION_PORT_R);
  }
  uint64_t reasons = ut30_stat_wrong_pt(ctx_) + ut30_stat_wrong_ssrc(ctx_) +
                     ut30_stat_len_mismatch(ctx_);
  EXPECT_EQ(reasons, 0u) << "no real error reasons in a healthy stream";
  EXPECT_EQ(err_r(), 0u)
      << "redundant-port err_packets must be zero when there are no real errors";
  EXPECT_EQ(err_p(), 0u);
}

/* Per stats_guide.md: a redundant packet lands in port[].packets (pre-redundancy)
 * and stat_pkts_redundant, but NOT in err_packets. */
TEST_F(St30RxErrPacketsTest, RedundantPacketCountedExactlyOnce) {
  /* prime session timestamp on P */
  feed_good(0, 2000, MTL_SESSION_PORT_P);
  /* one redundant packet on R (old timestamp gets filtered) */
  feed_good(99, 1000, MTL_SESSION_PORT_R);

  EXPECT_EQ(ut30_stat_redundant(ctx_), 1u) << "the R packet must be filtered";
  EXPECT_EQ(pkts_r(), 1u) << "redundant packet counts in pre-redundancy port[].packets";
  EXPECT_EQ(err_r(), 0u) << "redundant packet must not be counted as error";
  EXPECT_EQ(pkts_r() + err_r(), 1u) << "counted exactly once, not double-counted as err";
}
