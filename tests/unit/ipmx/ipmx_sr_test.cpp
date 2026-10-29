/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * The IPMX video Sender Report (TR-10-1 sections 8.7, 8.8.2): an RTCP SR with the IPMX
 * Info Block followed by an SDES CNAME (TR-10-9 section 8), once per frame, to the media
 * port + 1 on the session's own queue right before packet 0.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxSr*'
 */

#include <gtest/gtest.h>
#include <rte_mempool.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/ut_common.h"
#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr_harness.h"
#include "st_api.h"

namespace {

constexpr uint32_t kSsrc = 0x12345678;
constexpr uint16_t kMediaPort = 20000;
constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint64_t kNsPerS = 1000000000ull;
constexpr uint64_t kTaiNs = 1700000000ull * kNsPerS + 123456789;
constexpr uint32_t kRtpTs = 0xa1b2c3d4;
constexpr uint32_t k90k = 90000;
constexpr int kPortP = 0;
constexpr int kPortR = 1;
constexpr int kPktTypeNormal = 0;
/* SR 28, IPMX Info Block 176 */
constexpr uint32_t kSdesOffset = 204;
constexpr uint32_t kSrPktLen = UT_SR_UDP_HDR_LEN + kSdesOffset + 24; /* "192.168.1.10" */
/* Ethernet, IPv4, UDP and the 12 octet RTP header of an st20 packet */
constexpr uint32_t kSt20HdrLen = 62;
constexpr uint32_t kRtpPayloadOverhead = 54;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint16_t Be16(const uint8_t* p) {
  return (uint16_t)(p[0] << 8 | p[1]);
}

/* RFC 1071: a valid header sums to 0xffff in one's complement */
uint16_t OnesComplementSum(const uint8_t* hdr, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < len; i += 2) sum += (hdr[i] << 8) | hdr[i + 1];
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return sum;
}

class IpmxSrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, kSsrc, kMediaPort);
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  void Attach() {
    ASSERT_EQ(ut_sr_attach(ctx_), 0);
    ASSERT_TRUE(ut_sr_attached(ctx_));
  }
  /* the RTCP part of the last SR sent */
  std::vector<uint8_t> LastRtcp() {
    uint8_t pkt[UT_SR_PKT_MAX];
    uint16_t len = ut_sr_last_sr(ctx_, pkt);
    if (len <= UT_SR_UDP_HDR_LEN) return {};
    return std::vector<uint8_t>(pkt + UT_SR_UDP_HDR_LEN, pkt + len);
  }
  struct ut_sr_counts Counts() {
    struct ut_sr_counts c;
    ut_sr_counts(ctx_, &c);
    return c;
  }
  int SrsOnWire() {
    int n = 0;
    for (int i = 0; i < ut_sr_wire_count(ctx_); i++)
      if (ut_sr_wire_at(ctx_, i).kind == UT_SR_WIRE_SR) n++;
    return n;
  }
  ut_sr_ctx* ctx_ = nullptr;
};

TEST_F(IpmxSrTest, BuildsSrThenInfoBlockThenSdesCnameGolden) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  const uint8_t sr_golden[] = {
      0x80, 0xc8, 0x00, 0x32, /* V=2 RC=0 PT=SR length 50 */
      0x12, 0x34, 0x56, 0x78, /* SSRC */
      0x65, 0x53, 0xf1, 0x00, /* 1700000000 s */
      0x07, 0x5b, 0xcd, 0x15, /* 123456789 ns */
      0xa1, 0xb2, 0xc3, 0xd4, /* RTP timestamp */
      0x00, 0x00, 0x00, 0x00, /* packet count */
      0x00, 0x00, 0x00, 0x00, /* octet count */
  };
  const uint8_t sdes_golden[] = {
      0x81, 0xca, 0x00, 0x05,                            /* V=2 SC=1 PT=SDES length 5 */
      0x12, 0x34, 0x56, 0x78,                            /* SSRC */
      0x01, 0x0c, '1',  '9',  '2', '.',  '1',  '6', '8', /* CNAME, 12 octets */
      '.',  '1',  '.',  '1',  '0', 0x00, 0x00,           /* two nulls to the word end */
  };
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_video_info(ctx_, &info), 0);
  uint8_t block[IPMX_INFO_BLOCK_LEN];
  ipmx_info_block_build(&info, block);

  uint8_t rtcp[UT_SR_PKT_MAX];
  ASSERT_EQ(ut_sr_build(ctx_, rtcp, kTaiNs, kRtpTs), kSdesOffset + sizeof(sdes_golden));
  EXPECT_EQ(0, memcmp(rtcp, sr_golden, sizeof(sr_golden)));
  EXPECT_EQ(0, memcmp(rtcp + 28, block, sizeof(block)));
  EXPECT_EQ(Be16(rtcp + 28), 0x5831);
  EXPECT_EQ(0, memcmp(rtcp + kSdesOffset, sdes_golden, sizeof(sdes_golden)));
}

class IpmxSrCnameTest : public ::testing::TestWithParam<std::vector<uint8_t>> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
  }
};

TEST_P(IpmxSrCnameTest, CnameNullTerminatedToWordBoundary) {
  const std::vector<uint8_t>& ip = GetParam();
  const std::string cname = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." +
                            std::to_string(ip[2]) + "." + std::to_string(ip[3]);
  ut_sr_ctx* ctx = ut_sr_create(ip.data(), kSsrc, kMediaPort);
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(ut_sr_attach(ctx), 0);
  uint8_t rtcp[UT_SR_PKT_MAX];
  uint16_t len = ut_sr_build(ctx, rtcp, kTaiNs, kRtpTs);
  ut_sr_destroy(ctx);

  const uint8_t* sdes = rtcp + kSdesOffset;
  const size_t item_end = 8 + 2 + cname.size();
  const size_t sdes_len = (item_end + 1 + 3) / 4 * 4; /* at least one null */
  ASSERT_EQ(len, kSdesOffset + sdes_len);
  EXPECT_EQ(Be16(sdes + 2), sdes_len / 4 - 1);
  EXPECT_EQ(sdes[8], 1); /* CNAME */
  EXPECT_EQ(sdes[9], cname.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(sdes + 10), cname.size()), cname);
  for (size_t i = item_end; i < sdes_len; i++) EXPECT_EQ(sdes[i], 0) << "octet " << i;
}

INSTANTIATE_TEST_SUITE_P(Lengths7To15, IpmxSrCnameTest,
                         ::testing::Values(std::vector<uint8_t>{1, 2, 3, 4},
                                           std::vector<uint8_t>{10, 2, 3, 4},
                                           std::vector<uint8_t>{10, 20, 3, 4},
                                           std::vector<uint8_t>{10, 20, 30, 4},
                                           std::vector<uint8_t>{10, 20, 30, 40},
                                           std::vector<uint8_t>{100, 20, 30, 40},
                                           std::vector<uint8_t>{100, 200, 30, 40},
                                           std::vector<uint8_t>{100, 200, 250, 40},
                                           std::vector<uint8_t>{100, 200, 250, 255}));

TEST_F(IpmxSrTest, NtpIsPtpTruncatedSecondsAndNanoseconds) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  constexpr uint64_t kSec = 0x123456789ull; /* above 32 bits, keeps the low word */
  uint8_t rtcp[UT_SR_PKT_MAX];
  ut_sr_build(ctx_, rtcp, kSec * kNsPerS + 999999999, kRtpTs);
  EXPECT_EQ(Be32(rtcp + 8), 0x23456789u);
  EXPECT_EQ(Be32(rtcp + 12), 999999999u);
}

TEST_F(IpmxSrTest, SrGoesRightBeforePacket0ToMediaPortPlusOne) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ASSERT_EQ(ut_sr_wire_count(ctx_), 2);
  EXPECT_EQ(ut_sr_wire_at(ctx_, 0).kind, UT_SR_WIRE_SR);
  EXPECT_EQ(ut_sr_wire_at(ctx_, 1).kind, UT_SR_WIRE_PKT);

  uint8_t pkt[UT_SR_PKT_MAX];
  uint16_t len = ut_sr_last_sr(ctx_, pkt);
  const uint8_t* ip = pkt + 14;
  const uint8_t* udp = pkt + 34;
  ASSERT_EQ(len, kSrPktLen);
  EXPECT_EQ(Be16(pkt + 12), 0x0800);
  EXPECT_EQ(ip[1], 36 << 2); /* DSCP AF42 of the template */
  EXPECT_EQ(Be16(ip + 2), len - 14);
  EXPECT_EQ(ip[9], 17);
  EXPECT_EQ(0, memcmp(ip + 12, kSip, 4));
  const uint8_t dip[4] = {239, 0, 0, 1};
  EXPECT_EQ(0, memcmp(ip + 16, dip, 4));
  EXPECT_EQ(OnesComplementSum(ip, 20), 0xffff);
  EXPECT_EQ(Be16(udp + 2), kMediaPort + 1);
  EXPECT_EQ(Be16(udp + 4), len - 34);
}

TEST_F(IpmxSrTest, NtpIsTheInstantOfPacket0RtpTimestamp) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  constexpr uint64_t kTai = 1700000000ull * kNsPerS; /* a 90 kHz tick */
  const uint32_t rtp_ts = st10_tai_to_media_clk(kTai, k90k);
  ut_sr_frame(ctx_, kPortP, kTai + 1000000, rtp_ts, false); /* packet 0 1 ms later */
  std::vector<uint8_t> rtcp = LastRtcp();
  ASSERT_GE(rtcp.size(), 28u);
  EXPECT_EQ(Be32(&rtcp[4]), kSsrc);
  EXPECT_EQ(Be32(&rtcp[8]), 1700000000u);
  EXPECT_EQ(Be32(&rtcp[12]), 0u);
  EXPECT_EQ(Be32(&rtcp[16]), rtp_ts);
}

/* each 90 kHz tick sits at one of 9 phases of the ns grid, 1e9 / 90000 = 100000 / 9 */
TEST_F(IpmxSrTest, NtpFloorsAndRoundsBackToRtpTimestampAtEveryPhase) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (uint32_t phase = 0; phase < 9; phase++) {
    const uint32_t rtp_ts = kRtpTs + phase;
    ut_sr_frame(ctx_, kPortP, kTaiNs, rtp_ts, false);
    std::vector<uint8_t> rtcp = LastRtcp();
    ASSERT_GE(rtcp.size(), 28u);
    uint64_t tai = (uint64_t)Be32(&rtcp[8]) * kNsPerS + Be32(&rtcp[12]);
    uint32_t floor_ts = (uint32_t)((unsigned __int128)tai * k90k / kNsPerS);
    EXPECT_EQ(floor_ts, rtp_ts) << "phase " << phase;
    EXPECT_EQ(st10_tai_to_media_clk(tai, k90k), rtp_ts) << "phase " << phase;
    EXPECT_EQ(Be32(&rtcp[16]), rtp_ts);
  }
}

TEST_F(IpmxSrTest, CountsReportFramesBeforeThisOne) {
  ut_sr_set_pkt_info(ctx_, kPktTypeNormal, kSt20HdrLen + 1200, 10);
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (uint32_t i = 0; i < 3; i++) ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + i, false);
  std::vector<uint8_t> rtcp = LastRtcp();
  ASSERT_GE(rtcp.size(), 28u);
  EXPECT_EQ(Be32(&rtcp[20]), 2u * 10);
  EXPECT_EQ(Be32(&rtcp[24]), 2u * 10 * (kSt20HdrLen + 1200 - kRtpPayloadOverhead));
}

TEST_F(IpmxSrTest, CountsWrapAt32Bits) {
  ut_sr_set_pkt_info(ctx_, kPktTypeNormal, kRtpPayloadOverhead + 1, 0x80000000u);
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (uint32_t i = 0; i < 3; i++) ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + i, false);
  std::vector<uint8_t> rtcp = LastRtcp();
  ASSERT_GE(rtcp.size(), 28u);
  EXPECT_EQ(Be32(&rtcp[20]), 0u);
  EXPECT_EQ(Be32(&rtcp[24]), 0u);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + 3, false);
  rtcp = LastRtcp();
  EXPECT_EQ(Be32(&rtcp[20]), 0x80000000u);
  EXPECT_EQ(Be32(&rtcp[24]), 0x80000000u);
}

TEST_F(IpmxSrTest, OneSrPerFrameEvenWithARepeatedRtpTimestamp) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false); /* a user timestamp repeated */
  EXPECT_EQ(SrsOnWire(), 2);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + 1501, false);
  EXPECT_EQ(SrsOnWire(), 3);
  EXPECT_EQ(Counts().sent, 3u);
}

TEST_F(IpmxSrTest, EmptyBurstSendsNoSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_burst_empty(ctx_, kTaiNs, kRtpTs), 0);
  EXPECT_EQ(SrsOnWire(), 0);
}

TEST_F(IpmxSrTest, FirstFrameAtRtpTimestampZeroSends) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, 0, false);
  EXPECT_EQ(SrsOnWire(), 1);
}

TEST_F(IpmxSrTest, LatePacket0StillSendsAndCountsLate) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, true);
  EXPECT_EQ(SrsOnWire(), 1);
  struct ut_sr_counts c = Counts();
  EXPECT_EQ(c.sent, 1u);
  EXPECT_EQ(c.late, 1u);
}

std::vector<int> Kinds(ut_sr_ctx* ctx) {
  std::vector<int> kinds;
  for (int i = 0; i < ut_sr_wire_count(ctx); i++)
    kinds.push_back(ut_sr_wire_at(ctx, i).kind);
  return kinds;
}

TEST_F(IpmxSrTest, RefusedSrHoldsPacket0AndRetrySendsSrThenPacket0) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_refuse(ctx_, UT_SR_WIRE_SR, true);
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs), 0); /* packet 0 not sent */
  EXPECT_TRUE(Kinds(ctx_).empty());
  ut_sr_set_refuse(ctx_, UT_SR_WIRE_SR, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs), 1); /* the inflight retry */
  EXPECT_EQ(Kinds(ctx_), (std::vector<int>{UT_SR_WIRE_SR, UT_SR_WIRE_PKT}));
  struct ut_sr_counts c = Counts();
  EXPECT_EQ(c.sent, 1u);
  EXPECT_EQ(c.burst_fail, 1u);
}

TEST_F(IpmxSrTest, RefusedPacket0RetryDoesNotRepeatSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_refuse(ctx_, UT_SR_WIRE_PKT, true);
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs), 0);
  ut_sr_set_refuse(ctx_, UT_SR_WIRE_PKT, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs), 1);
  EXPECT_EQ(Kinds(ctx_), (std::vector<int>{UT_SR_WIRE_SR, UT_SR_WIRE_PKT}));
  EXPECT_EQ(Counts().sent, 1u);
}

TEST_F(IpmxSrTest, RedundantPortSendsNone) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);
  EXPECT_EQ(SrsOnWire(), 0);
}

TEST_F(IpmxSrTest, RedundantPacket0DoesNotTakePrimarySr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_burst_pkt0(ctx_, kPortR, kTaiNs, kRtpTs);
  ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs);
  ASSERT_EQ(ut_sr_wire_count(ctx_), 3);
  EXPECT_EQ(ut_sr_wire_at(ctx_, 1).kind, UT_SR_WIRE_SR);
  EXPECT_EQ(ut_sr_wire_at(ctx_, 1).port, kPortP);
}

TEST_F(IpmxSrTest, StaleSrOfUnsentFrameIsNotSentBeforeNextFrame) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs + 1501); /* another frame's packet 0 */
  EXPECT_EQ(SrsOnWire(), 0);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + 3003, false);
  EXPECT_EQ(SrsOnWire(), 1);
  EXPECT_EQ(Be32(&LastRtcp()[16]), kRtpTs + 3003);
  EXPECT_EQ(ut_sr_pool_in_use(ctx_), 0u);
  EXPECT_EQ(Counts().dropped, 1u);
}

/* requested before the mempool rounds it up to 2^q - 1 */
TEST_F(IpmxSrTest, PoolAsksAnSrForEveryFrameTheTxRingFitsAndOnePending) {
  for (uint32_t frame_pkts : {1u, 3u, 10u, 64u, 4320u}) {
    ut_sr_set_frame_pkts(ctx_, frame_pkts);
    EXPECT_GE(ut_sr_pool_n(ctx_), 64 / frame_pkts + 1) << frame_pkts << " pkts a frame";
  }
}

TEST_F(IpmxSrTest, PoolHoldsAnSrForEveryFrameTheTxRingFitsAndOnePending) {
  constexpr uint32_t kFrames = 64; /* 64 descriptors, one packet a frame */
  ut_sr_set_frame_pkts(ctx_, 1);
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_hold(ctx_, true);
  for (uint32_t i = 0; i < kFrames; i++)
    ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + i, false);
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs + kFrames, false);
  EXPECT_EQ(Counts().alloc_fail, 0u);
  EXPECT_EQ(SrsOnWire(), (int)kFrames);
  EXPECT_EQ(ut_sr_pool_in_use(ctx_), kFrames + 1);
}

TEST_F(IpmxSrTest, QueuedSrReturnedByFlushThenDetachFreesPool) {
  uint64_t before = ut_rte_alloc_count();
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_hold(ctx_, true);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ASSERT_EQ(ut_sr_pool_in_use(ctx_), 1u);
  std::string name = ut_sr_pool_name(ctx_);
  ut_sr_flush(ctx_);
  ut_sr_detach(ctx_);
  EXPECT_EQ(rte_mempool_lookup(name.c_str()), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

TEST_F(IpmxSrTest, DetachLeavesPoolWhileQueueStillHoldsSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_hold(ctx_, true);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  std::string name = ut_sr_pool_name(ctx_);
  ut_sr_detach(ctx_);
  EXPECT_FALSE(ut_sr_attached(ctx_));
  EXPECT_NE(rte_mempool_lookup(name.c_str()), nullptr);
  ut_sr_flush(ctx_); /* the held mbuf goes back to a live pool */
}

TEST_F(IpmxSrTest, DetachFreesPendingSr) {
  uint64_t before = ut_rte_alloc_count();
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  std::string name = ut_sr_pool_name(ctx_);
  ut_sr_detach(ctx_);
  EXPECT_EQ(rte_mempool_lookup(name.c_str()), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

}  // namespace
