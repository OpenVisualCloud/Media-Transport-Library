/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * The CNI RX handler and the control-packet parsers it calls read only the received
 * bytes: every frame ends on the last byte before a PROT_NONE page, so a read past
 * its end faults.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*Cni*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "cni/mt_cni_harness.h"

namespace {

using Frame = std::vector<uint8_t>;

constexpr uint16_t kEtherIpv4 = 0x0800;
constexpr uint16_t kEtherVlan = 0x8100;
constexpr uint8_t kProtoIgmp = 2;
constexpr uint8_t kProtoUdp = 17;

void Put16(Frame& f, uint16_t v) {
  f.push_back(v >> 8);
  f.push_back(v & 0xff);
}

Frame Eth(uint16_t ether_type) {
  Frame f = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0, 0, 0, 0, 0x01};
  Put16(f, ether_type);
  return f;
}

/* An IPv4 header of ihl 32-bit words, zero options from 10.0.0.1 to 10.0.0.9. */
Frame Ipv4(uint8_t ihl, uint8_t proto) {
  Frame f = Eth(kEtherIpv4);
  f.push_back(0x40 | ihl);
  const Frame rest = {0, 0, 0, 0, 0, 0x40, 0, 64, proto, 0, 0, 10, 0, 0, 1, 10, 0, 0, 9};
  f.insert(f.end(), rest.begin(), rest.end());
  if (ihl > 5) f.resize(f.size() + static_cast<size_t>(ihl - 5) * 4);
  return f;
}

Frame Udp(uint8_t ihl, uint16_t src_port, uint16_t dst_port) {
  Frame f = Ipv4(ihl, kProtoUdp);
  Put16(f, src_port);
  Put16(f, dst_port);
  Put16(f, 0);
  Put16(f, 0);
  return f;
}

constexpr uint16_t kDhcpServerPort = 67;
constexpr uint16_t kDhcpClientPort = 68;
constexpr uint8_t kDhcpSubnetMask = 1;
constexpr uint8_t kDhcpRouter = 3;
constexpr uint8_t kDhcpLeaseTime = 51;
constexpr uint8_t kDhcpMessageType = 53;
constexpr uint8_t kDhcpServerId = 54;
constexpr uint8_t kDhcpPad = 0;
constexpr uint8_t kDhcpEnd = 255;
constexpr uint8_t kDhcpOffer = 2;
constexpr uint8_t kDhcpAck = 5;
const Frame kServerIp = {192, 168, 0, 1};
const Frame kNetmask = {255, 255, 255, 0};

/* A BOOTREPLY of the client's transaction, the options after the magic cookie. */
Frame Dhcp(const Frame& options) {
  Frame f = Udp(5, kDhcpServerPort, kDhcpClientPort);
  Frame bootp(240, 0);
  bootp[0] = 2; /* BOOTREPLY */
  for (int i = 0; i < 4; i++) bootp[4 + i] = UT_CNI_DHCP_XID >> (24 - 8 * i);
  const uint8_t cookie[4] = {0x63, 0x82, 0x53, 0x63};
  std::copy(cookie, cookie + 4, bootp.begin() + 236);
  f.insert(f.end(), bootp.begin(), bootp.end());
  f.insert(f.end(), options.begin(), options.end());
  return f;
}

Frame Option(uint8_t code, const Frame& value) {
  Frame o(2 + value.size());
  o[0] = code;
  o[1] = value.size();
  std::copy(value.begin(), value.end(), o.begin() + 2);
  return o;
}

Frame operator+(Frame a, const Frame& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

bool SameBytes(const uint8_t* got, const Frame& want) {
  return std::equal(want.begin(), want.end(), got);
}

const Frame kOffer = Option(kDhcpMessageType, {kDhcpOffer});
const Frame kAck = Option(kDhcpMessageType, {kDhcpAck});
const Frame kLease = Option(kDhcpLeaseTime, {0, 0, 0x0e, 0x10}); /* one hour */

class CniRxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = ut_cni_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_cni_destroy(ctx_);
  }
  int Rx(const Frame& f, size_t len) {
    return ut_cni_rx(ctx_, f.data(), len);
  }
  int Rx(const Frame& f) {
    return Rx(f, f.size());
  }

  ut_cni_ctx* ctx_ = nullptr;
};

class CniRxBounds : public CniRxTest {};

TEST_F(CniRxBounds, ShorterThanEthernetHeaderIsRejected) {
  EXPECT_LT(Rx(Eth(kEtherIpv4), 13), 0);
}

TEST_F(CniRxBounds, TruncatedVlanTagIsRejected) {
  Frame f = Eth(kEtherVlan);
  Put16(f, 0);
  Put16(f, kEtherIpv4);
  EXPECT_LT(Rx(f, f.size() - 1), 0);
}

TEST_F(CniRxBounds, TruncatedIpv4HeaderIsRejected) {
  Frame f = Ipv4(5, kProtoUdp);
  EXPECT_LT(Rx(f, f.size() - 1), 0);
}

TEST_F(CniRxBounds, Ipv4HeaderLengthBelowFixedHeaderIsRejected) {
  EXPECT_LT(Rx(Udp(4, 1000, 1000)), 0);
}

TEST_F(CniRxBounds, Ipv4OptionsPastFrameEndAreRejected) {
  Frame f = Ipv4(15, kProtoIgmp);
  EXPECT_LT(Rx(f, f.size() - 1), 0);
}

TEST_F(CniRxBounds, TruncatedUdpHeaderIsRejected) {
  Frame f = Udp(5, 1000, 1000);
  EXPECT_LT(Rx(f, f.size() - 1), 0);
}

TEST_F(CniRxBounds, UdpFrameIsHandled) {
  EXPECT_EQ(Rx(Udp(5, 1000, 1000)), 0);
}

class CniDhcpBounds : public CniRxTest {};

TEST_F(CniDhcpBounds, OfferIsParsed) {
  Rx(Dhcp(kOffer + Option(kDhcpServerId, kServerIp) + Frame{kDhcpEnd, 0, 0}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_REQUESTING);
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_server_ip(ctx_), kServerIp));
}

TEST_F(CniDhcpBounds, AckIsParsed) {
  Rx(Dhcp(kAck + Option(kDhcpSubnetMask, kNetmask) + kLease +
          Option(kDhcpServerId, kServerIp) + Frame{kDhcpEnd}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_BOUND);
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_netmask(ctx_), kNetmask));
}

TEST_F(CniDhcpBounds, OfferWithPadOptionsIsParsed) {
  Rx(Dhcp(kOffer + Frame{kDhcpPad} + Option(kDhcpServerId, kServerIp) +
          Frame{kDhcpPad, kDhcpEnd} + Frame(41, 0)));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_REQUESTING);
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_server_ip(ctx_), kServerIp));
}

TEST_F(CniDhcpBounds, AckWithPadBeforeEndIsParsed) {
  Rx(Dhcp(kAck + Option(kDhcpServerId, kServerIp) + Option(kDhcpSubnetMask, kNetmask) +
          kLease + Frame{kDhcpPad, kDhcpEnd} + Frame(41, 0)));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_BOUND);
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_server_ip(ctx_), kServerIp));
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_netmask(ctx_), kNetmask));
}

TEST_F(CniDhcpBounds, OfferWithoutEndOptionIsRejected) {
  Rx(Dhcp(kOffer + Option(kDhcpServerId, kServerIp)));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

TEST_F(CniDhcpBounds, AckWithoutEndOptionIsRejected) {
  Rx(Dhcp(kAck + Option(kDhcpSubnetMask, kNetmask) + kLease));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

TEST_F(CniDhcpBounds, ShorterThanFixedHeaderIsRejected) {
  Frame f = Dhcp({});
  Rx(f, f.size() - 1);
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

TEST_F(CniDhcpBounds, WithoutMessageTypeOptionIsRejected) {
  Rx(Dhcp({}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

TEST_F(CniDhcpBounds, MessageTypeWithoutValueIsRejected) {
  Rx(Dhcp(Frame{kDhcpMessageType, 0}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

/* An empty message type must not take the code of the next option, 5 (ACK), as its value.
 */
TEST_F(CniDhcpBounds, EmptyMessageTypeIsRejected) {
  Rx(Dhcp(Frame{kDhcpMessageType, 0, 5, 4, 192, 168, 0, 1} + kLease + Frame{kDhcpEnd}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

TEST_F(CniDhcpBounds, OptionLengthPastPacketEndIsRejected) {
  Rx(Dhcp(kOffer + Frame{kDhcpServerId, 4, 192, 168}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
  EXPECT_TRUE(SameBytes(ut_cni_dhcp_server_ip(ctx_), {0, 0, 0, 0}));
}

TEST_F(CniDhcpBounds, OptionWithoutLengthByteIsRejected) {
  Rx(Dhcp(kOffer + Frame{kDhcpServerId}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

/* Each of these options is read as 4 bytes; the lease after it binds an ACK taken anyway.
 */
class CniDhcpShortValue : public CniRxTest,
                          public ::testing::WithParamInterface<uint8_t> {};

TEST_P(CniDhcpShortValue, IsRejected) {
  Rx(Dhcp(kAck + Option(GetParam(), {0xff}) + kLease + Frame{kDhcpEnd}));
  EXPECT_EQ(ut_cni_dhcp_status(ctx_), UT_CNI_DHCP_DISCOVERING);
}

INSTANTIATE_TEST_SUITE_P(Option, CniDhcpShortValue,
                         ::testing::Values(kDhcpSubnetMask, kDhcpRouter, kDhcpLeaseTime,
                                           kDhcpServerId));

}  // namespace
