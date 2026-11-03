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

}  // namespace
