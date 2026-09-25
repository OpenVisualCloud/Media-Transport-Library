/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The optional RFC4585 ssrc check on the RTCP TX NACK path. A nack for this
 * sender carries the session ssrc (RFC4585 6.1, "SSRC of media source"). With
 * the check on, a nack with any other ssrc is dropped before the fci walk.
 * The ring holds real mbufs and the packet ends on a PROT_NONE page, so a read
 * past the received bytes stops the test.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='MtRtcpTxNackSsrc*'
 */

#include <gtest/gtest.h>

#include "session/rtcp_attack_harness.h"

namespace {

constexpr uint32_t kSessionSsrc = 0x11223344;
constexpr uint32_t kOtherSsrc = 0x55667788;

constexpr uint16_t LenForFci(uint32_t n) {
  return (uint16_t)(2 + n);
}
constexpr size_t BytesForFci(uint32_t n) {
  return 12u + n * 4u;
}

class MtRtcpTxNackSsrc : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ASSERT_EQ(ut_rtk_init(), 0);
  }
  void TearDown() override {
    if (ctx_) {
      EXPECT_TRUE(ut_rtk_fifo_guard_intact(ctx_));
      EXPECT_EQ(ut_rtk_pool_avail(ctx_), ut_rtk_pool_avail_at_create(ctx_))
          << "copy mbufs leaked";
      ut_rtk_destroy(ctx_);
    }
  }
  void Make(int ring) {
    ctx_ = ut_rtk_create(ring, 1, 0);
    ASSERT_NE(ctx_, nullptr);
    ASSERT_EQ(ut_rtk_buffer(ctx_, 100, ring), 0);
  }
  ut_rtk_stats Nack1(uint16_t start, uint16_t follow) {
    ut_rtk_fci f = {start, follow};
    return ut_rtk_nack(ctx_, LenForFci(1), &f, 1, BytesForFci(1));
  }
  ut_rtk_ctx* ctx_ = nullptr;
};

/* The check is on and the nack carries the session ssrc: it is served. */
TEST_F(MtRtcpTxNackSsrc, MatchingSsrcRetransmits) {
  Make(64);
  ut_rtk_enable_ssrc_check(ctx_, kSessionSsrc);
  ut_rtk_set_nack_ssrc(ctx_, kSessionSsrc);
  auto s = Nack1(102, 2);
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.nack_received, 1u);
  EXPECT_EQ(s.sent, 3u);
  EXPECT_EQ(s.succ, 3u);
  EXPECT_EQ(ut_rtk_drop_ssrc(ctx_), 0u);
}

/* The check is on and the nack carries another ssrc: it is dropped, nothing is
 * sent, and it is not counted as a received nack. */
TEST_F(MtRtcpTxNackSsrc, MismatchedSsrcDropped) {
  Make(64);
  ut_rtk_enable_ssrc_check(ctx_, kSessionSsrc);
  ut_rtk_set_nack_ssrc(ctx_, kOtherSsrc);
  auto s = Nack1(102, 2);
  EXPECT_EQ(s.ret, -EIO);
  EXPECT_EQ(s.nack_received, 0u);
  EXPECT_EQ(s.sent, 0u);
  EXPECT_EQ(s.succ, 0u);
  EXPECT_EQ(ut_rtk_drop_ssrc(ctx_), 1u);
}

/* The check is off (the default): a nack with any ssrc is served, so the
 * behavior of an existing session does not change. */
TEST_F(MtRtcpTxNackSsrc, CheckDisabledIgnoresSsrc) {
  Make(64);
  ut_rtk_set_nack_ssrc(ctx_, kOtherSsrc);
  auto s = Nack1(102, 2);
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.nack_received, 1u);
  EXPECT_EQ(s.sent, 3u);
  EXPECT_EQ(ut_rtk_drop_ssrc(ctx_), 0u);
}

/* The ssrc check runs before the fci walk. A mismatched ssrc with a wild len
 * that would walk past the packet is dropped at the ssrc, so the fci pointer is
 * never read. The PROT_NONE page after the packet proves no read past it. */
TEST_F(MtRtcpTxNackSsrc, MismatchedSsrcWithWildLenNoOverread) {
  Make(64);
  ut_rtk_enable_ssrc_check(ctx_, kSessionSsrc);
  ut_rtk_set_nack_ssrc(ctx_, kOtherSsrc);
  ut_rtk_fci f = {0, 0};
  /* len 0xFFFF claims ~256 KB, recv is one fci: only the ssrc drop saves it. */
  auto s = ut_rtk_nack(ctx_, 0xFFFF, &f, 1, BytesForFci(1));
  EXPECT_EQ(s.ret, -EIO);
  EXPECT_EQ(s.sent, 0u);
  EXPECT_EQ(ut_rtk_drop_ssrc(ctx_), 1u);
}

}  // namespace
