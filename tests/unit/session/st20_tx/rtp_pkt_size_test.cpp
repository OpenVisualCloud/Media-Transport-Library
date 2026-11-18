/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * Pins the RTP-level packet size limits of the ST20 and ST22 TX sessions.
 * rtp_pkt_size may reach the video TX limit, ST_PKT_MAX_ETHER_BYTES (1494) minus the
 * 42-byte Ethernet/IPv4/UDP header MTL prepends, so that a 1396-byte ST 2022-6 packet
 * fits. put_mbuf() then takes any len from 1 up to the session's rtp_pkt_size.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*St20TxRtpPktSize*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>

#include "session/st20_tx_harness.h"

namespace {

constexpr uint16_t kUdpHdrLen = 42;
constexpr uint16_t kRtpPktSizeMax = 1494 - kUdpHdrLen;
/* 12 RTP + 8 HBRMT + 1376 payload */
constexpr uint16_t kSt2022_6PktSize = 1396;

/* Session create: tv_ops_check() / tv_st22_ops_check() on rtp_pkt_size, the param
 * selecting ST22 (true) or ST20 (false). */
class St20TxRtpPktSizeOpsTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
  }
};

/* The ST 2022-6 packet is the size users actually reported as rejected. */
TEST_P(St20TxRtpPktSizeOpsTest, AcceptsSt2022_6PacketSize) {
  EXPECT_EQ(0, ut_txv_rtp_ops_check(GetParam(), kSt2022_6PktSize));
}

/* The limit is inclusive: a packet of exactly the max fills the mbuf data room. */
TEST_P(St20TxRtpPktSizeOpsTest, AcceptsMaxPacketSize) {
  EXPECT_EQ(0, ut_txv_rtp_ops_check(GetParam(), kRtpPktSizeMax));
}

/* One byte past the max no longer fits ST_PKT_MAX_ETHER_BYTES once headers are on. */
TEST_P(St20TxRtpPktSizeOpsTest, RejectsMaxPacketSizePlusOne) {
  EXPECT_EQ(-EINVAL, ut_txv_rtp_ops_check(GetParam(), kRtpPktSizeMax + 1));
}

/* A zero size would size the mbuf pools and the rate limit from nothing. */
TEST_P(St20TxRtpPktSizeOpsTest, RejectsZeroPacketSize) {
  EXPECT_EQ(-EINVAL, ut_txv_rtp_ops_check(GetParam(), 0));
}

INSTANTIATE_TEST_SUITE_P(St20AndSt22, St20TxRtpPktSizeOpsTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "St22" : "St20";
                         });

/* put_mbuf(): st20_tx_put_mbuf() / st22_tx_put_mbuf() on a session created with the
 * max rtp_pkt_size, the param selecting ST22 (true) or ST20 (false). Every rejected
 * mbuf must go back to its pool, as the app no longer owns it. */
class St20TxRtpPktSizePutMbufTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    ASSERT_EQ(0, ut_txv_init());
    ctx_ = ut_txv_create();
    ASSERT_NE(nullptr, ctx_);
  }

  void TearDown() override {
    ut_txv_destroy(ctx_);
  }

  ut_txv_ctx* ctx_ = nullptr;
  bool freed_ = false;
  uint16_t queued_len_ = 0;
};

/* A len equal to the session's rtp_pkt_size is queued whole, MTL's header room added. */
TEST_P(St20TxRtpPktSizePutMbufTest, QueuesLenEqualToRtpPktSize) {
  EXPECT_EQ(0, ut_txv_put_mbuf(ctx_, GetParam(), kRtpPktSizeMax, kRtpPktSizeMax, &freed_,
                               &queued_len_));
  EXPECT_FALSE(freed_);
  EXPECT_EQ(kRtpPktSizeMax + kUdpHdrLen, queued_len_);
}

/* A 2022-6 packet on a session sized for it, the case users reported as rejected. */
TEST_P(St20TxRtpPktSizePutMbufTest, QueuesSt2022_6Packet) {
  EXPECT_EQ(0, ut_txv_put_mbuf(ctx_, GetParam(), kSt2022_6PktSize, kSt2022_6PktSize,
                               &freed_, &queued_len_));
  EXPECT_FALSE(freed_);
  EXPECT_EQ(kSt2022_6PktSize + kUdpHdrLen, queued_len_);
}

/* len may only shrink a packet: past rtp_pkt_size the pools and rate limit are short. */
TEST_P(St20TxRtpPktSizePutMbufTest, RejectsLenAboveRtpPktSize) {
  EXPECT_EQ(-EIO, ut_txv_put_mbuf(ctx_, GetParam(), kSt2022_6PktSize,
                                  kSt2022_6PktSize + 1, &freed_, &queued_len_));
  EXPECT_TRUE(freed_);
  EXPECT_EQ(0, queued_len_);
}

/* An empty packet carries no RTP header for the builder to read. */
TEST_P(St20TxRtpPktSizePutMbufTest, RejectsZeroLen) {
  EXPECT_EQ(-EIO,
            ut_txv_put_mbuf(ctx_, GetParam(), kRtpPktSizeMax, 0, &freed_, &queued_len_));
  EXPECT_TRUE(freed_);
  EXPECT_EQ(0, queued_len_);
}

INSTANTIATE_TEST_SUITE_P(St20AndSt22, St20TxRtpPktSizePutMbufTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "St22" : "St20";
                         });

}  // namespace
