/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mt_ptp_parse() and the PTP RX queue handler read only the received bytes: each
 * message ends on the last byte before a PROT_NONE page, so a read past its end faults.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*Ptp*Bounds*'
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "ptp/ptp_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;

constexpr size_t kHeaderLen = 34;
constexpr size_t kUdpFrameHeaderLen = 42; /* Ethernet, IPv4 and UDP */
constexpr uint8_t kDelayReq = 1;
constexpr uint8_t kFollowUp = 8;
constexpr uint8_t kDelayResp = 9;
constexpr uint8_t kAnnounce = 11;
constexpr uint8_t kSeq = 7;
const Bytes kOurPortId = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

/* A message from the master in sequence kSeq, with a nonzero timestamp after the header
 * and kOurPortId where a delay_resp names the requesting port. */
Bytes Message(uint8_t type, size_t len) {
  Bytes m(64, 0);
  m[0] = type;
  m[1] = 2;
  m[31] = kSeq;
  std::fill(m.begin() + kHeaderLen, m.begin() + kHeaderLen + 10, 1);
  std::copy(kOurPortId.begin(), kOurPortId.end(), m.begin() + 44);
  m.resize(len);
  return m;
}

Bytes UdpFrame(const Bytes& msg) {
  Bytes f(kUdpFrameHeaderLen + msg.size(), 0);
  f[12] = 0x08; /* IPv4 */
  f[14] = 0x45;
  f[23] = 17; /* UDP */
  std::copy(msg.begin(), msg.end(), f.begin() + kUdpFrameHeaderLen);
  return f;
}

bool T1Set(const ut_ptp_ctx* ctx) {
  return ut_ptp_t1(ctx) != 0;
}

bool T4Set(const ut_ptp_ctx* ctx) {
  return ut_ptp_t4(ctx) != 0;
}

struct PtpMessage {
  uint8_t type;
  size_t len;
  bool (*parsed)(const ut_ptp_ctx* ctx);
};

void PrintTo(const PtpMessage& p, std::ostream* os) {
  *os << "type " << +p.type << ", " << p.len << " bytes";
}

class PtpParseBoundsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_ptp_init(), 0);
    ctx_ = ut_ptp_create();
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_ptp_destroy(ctx_);
  }
  int Parse(const Bytes& m, size_t len) {
    return ut_ptp_parse(ctx_, m.data(), len);
  }
  int RxqHandle(const Bytes& f, size_t len) {
    return ut_ptp_rxq_handle(ctx_, f.data(), len);
  }

  ut_ptp_ctx* ctx_ = nullptr;
};

/* The master is known unless the message is an announce, and the cycle waits for the
 * follow_up and delay_resp of kSeq; its delay_req is out, so a follow_up sends none. */
class PtpParseBounds : public PtpParseBoundsTest,
                       public ::testing::WithParamInterface<PtpMessage> {
 protected:
  void SetUp() override {
    PtpParseBoundsTest::SetUp();
    ut_ptp_set_master_initialized(ctx_, GetParam().type != kAnnounce);
    ut_ptp_set_t2_sequence_id(ctx_, kSeq);
    ut_ptp_set_t3_sequence_id(ctx_, kSeq);
    ut_ptp_set_t3(ctx_, 1);
    ut_ptp_set_our_port_id(ctx_, kOurPortId.data());
  }
};

TEST_P(PtpParseBounds, CompleteMessageIsParsed) {
  const PtpMessage& p = GetParam();
  EXPECT_EQ(Parse(Message(p.type, p.len), p.len), 0);
  EXPECT_TRUE(p.parsed(ctx_));
}

TEST_P(PtpParseBounds, MessageOneByteShortIsRejected) {
  const PtpMessage& p = GetParam();
  EXPECT_LT(Parse(Message(p.type, p.len), p.len - 1), 0);
  EXPECT_FALSE(p.parsed(ctx_));
}

/* a follow_up stores t1, a delay_resp t4, and an announce sets the master */
INSTANTIATE_TEST_SUITE_P(
    Type, PtpParseBounds,
    ::testing::Values(PtpMessage{kFollowUp, 44, T1Set}, PtpMessage{kDelayResp, 54, T4Set},
                      PtpMessage{kAnnounce, 64, ut_ptp_master_initialized}));

class PtpHeaderBounds : public PtpParseBoundsTest {};

TEST_F(PtpHeaderBounds, ShorterThanHeaderIsRejected) {
  ut_ptp_set_master_initialized(ctx_, true);
  EXPECT_LT(Parse(Message(kDelayReq, kHeaderLen), kHeaderLen - 1), 0);
}

class PtpRxqBounds : public PtpParseBoundsTest {};

TEST_F(PtpRxqBounds, ShorterThanUdpFrameHeaderIsRejected) {
  Bytes f = UdpFrame({});
  EXPECT_LT(RxqHandle(f, f.size() - 1), 0);
}

TEST_F(PtpRxqBounds, AnnounceIsParsed) {
  Bytes f = UdpFrame(Message(kAnnounce, 64));
  RxqHandle(f, f.size());
  EXPECT_TRUE(ut_ptp_master_initialized(ctx_));
}

TEST_F(PtpRxqBounds, TruncatedAnnounceIsRejected) {
  Bytes f = UdpFrame(Message(kAnnounce, 64));
  RxqHandle(f, f.size() - 1);
  EXPECT_FALSE(ut_ptp_master_initialized(ctx_));
}

}  // namespace
