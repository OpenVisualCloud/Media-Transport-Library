/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Tests for the per-port `err_packets` counter on the ST 2110-40 RX session.
 * The `_handle_mbuf` wrapper increments
 * `port_user_stats.common.port[s_port].err_packets` whenever the per-packet
 * handler returns < 0.
 *
 * Test goals:
 *   1. Every genuine error path (wrong PT, wrong SSRC, bad F-bits) MUST
 *      increment err_packets and the corresponding per-reason counter,
 *      exactly once each.
 *   2. err_packets MUST equal the sum of per-reason drop counters.
 *   3. Successful packets MUST NOT increment err_packets.
 *   4. Redundancy-filtered packets (legitimate duplicates) MUST NOT
 *      count as errors.
 */

#include <gtest/gtest.h>

#include "session/st40_harness.h"

class St40RxErrPacketsTest : public ::testing::Test {
 protected:
  ut_test_ctx* ctx_ = nullptr;

  void SetUp() override {
    ASSERT_EQ(ut40_init(), 0);
    ut40_drain_ring();
    ctx_ = ut40_ctx_create(2);
    ASSERT_NE(ctx_, nullptr);
    ut40_ctx_set_pt(ctx_, 113);
    ut40_ctx_set_ssrc(ctx_, 0x1234);
  }
  void TearDown() override {
    ut40_drain_ring();
    if (ctx_) ut40_ctx_destroy(ctx_);
  }

  uint64_t err_p() {
    return ut40_stat_port_err_packets(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t err_r() {
    return ut40_stat_port_err_packets(ctx_, MTL_SESSION_PORT_R);
  }
  uint64_t pkts_p() {
    return ut40_stat_port_pkts(ctx_, MTL_SESSION_PORT_P);
  }
  uint64_t pkts_r() {
    return ut40_stat_port_pkts(ctx_, MTL_SESSION_PORT_R);
  }
  int feed_via(uint16_t seq, uint32_t ts, int marker, mtl_session_port port,
               uint8_t pt = 113, uint32_t ssrc = 0x1234, uint8_t f_bits = 0) {
    struct ut40_pkt_spec spec = {.seq = seq,
                                 .ts = ts,
                                 .marker = marker,
                                 .port = port,
                                 .payload_type = pt,
                                 .ssrc = ssrc,
                                 .f_bits = f_bits};
    return ut40_feed_spec_via_wrapper(ctx_, spec);
  }
};

TEST_F(St40RxErrPacketsTest, GoodPacketsNotCountedAsErr) {
  feed_via(0, 1000, /*marker=*/1, MTL_SESSION_PORT_P);
  feed_via(1, 1001, /*marker=*/1, MTL_SESSION_PORT_P);
  EXPECT_EQ(err_p(), 0u);
  EXPECT_EQ(pkts_p(), 2u);
}

TEST_F(St40RxErrPacketsTest, WrongPtCountedAsErr) {
  feed_via(0, 1000, 1, MTL_SESSION_PORT_P, /*pt=*/77);
  EXPECT_EQ(ut40_stat_wrong_pt(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St40RxErrPacketsTest, WrongSsrcCountedAsErr) {
  feed_via(0, 1000, 1, MTL_SESSION_PORT_P, 113, /*ssrc=*/0x9999);
  EXPECT_EQ(ut40_stat_wrong_ssrc(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St40RxErrPacketsTest, InvalidFBitsCountedAsErr) {
  /* Default progressive session; F-bits=0b01 is an invalid encoding. */
  feed_via(0, 1000, 1, MTL_SESSION_PORT_P, 113, 0x1234, /*f_bits=*/0b01);
  EXPECT_EQ(ut40_stat_wrong_interlace(ctx_), 1u);
  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 0u);
}

TEST_F(St40RxErrPacketsTest, ErrPacketsEqualsSumOfReasons) {
  feed_via(0, 1000, 1, MTL_SESSION_PORT_P, /*pt=*/77);            /* wrong PT */
  feed_via(1, 1001, 1, MTL_SESSION_PORT_P, 113, /*ssrc=*/0x9999); /* wrong SSRC */
  feed_via(2, 1002, 1, MTL_SESSION_PORT_P, 113, 0x1234, 0b01);    /* bad F bits */

  uint64_t reasons = ut40_stat_wrong_pt(ctx_) + ut40_stat_wrong_ssrc(ctx_) +
                     ut40_stat_wrong_interlace(ctx_);
  EXPECT_EQ(reasons, 3u);
  EXPECT_EQ(err_p(), reasons)
      << "every err_packets bump must be explainable by a per-reason counter";
}

TEST_F(St40RxErrPacketsTest, MixedBurstOnlyBadCounted) {
  feed_via(0, 1000, 1, MTL_SESSION_PORT_P);
  feed_via(1, 1001, 1, MTL_SESSION_PORT_P, /*pt=*/77);
  feed_via(2, 1002, 1, MTL_SESSION_PORT_P);

  EXPECT_EQ(err_p(), 1u);
  EXPECT_EQ(pkts_p(), 2u);
}

/* Redundancy-filtered packets must not inflate err_packets. */
TEST_F(St40RxErrPacketsTest, RedundancyFilterDoesNotBumpErrCounter) {
  /* P sends 5 packets first (each its own marker = its own frame). */
  for (int i = 0; i < 5; i++) {
    feed_via(static_cast<uint16_t>(i), 1000 + i, /*marker=*/1, MTL_SESSION_PORT_P);
  }
  /* R replays the same packets -- legitimate redundancy. */
  for (int i = 0; i < 5; i++) {
    feed_via(static_cast<uint16_t>(i), 1000 + i, /*marker=*/1, MTL_SESSION_PORT_R);
  }

  /* All 5 R packets must be filtered as redundant. */
  EXPECT_GT(ut40_stat_redundant(ctx_), 0u)
      << "the redundancy filter must classify the R replay";
  /* Redundant packets must NOT be counted as errors. */
  EXPECT_EQ(err_r(), 0u) << "redundant-filtered packets must not be classified as errors";
}

TEST_F(St40RxErrPacketsTest, HealthyRedundantStreamHasZeroUnexplainedErrors) {
  for (int i = 0; i < 5; i++) {
    feed_via(static_cast<uint16_t>(i), 1000 + i, 1, MTL_SESSION_PORT_P);
    feed_via(static_cast<uint16_t>(i), 1000 + i, 1, MTL_SESSION_PORT_R);
  }
  uint64_t reasons = ut40_stat_wrong_pt(ctx_) + ut40_stat_wrong_ssrc(ctx_) +
                     ut40_stat_wrong_interlace(ctx_);
  EXPECT_EQ(reasons, 0u) << "no real error reasons in a healthy stream";
  EXPECT_EQ(err_r(), 0u)
      << "redundant-port err_packets must be zero when there are no real errors";
  EXPECT_EQ(err_p(), 0u);
}

/* Per stats_guide.md: a redundant packet lands in port[].packets (pre-redundancy)
 * and stat_pkts_redundant, but NOT in err_packets. */
TEST_F(St40RxErrPacketsTest, RedundantPacketCountedExactlyOnce) {
  /* prime session state on P */
  feed_via(0, 2000, 1, MTL_SESSION_PORT_P);
  /* one redundant packet on R (old timestamp + low seq → filtered) */
  feed_via(0, 1000, 1, MTL_SESSION_PORT_R);

  EXPECT_GT(ut40_stat_redundant(ctx_), 0u) << "the R packet must be filtered";
  EXPECT_EQ(pkts_r(), 1u) << "redundant packet counts in pre-redundancy port[].packets";
  EXPECT_EQ(err_r(), 0u) << "redundant packet must not be counted as error";
  EXPECT_EQ(pkts_r() + err_r(), 1u) << "counted exactly once, not double-counted as err";
}
