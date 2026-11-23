/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The RTCP TX ring buffers the sent RTP pkts in seq order, and refuses a pkt after a
 * seq gap while it holds any. A link resync or a TX queue recovery frees pkts that have
 * seqs, so it restarts the ring: mt_rtcp_tx_buffer_reset() gives back each pkt, and the
 * next pkt starts a new run that a NACK finds.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='RtcpTxBufferResetTest.*'
 */

#include <gtest/gtest.h>

#include "session/rtcp_attack_harness.h"

namespace {

constexpr int kRing = 128;
constexpr uint16_t kFirst = 100;
constexpr uint16_t kAfterGap = 300;

class RtcpTxBufferResetTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ASSERT_EQ(ut_rtk_init(), 0);
  }
  void SetUp() override {
    ctx_ = ut_rtk_create(kRing, 1, 0);
    ASSERT_NE(ctx_, nullptr);
    src_avail_ = ut_rtk_src_avail(ctx_);
    ASSERT_EQ(ut_rtk_buffer(ctx_, kFirst, 64), 0);
  }
  void TearDown() override {
    ut_rtk_destroy(ctx_);
  }
  ut_rtk_stats Nack(uint16_t start, uint16_t follow) {
    ut_rtk_fci f = {start, follow};
    return ut_rtk_nack(ctx_, 3, &f, 1, 16);
  }
  ut_rtk_ctx* ctx_ = nullptr;
  unsigned int src_avail_ = 0;
};

TEST_F(RtcpTxBufferResetTest, PktAfterASeqGapIsRefused) {
  EXPECT_LT(ut_rtk_buffer(ctx_, kAfterGap, 1), 0);
}

TEST_F(RtcpTxBufferResetTest, ResetGivesBackEachPkt) {
  ut_rtk_reset(ctx_);

  EXPECT_EQ(ut_rtk_src_avail(ctx_), src_avail_);
}

TEST_F(RtcpTxBufferResetTest, EveryPktAfterAResetIsTaken) {
  ut_rtk_reset(ctx_);

  for (uint16_t i = 0; i < 2 * kRing; i++)
    ASSERT_EQ(ut_rtk_buffer(ctx_, kAfterGap + i, 1), 0) << "pkt " << i;
}

TEST_F(RtcpTxBufferResetTest, NackForAPktAfterTheResetRetransmitsIt) {
  ut_rtk_reset(ctx_);
  ASSERT_EQ(ut_rtk_buffer(ctx_, kAfterGap, 64), 0);

  ut_rtk_stats s = Nack(kAfterGap + 10, 2);

  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.sent, 3u);
  EXPECT_EQ(s.succ, 3u);
}

}  // namespace
