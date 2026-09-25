/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Attacks on the RTCP TX NACK path after the bounds fix. Unlike
 * parse_nack_bounds_test.cpp, the ring holds real mbufs, the packet ends on a
 * PROT_NONE page, and the stack depth of the retransmit call is measured.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='RtcpNackAttack*'
 */

#include <gtest/gtest.h>

#include <vector>

#include "session/rtcp_attack_harness.h"

namespace {

constexpr uint16_t kRetransmitBit = 0x4000; /* ST20_RETRANSMIT */

constexpr uint16_t LenForFci(uint32_t n) {
  return (uint16_t)(2 + n);
}
constexpr size_t BytesForFci(uint32_t n) {
  return 12u + n * 4u;
}

class RtcpNackAttack : public ::testing::Test {
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
  void Make(int ring, unsigned int pool_n = 0) {
    ctx_ = ut_rtk_create(ring, 1, pool_n);
    ASSERT_NE(ctx_, nullptr);
  }
  ut_rtk_stats Nack1(uint16_t start, uint16_t follow) {
    ut_rtk_fci f = {start, follow};
    return ut_rtk_nack(ctx_, LenForFci(1), &f, 1, BytesForFci(1));
  }
  ut_rtk_ctx* ctx_ = nullptr;
};

/* ── Retransmit path with a filled ring ─────────────────────────────────── */

TEST_F(RtcpNackAttack, WellFormedRetransmitsExactSeqs) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 100, 64), 0);
  auto s = Nack1(102, 2);
  EXPECT_EQ(s.ret, 0);
  ASSERT_EQ(s.sent, 3u);
  for (unsigned i = 0; i < 3; i++) {
    EXPECT_EQ(ut_rtk_sent_seq(ctx_, i), 102 + i);
    EXPECT_TRUE(ut_rtk_sent_row_length(ctx_, i) & kRetransmitBit);
  }
  EXPECT_EQ(s.succ, 3u);
  EXPECT_EQ(s.fail, 0u);
}

TEST_F(RtcpNackAttack, SeqWrapAcrossZero) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 65530, 12), 0); /* 65530 .. 5 */
  auto s = Nack1(65534, 3);
  ASSERT_EQ(s.sent, 4u);
  EXPECT_EQ(ut_rtk_sent_seq(ctx_, 0), 65534);
  EXPECT_EQ(ut_rtk_sent_seq(ctx_, 1), 65535);
  EXPECT_EQ(ut_rtk_sent_seq(ctx_, 2), 0);
  EXPECT_EQ(ut_rtk_sent_seq(ctx_, 3), 1);
}

/* The ring overwrote its oldest entries, so read_idx is not 0 and a read
 * wraps the fifo data array. The guard slots must stay untouched. */
TEST_F(RtcpNackAttack, FifoIndexWrapReadsInsideData) {
  Make(16);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 16 + 10), 0); /* ring holds 10 .. 25 */
  auto s = Nack1(20, 5);
  ASSERT_EQ(s.sent, 6u);
  for (unsigned i = 0; i < 6; i++) EXPECT_EQ(ut_rtk_sent_seq(ctx_, i), 20 + i);
}

TEST_F(RtcpNackAttack, StartBeforeHeadIsObsolete) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 1000, 64), 0);
  auto s = Nack1(990, 4);
  EXPECT_EQ(s.sent, 0u);
  EXPECT_EQ(s.fail_obsolete, 5u);
}

TEST_F(RtcpNackAttack, RangePastTailIsReadFail) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 1000, 64), 0);
  auto s = Nack1(1060, 10); /* 1060 .. 1070, tail is 1063 */
  EXPECT_EQ(s.sent, 0u);
  EXPECT_EQ(s.fail_read, 11u);
}

/* start half the seq space away: rtp_seq_num_cmp flips, diff is huge. */
TEST_F(RtcpNackAttack, StartHalfSeqSpaceAwayIsRefused) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  for (uint16_t start : {(uint16_t)32767, (uint16_t)32768, (uint16_t)32769}) {
    auto s = Nack1(start, 0);
    EXPECT_EQ(s.sent, 0u) << "start " << start;
  }
}

TEST_F(RtcpNackAttack, CopyPoolExhaustedNoLeak) {
  Make(64, /*pool_n=*/4);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  auto s = Nack1(0, 9); /* bulk 10, only 4 copies */
  EXPECT_EQ(s.sent, 4u);
  EXPECT_EQ(s.fail_nobuf, 6u);
  EXPECT_EQ(s.fail, 6u);
}

TEST_F(RtcpNackAttack, PartialBurstNoLeak) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  ut_rtk_set_burst_limit(ctx_, 3);
  auto s = Nack1(0, 9);
  EXPECT_EQ(s.sent, 3u);
  EXPECT_EQ(s.fail_burst, 7u);
  EXPECT_EQ(s.succ, 3u);
  EXPECT_EQ(s.fail, 7u);
}

/* ── Claims of the fix ──────────────────────────────────────────────────── */

/* A follow longer than the ring holds is refused, not shortened to the ring. */
TEST_F(RtcpNackAttack, FollowPastRingIsRefusedNotShortened) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  auto s = Nack1(0, 0xFFFE);
  EXPECT_EQ(s.sent, 0u) << "a 65535-packet request retransmitted " << s.sent;
  EXPECT_EQ(s.fail, 65535u);
}

/* follow 0xFFFF asks for 65536 packets. It must not wrap to a 0-packet request. */
TEST_F(RtcpNackAttack, FollowAllOnesIsRefusedNotWrapped) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  auto s = Nack1(0, 0xFFFF);
  EXPECT_EQ(s.sent, 0u);
  EXPECT_EQ(s.fail, 65536u);
}

/* One 1036-byte datagram, 256 FCIs, each asks for the full ring. The first
 * FCI uses the budget of the NACK, the other 255 get nothing. */
TEST_F(RtcpNackAttack, OneDatagramRetransmitsAtMostTheRing) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  std::vector<ut_rtk_fci> f(256, ut_rtk_fci{0, 63});
  auto s = ut_rtk_nack(ctx_, LenForFci(256), f.data(), 256, BytesForFci(256));
  EXPECT_EQ(s.ret, 0);
  EXPECT_LE(s.sent, 64u) << "one datagram gave " << s.sent << " packet copies";
}

/* Disjoint ranges that together fit the ring are all served. */
TEST_F(RtcpNackAttack, DisjointRangesInsideBudgetAllServed) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  ut_rtk_fci f[3] = {{0, 9}, {20, 19}, {50, 13}}; /* 10 + 20 + 14 = 44 */
  auto s = ut_rtk_nack(ctx_, LenForFci(3), f, 3, BytesForFci(3));
  EXPECT_EQ(s.sent, 44u);
  EXPECT_EQ(s.fail, 0u);
}

/* A range longer than one chunk crosses the chunk edges in seq order. */
TEST_F(RtcpNackAttack, RangeLongerThanChunkKeepsSeqOrder) {
  Make(256);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 65500, 256), 0); /* wraps at 65535 */
  auto s = Nack1(65510, 99);
  ASSERT_EQ(s.sent, 100u);
  for (unsigned i = 0; i < 100; i++)
    EXPECT_EQ(ut_rtk_sent_seq(ctx_, i), (uint16_t)(65510 + i)) << i;
}

/* A full retransmit of a big ring does not grow the stack with the range:
 * before the fix, two VLAs of 4096 pointers took 64 KiB. */
TEST_F(RtcpNackAttack, StackDepthFullRingRetransmit) {
  Make(4096);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 4096), 0);
  auto s = Nack1(0, 4095);
  EXPECT_EQ(s.sent, 4096u);
  EXPECT_LT(s.max_stack_depth, 16u * 1024) << s.max_stack_depth;
}

/* ── Over-read with the packet end on a PROT_NONE page ──────────────────── */

TEST_F(RtcpNackAttack, RuntsAtPageEndDoNotFault) {
  Make(64);
  uint8_t hdr[12] = {0x80, 205, 0, 3, 0, 0, 0, 0, 'I', 'M', 'T', 'L'};
  for (size_t len : {(size_t)0, (size_t)1, (size_t)4, (size_t)11}) {
    auto s = ut_rtk_raw(ctx_, hdr, len);
    EXPECT_LT(s.ret, 0) << len;
    EXPECT_EQ(s.nack_received, 0u) << len;
  }
}

TEST_F(RtcpNackAttack, ExactLengthAtPageEndDoesNotFault) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  std::vector<ut_rtk_fci> f(5, ut_rtk_fci{0, 0});
  auto s = ut_rtk_nack(ctx_, LenForFci(5), f.data(), 5, BytesForFci(5));
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.sent, 5u);
}

/* recv_len not a multiple of 4: a trailing partial FCI must not be walked. */
TEST_F(RtcpNackAttack, TrailingPartialFciNotWalked) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  ut_rtk_fci f = {0, 0};
  for (size_t extra : {(size_t)1, (size_t)2, (size_t)3}) {
    auto ok = ut_rtk_nack(ctx_, LenForFci(0), &f, 0, 12 + extra);
    EXPECT_EQ(ok.ret, 0);
    EXPECT_EQ(ok.sent, 0u);
    auto bad = ut_rtk_nack(ctx_, LenForFci(1), &f, 0, 12 + extra);
    EXPECT_LT(bad.ret, 0);
  }
}

/* Received bytes past the declared size (Ethernet padding) are not walked. */
TEST_F(RtcpNackAttack, PaddingPastDeclaredLenNotWalked) {
  Make(64);
  ASSERT_EQ(ut_rtk_buffer(ctx_, 0, 64), 0);
  std::vector<ut_rtk_fci> f(5, ut_rtk_fci{0, 0});
  auto s = ut_rtk_nack(ctx_, LenForFci(1), f.data(), 5, BytesForFci(5));
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.sent, 1u);
}

/* Largest declared len with every byte present: capped, no fault. */
TEST_F(RtcpNackAttack, MaxLenFieldFullyReceivedIsCapped) {
  Make(64);
  const size_t bytes = (0xFFFFu + 1) * 4;
  auto s = ut_rtk_nack(ctx_, 0xFFFF, nullptr, 0, bytes);
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.fail, 256u); /* empty ring: one fail per walked FCI (start 0 follow 0) */
}

/* A non-NACK ptype with a wild len: nothing past the header is read. */
TEST_F(RtcpNackAttack, NonNackTypeWithWildLenReadsNothing) {
  Make(64);
  uint8_t hdr[12] = {0x80, 200, 0xFF, 0xFF, 0, 0, 0, 0, 'I', 'M', 'T', 'L'};
  auto s = ut_rtk_raw(ctx_, hdr, sizeof(hdr));
  EXPECT_EQ(s.ret, 0);
  EXPECT_EQ(s.nack_received, 0u);
}

}  // namespace
