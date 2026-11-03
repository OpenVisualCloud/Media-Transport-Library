/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * IPMX video packets carry DSCP AF42 (36), IPv4 TOS 0x90 with ECN 0
 * (TR-10-9 section 16); without the flag the TOS stays 0. The St22* cases set the
 * internal ST20_TX_FLAG_IPMX directly and check the tv_build_st22() packet; the
 * st22_tx_create() flag mapping is not reachable here.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxTxDscp*'
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "ipmx/ipmx_test_ops.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr uint8_t kAf42Tos = 36 << 2;
constexpr int kTosOffset = 1;

/* RFC 1071: a valid header sums to 0xffff in one's complement */
uint16_t OnesComplementSum(const uint8_t* hdr, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < len; i += 2) sum += (hdr[i] << 8) | hdr[i + 1];
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return sum;
}

class IpmxTxDscpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_txv_init(), 0);
    ctx_ = ut_txv_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_txv_destroy(ctx_);
  }
  ut_txv_ctx* ctx_ = nullptr;
};

TEST_F(IpmxTxDscpTest, PrimaryHeaderTosIsAf42) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 1), 0);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_P), kAf42Tos);
}

TEST_F(IpmxTxDscpTest, RedundantHeaderTosIsAf42) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 2), 0);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_P), kAf42Tos);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_R), kAf42Tos);
}

TEST_F(IpmxTxDscpTest, UpdateDestinationKeepsAf42) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 1), 0);
  uint16_t src_port = 0, dst_port = 0;
  ASSERT_EQ(ut_txv_update_dst(ctx_, 0, ut_ipmx::kIpmxPort, ut_ipmx::kIpmxPort + 2,
                              &src_port, &dst_port),
            0);
  EXPECT_EQ(dst_port, ut_ipmx::kIpmxPort + 2);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_P), kAf42Tos);
}

TEST_F(IpmxTxDscpTest, FlagOffHeaderTosIsZero) {
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 2), 0);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_P), 0);
  EXPECT_EQ(ut_txv_hdr_tos(ctx_, MTL_SESSION_PORT_R), 0);
}

TEST_F(IpmxTxDscpTest, SoftwareChecksumValidOnPrimaryPacket) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 2), 0);
  uint8_t hdr[UT_TXV_IPV4_HDR_LEN];
  ASSERT_EQ(ut_txv_build_ipv4_hdr(ctx_, MTL_SESSION_PORT_P, hdr), 0);
  EXPECT_EQ(hdr[kTosOffset], kAf42Tos);
  EXPECT_EQ(OnesComplementSum(hdr, UT_TXV_IPV4_HDR_LEN), 0xffff);
}

TEST_F(IpmxTxDscpTest, SoftwareChecksumValidOnRedundantPacket) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  ASSERT_EQ(ut_txv_init_hdrs(ctx_, 2), 0);
  uint8_t hdr[UT_TXV_IPV4_HDR_LEN];
  ASSERT_EQ(ut_txv_build_ipv4_hdr(ctx_, MTL_SESSION_PORT_R, hdr), 0);
  EXPECT_EQ(hdr[kTosOffset], kAf42Tos);
  EXPECT_EQ(OnesComplementSum(hdr, UT_TXV_IPV4_HDR_LEN), 0xffff);
}

/* TR-10-9 section 16 for JPEG XS: the st22 codestream packet carries AF42 */
TEST_F(IpmxTxDscpTest, St22PacketTosIsAf42) {
  ut_txv_add_flags(ctx_, ST20_TX_FLAG_IPMX);
  uint8_t hdr[UT_TXV_IPV4_HDR_LEN];
  ASSERT_EQ(ut_txv_build_st22_ipv4_hdr(ctx_, hdr), 0);
  EXPECT_EQ(hdr[kTosOffset], kAf42Tos);
  EXPECT_EQ(OnesComplementSum(hdr, UT_TXV_IPV4_HDR_LEN), 0xffff);
}

/* without the flag the st22 packet TOS stays 0, current MTL behaviour */
TEST_F(IpmxTxDscpTest, St22FlagOffPacketTosIsZero) {
  uint8_t hdr[UT_TXV_IPV4_HDR_LEN];
  ASSERT_EQ(ut_txv_build_st22_ipv4_hdr(ctx_, hdr), 0);
  EXPECT_EQ(hdr[kTosOffset], 0);
}

}  // namespace
