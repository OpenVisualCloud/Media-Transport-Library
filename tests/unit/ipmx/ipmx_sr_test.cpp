/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * The IPMX video Sender Report (TR-10-1 sections 8.7, 8.8.2): an RTCP SR with the IPMX
 * Info Block followed by an SDES CNAME (TR-10-9 section 8), once per frame, to the media
 * port + 1 on the session's own queue right before packet 0. With ST 2022-7 each leg is
 * its own RTP stream to its own destination, so each leg sends its own SR.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='IpmxSr*'
 */

#include <gtest/gtest.h>
#include <rte_mempool.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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
    ut_sr_counts(ctx_, kPortP, &c);
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

/* st22_tx_create() maps ST22_TX_FLAG_IPMX to ST20_TX_FLAG_IPMX, but the st22 SR needs
 * its own IPMX Info Block (TR-10-11 section 12), so an st22 session sends none yet */
TEST_F(IpmxSrTest, St22SessionCreatesNoSr) {
  ut_sr_set_st22(ctx_);
  EXPECT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_FALSE(ut_sr_attached(ctx_));
}

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

/* a single-port session takes a ts-refclk change at every frame start, also when the
 * application repeats an RTP timestamp */
TEST_F(IpmxSrTest, TsRefclkChangeAppliesOnARepeatedRtpTimestamp) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, "ptp=traceable"), 0);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  std::vector<uint8_t> rtcp = LastRtcp();
  ASSERT_GE(rtcp.size(), 28u + IPMX_INFO_BLOCK_LEN);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(&rtcp[28 + IPMX_TS_REFCLK_OFFSET])),
            "ptp=traceable");
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

TEST_F(IpmxSrTest, StaleSrOfUnsentFrameIsNotSentBeforeNextFrame) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs + 1501); /* another frame's packet 0 */
  EXPECT_EQ(SrsOnWire(), 0);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + 3003, false);
  EXPECT_EQ(SrsOnWire(), 1);
  EXPECT_EQ(Be32(&LastRtcp()[16]), kRtpTs + 3003);
  EXPECT_EQ(ut_sr_pool_in_use(ctx_, kPortP), 0u);
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
  EXPECT_EQ(ut_sr_pool_in_use(ctx_, kPortP), kFrames + 1);
}

TEST_F(IpmxSrTest, QueuedSrReturnedByFlushThenDetachFreesPool) {
  uint64_t before = ut_rte_alloc_count();
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_hold(ctx_, true);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ASSERT_EQ(ut_sr_pool_in_use(ctx_, kPortP), 1u);
  std::string name = ut_sr_pool_name(ctx_, kPortP);
  ut_sr_flush(ctx_);
  ut_sr_detach(ctx_);
  EXPECT_EQ(rte_mempool_lookup(name.c_str()), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

TEST_F(IpmxSrTest, DetachLeavesPoolWhileQueueStillHoldsSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_hold(ctx_, true);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  std::string name = ut_sr_pool_name(ctx_, kPortP);
  ut_sr_detach(ctx_);
  EXPECT_FALSE(ut_sr_attached(ctx_));
  EXPECT_NE(rte_mempool_lookup(name.c_str()), nullptr);
  ut_sr_flush(ctx_); /* the held mbuf goes back to a live pool */
}

TEST_F(IpmxSrTest, DetachFreesPendingSr) {
  uint64_t before = ut_rte_alloc_count();
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  std::string name = ut_sr_pool_name(ctx_, kPortP);
  ut_sr_detach(ctx_);
  EXPECT_EQ(rte_mempool_lookup(name.c_str()), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

/*
 * ST 2022-7: the R leg sends its own SR to its own destination, on its own queue right
 * before its own packet 0, with its own pool, counters and CNAME; the two SRs of a frame
 * carry the same SSRC, timestamps and Info Block (TR-10-1 section 8.7, RFC 3550 6.4.1).
 */
constexpr uint8_t kSipR[4] = {192, 168, 2, 20};
constexpr uint8_t kDipR[4] = {239, 0, 0, 2};
constexpr uint16_t kMediaPortR = 20002;
constexpr uint32_t kSrPktLenR = UT_SR_UDP_HDR_LEN + kSdesOffset + 24; /* "192.168.2.20" */

class IpmxSrRedundantTest : public IpmxSrTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(IpmxSrTest::SetUp());
    ut_sr_set_redundant(ctx_, kSipR, kDipR, kMediaPortR);
  }
  struct ut_sr_counts CountsOf(int port) {
    struct ut_sr_counts c;
    ut_sr_counts(ctx_, port, &c);
    return c;
  }
  std::vector<uint8_t> LastPkt(int port) {
    uint8_t pkt[UT_SR_PKT_MAX];
    uint16_t len = ut_sr_last_sr_port(ctx_, port, pkt);
    return std::vector<uint8_t>(pkt, pkt + len);
  }
  /* the RTCP SR and Info Block, the part both legs share */
  std::vector<uint8_t> SharedPart(int port) {
    std::vector<uint8_t> pkt = LastPkt(port);
    if (pkt.size() < UT_SR_UDP_HDR_LEN + kSdesOffset) return {};
    std::vector<uint8_t> rtcp(pkt.begin() + UT_SR_UDP_HDR_LEN,
                              pkt.begin() + UT_SR_UDP_HDR_LEN + kSdesOffset);
    return rtcp;
  }
  std::string Cname(int port) {
    std::vector<uint8_t> pkt = LastPkt(port);
    const size_t item = UT_SR_UDP_HDR_LEN + kSdesOffset + 8;
    if (pkt.size() < item + 2 || pkt.size() < item + 2 + pkt[item + 1]) return "";
    return std::string(reinterpret_cast<const char*>(&pkt[item + 2]), pkt[item + 1]);
  }
  /* the wire order of SRs and packets 0 as (kind, port) */
  std::vector<std::pair<int, int>> Wire() {
    std::vector<std::pair<int, int>> w;
    for (int i = 0; i < ut_sr_wire_count(ctx_); i++)
      w.emplace_back(ut_sr_wire_at(ctx_, i).kind, ut_sr_wire_at(ctx_, i).port);
    return w;
  }
};

/* TR-10-1 section 8.8.2 per leg: one SR a frame, right before that leg's packet 0 */
TEST_F(IpmxSrRedundantTest, EachLegSendsOneSrRightBeforeItsPacket0) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (uint32_t i = 0; i < 2; i++) {
    ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs + i, false);
    ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs + i, false);
  }
  const std::vector<std::pair<int, int>> one_frame = {{UT_SR_WIRE_SR, kPortP},
                                                      {UT_SR_WIRE_PKT, kPortP},
                                                      {UT_SR_WIRE_SR, kPortR},
                                                      {UT_SR_WIRE_PKT, kPortR}};
  std::vector<std::pair<int, int>> two_frames = one_frame;
  two_frames.insert(two_frames.end(), one_frame.begin(), one_frame.end());
  EXPECT_EQ(Wire(), two_frames);
  EXPECT_EQ(CountsOf(kPortP).sent, 2u);
  EXPECT_EQ(CountsOf(kPortR).sent, 2u);
}

/* with both packets 0 dequeued before either burst, each leg's SR still goes out on its
 * own queue right before that leg's packet 0, R leg first here */
TEST_F(IpmxSrRedundantTest, BothDequeuedThenRBurstFirstKeepsEachSrOnItsLeg) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_dequeue(ctx_, kPortR, kTaiNs, kRtpTs, false);
  ut_sr_burst_pkt0(ctx_, kPortR, kTaiNs, kRtpTs);
  ut_sr_burst_pkt0(ctx_, kPortP, kTaiNs, kRtpTs);
  EXPECT_EQ(Wire(), (std::vector<std::pair<int, int>>{{UT_SR_WIRE_SR, kPortR},
                                                      {UT_SR_WIRE_PKT, kPortR},
                                                      {UT_SR_WIRE_SR, kPortP},
                                                      {UT_SR_WIRE_PKT, kPortP}}));
}

/* RFC 3550 section 6.4.1, TR-10-1 section 8.7: the R leg SR goes to the R destination IP
 * and the R media port + 1, from the R source, with a valid IPv4 header; the P leg SR
 * keeps the P addresses */
TEST_F(IpmxSrRedundantTest, RLegSrGoesToRDestinationPortPlusOne) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);

  std::vector<uint8_t> r = LastPkt(kPortR);
  ASSERT_EQ(r.size(), kSrPktLenR);
  const uint8_t* ip = &r[14];
  EXPECT_EQ(0, memcmp(ip + 12, kSipR, 4));
  EXPECT_EQ(0, memcmp(ip + 16, kDipR, 4));
  EXPECT_EQ(Be16(ip + 2), r.size() - 14);
  EXPECT_EQ(OnesComplementSum(ip, 20), 0xffff);
  EXPECT_EQ(Be16(&r[34 + 2]), kMediaPortR + 1);

  std::vector<uint8_t> p = LastPkt(kPortP);
  ASSERT_EQ(p.size(), kSrPktLen);
  const uint8_t dip_p[4] = {239, 0, 0, 1};
  EXPECT_EQ(0, memcmp(&p[14 + 12], kSip, 4));
  EXPECT_EQ(0, memcmp(&p[14 + 16], dip_p, 4));
  EXPECT_EQ(Be16(&p[34 + 2]), kMediaPort + 1);
}

/* the R leg SR takes the launch time of the R packet 0 from the R interface's flag and
 * dynfield; the P interface here has no launch-time flag at all */
TEST_F(IpmxSrRedundantTest, RLegSrTakesRPacket0LaunchTime) {
  ASSERT_EQ(ut_sr_set_launch_time(ctx_, kPortR), 0);
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);
  ASSERT_EQ(Wire(), (std::vector<std::pair<int, int>>{{UT_SR_WIRE_SR, kPortR},
                                                      {UT_SR_WIRE_PKT, kPortR}}));
  struct ut_sr_wire sr = ut_sr_wire_at(ctx_, 0);
  struct ut_sr_wire pkt0 = ut_sr_wire_at(ctx_, 1);
  EXPECT_TRUE(pkt0.launch);
  EXPECT_TRUE(sr.launch);
  EXPECT_EQ(sr.launch_time, pkt0.launch_time);
  EXPECT_EQ(sr.launch_time, kTaiNs);
}

/* RFC 3550 section 6.5.1 numeric host form: the CNAME of each leg is its source IPv4 */
TEST_F(IpmxSrRedundantTest, CnameOfEachLegIsItsSourceIp) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);
  EXPECT_EQ(Cname(kPortP), "192.168.1.10");
  EXPECT_EQ(Cname(kPortR), "192.168.2.20");
}

/* one stream on two legs: SSRC, NTP and RTP timestamps, packet and octet counts and the
 * whole Info Block are the same on both legs, frame after frame */
TEST_F(IpmxSrRedundantTest, BothLegsShareSsrcTimestampsCountsAndInfoBlock) {
  ut_sr_set_pkt_info(ctx_, kPktTypeNormal, kSt20HdrLen + 1200, 10);
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (uint32_t i = 0; i < 3; i++) {
    ut_sr_frame(ctx_, kPortP, kTaiNs + i * 16683333ull, kRtpTs + i * 1501, false);
    ut_sr_frame(ctx_, kPortR, kTaiNs + i * 16683333ull + 1000, kRtpTs + i * 1501, false);
    std::vector<uint8_t> p = SharedPart(kPortP);
    ASSERT_EQ(p.size(), kSdesOffset);
    EXPECT_EQ(p, SharedPart(kPortR)) << "frame " << i;
    EXPECT_EQ(Be32(&p[4]), kSsrc);
    EXPECT_EQ(Be32(&p[20]), i * 10);
  }
}

/*
 * A ts-refclk or block version change lands on both legs in the same frame, whichever
 * leg dequeues packet 0 first, so a receiver on either leg sees the same Info Block for
 * every frame (TR-10-1 section 8.7, one stream).
 */
class IpmxSrRedundantChangeTest : public IpmxSrRedundantTest,
                                  public ::testing::WithParamInterface<bool> {
 protected:
  int First() const {
    return GetParam() ? kPortR : kPortP;
  }
  int Second() const {
    return GetParam() ? kPortP : kPortR;
  }
  void Frame(int port, uint32_t i) {
    ut_sr_frame(ctx_, port, kTaiNs + i * 16683333ull, kRtpTs + i * 1501, false);
  }
  std::vector<uint8_t> InfoBlock(int port) {
    std::vector<uint8_t> shared = SharedPart(port);
    if (shared.size() < kSdesOffset) return {};
    return std::vector<uint8_t>(shared.begin() + 28, shared.end());
  }
  std::string TsRefclk(int port) {
    std::vector<uint8_t> block = InfoBlock(port);
    if (block.size() < IPMX_INFO_BLOCK_LEN) return "";
    return std::string(reinterpret_cast<const char*>(&block[IPMX_TS_REFCLK_OFFSET]));
  }
  uint8_t Version(int port) {
    std::vector<uint8_t> block = InfoBlock(port);
    return block.size() < IPMX_INFO_BLOCK_LEN ? 0xff
                                              : block[IPMX_INFO_BLOCK_VERSION_OFFSET];
  }
};

/* the ts-refclk set after the first leg built its SR waits for the next frame on both */
TEST_P(IpmxSrRedundantChangeTest, TsRefclkSetBetweenLegsAppliesToBothNextFrame) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Frame(First(), 0);
  Frame(Second(), 0);
  const std::string before = TsRefclk(First());
  Frame(First(), 1);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, "ptp=traceable"), 0);
  Frame(Second(), 1);
  EXPECT_EQ(TsRefclk(First()), before);
  EXPECT_EQ(TsRefclk(Second()), before);
  EXPECT_EQ(Version(First()), Version(Second()));
  Frame(First(), 2);
  Frame(Second(), 2);
  EXPECT_EQ(TsRefclk(First()), "ptp=traceable");
  EXPECT_EQ(TsRefclk(Second()), "ptp=traceable");
  EXPECT_EQ(Version(First()), 1);
  EXPECT_EQ(Version(Second()), 1);
}

/* the block version set after the first leg built its SR waits for the next frame */
TEST_P(IpmxSrRedundantChangeTest, BlockVersionSetBetweenLegsAppliesToBothNextFrame) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Frame(First(), 0);
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 7), 0);
  Frame(Second(), 0);
  EXPECT_EQ(Version(First()), 0);
  EXPECT_EQ(Version(Second()), 0);
  Frame(First(), 1);
  Frame(Second(), 1);
  EXPECT_EQ(Version(First()), 7);
  EXPECT_EQ(Version(Second()), 7);
}

/* a queue recovery can flush one leg's packet 0 after the other leg already dequeued
 * it, so per-leg frame counts are not a frame identity: a ts-refclk change still lands
 * on both legs in the same frame after such a flush */
TEST_P(IpmxSrRedundantChangeTest, TsRefclkLandsOnBothLegsAfterALegDroppedAFrameStart) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Frame(First(), 0);
  Frame(Second(), 0);
  ut_sr_dequeue(ctx_, Second(), kTaiNs + 16683333ull, kRtpTs + 1501, false);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, "ptp=traceable"), 0);
  Frame(First(), 2);
  Frame(Second(), 2);
  EXPECT_EQ(TsRefclk(First()), "ptp=traceable");
  EXPECT_EQ(TsRefclk(Second()), "ptp=traceable");
  EXPECT_EQ(Version(First()), Version(Second()));
}

/* with a repeated user RTP timestamp the leg that went second can lead the next frame;
 * a ts-refclk change in between still lands on both legs of that frame */
TEST_P(IpmxSrRedundantChangeTest, TsRefclkLandsOnBothLegsWhenLeadSwapsOnRepeatedTs) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, First(), kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, Second(), kTaiNs, kRtpTs, false);
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, "ptp=traceable"), 0);
  ut_sr_frame(ctx_, Second(), kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, First(), kTaiNs, kRtpTs, false);
  EXPECT_EQ(TsRefclk(First()), "ptp=traceable");
  EXPECT_EQ(TsRefclk(Second()), "ptp=traceable");
  EXPECT_EQ(Version(First()), Version(Second()));
}

INSTANTIATE_TEST_SUITE_P(PFirstOrRFirst, IpmxSrRedundantChangeTest,
                         ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "RFirst" : "PFirst";
                         });

/* a destination update of the R leg moves its SR and leaves P's */
TEST_F(IpmxSrRedundantTest, RLegSrFollowsItsDestinationUpdate) {
  const uint8_t dip[4] = {239, 0, 0, 9};
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_dest(ctx_, kPortR, dip, 30000);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);
  std::vector<uint8_t> r = LastPkt(kPortR);
  ASSERT_EQ(r.size(), kSrPktLenR);
  EXPECT_EQ(0, memcmp(&r[14 + 16], dip, 4));
  EXPECT_EQ(Be16(&r[34 + 2]), 30001);
  EXPECT_EQ(Be16(&LastPkt(kPortP)[34 + 2]), kMediaPort + 1);
}

/* a queue refusing the R leg SR holds R's packet 0 only; P is untouched and R's retry
 * sends its SR then packet 0 */
TEST_F(IpmxSrRedundantTest, RefusedSrOnRLegDoesNotAffectPLeg) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_set_refuse_port(ctx_, kPortR, UT_SR_WIRE_SR, true);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_dequeue(ctx_, kPortR, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortR, kTaiNs, kRtpTs), 0);
  ut_sr_set_refuse_port(ctx_, kPortR, UT_SR_WIRE_SR, false);
  EXPECT_EQ(ut_sr_burst_pkt0(ctx_, kPortR, kTaiNs, kRtpTs), 1);
  EXPECT_EQ(Wire(), (std::vector<std::pair<int, int>>{{UT_SR_WIRE_SR, kPortP},
                                                      {UT_SR_WIRE_PKT, kPortP},
                                                      {UT_SR_WIRE_SR, kPortR},
                                                      {UT_SR_WIRE_PKT, kPortR}}));
  EXPECT_EQ(CountsOf(kPortP).burst_fail, 0u);
  EXPECT_EQ(CountsOf(kPortP).sent, 1u);
  EXPECT_EQ(CountsOf(kPortR).burst_fail, 1u);
  EXPECT_EQ(CountsOf(kPortR).sent, 1u);
}

/* an empty R leg pool skips R's SR only; P still sends its own */
TEST_F(IpmxSrRedundantTest, AllocFailOnRLegDoesNotAffectPLeg) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_drain_pool(ctx_, kPortR);
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, false);
  EXPECT_EQ(Wire(), (std::vector<std::pair<int, int>>{{UT_SR_WIRE_SR, kPortP},
                                                      {UT_SR_WIRE_PKT, kPortP},
                                                      {UT_SR_WIRE_PKT, kPortR}}));
  EXPECT_EQ(CountsOf(kPortP).alloc_fail, 0u);
  EXPECT_EQ(CountsOf(kPortP).sent, 1u);
  EXPECT_EQ(CountsOf(kPortR).alloc_fail, 1u);
  ut_sr_flush(ctx_);
}

/* an R leg late packet 0 counts late on R only */
TEST_F(IpmxSrRedundantTest, LateCountsPerLeg) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_frame(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_frame(ctx_, kPortR, kTaiNs, kRtpTs, true);
  EXPECT_EQ(CountsOf(kPortP).late, 0u);
  EXPECT_EQ(CountsOf(kPortR).late, 1u);
}

/* the RL lead is the shaper time of that leg's own pending SR */
TEST_F(IpmxSrRedundantTest, RlLeadIsPerLeg) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_rl_lead_ns(ctx_, kPortP, 1000, 1000), kSrPktLen);
  EXPECT_EQ(ut_sr_rl_lead_ns(ctx_, kPortR, 1000, 1000), 0u);
  ut_sr_dequeue(ctx_, kPortR, kTaiNs, kRtpTs, false);
  EXPECT_EQ(ut_sr_rl_lead_ns(ctx_, kPortR, 1000, 1000), kSrPktLenR);
}

/* each leg has its own pool, named with its port and the recovery_idx */
TEST_F(IpmxSrRedundantTest, EachLegHasItsOwnPoolNamedWithPortAndRecoveryIdx) {
  ut_sr_set_recovery_idx(ctx_, 3);
  ASSERT_NO_FATAL_FAILURE(Attach());
  const std::string p = ut_sr_pool_name(ctx_, kPortP);
  const std::string r = ut_sr_pool_name(ctx_, kPortR);
  EXPECT_NE(p, r);
  EXPECT_NE(p.find("_SR0_3_"), std::string::npos) << p;
  EXPECT_NE(r.find("_SR1_3_"), std::string::npos) << r;
}

/* an R leg pool that cannot be created fails the attach and frees the P leg, no leak */
TEST_F(IpmxSrRedundantTest, RLegPoolFailureFailsAttachWithoutLeak) {
  ASSERT_EQ(ut_sr_block_pool(ctx_, kPortR), 0);
  uint64_t before = ut_rte_alloc_count();
  EXPECT_LT(ut_sr_attach(ctx_), 0);
  EXPECT_FALSE(ut_sr_attached(ctx_));
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

/* detach frees both pools and both pending SRs, no leak */
TEST_F(IpmxSrRedundantTest, DetachFreesBothLegPoolsAndPendingSrs) {
  uint64_t before = ut_rte_alloc_count();
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_dequeue(ctx_, kPortP, kTaiNs, kRtpTs, false);
  ut_sr_dequeue(ctx_, kPortR, kTaiNs, kRtpTs, false);
  const std::string p = ut_sr_pool_name(ctx_, kPortP);
  const std::string r = ut_sr_pool_name(ctx_, kPortR);
  ut_sr_detach(ctx_);
  EXPECT_EQ(rte_mempool_lookup(p.c_str()), nullptr);
  EXPECT_EQ(rte_mempool_lookup(r.c_str()), nullptr);
  EXPECT_EQ(ut_rte_alloc_count(), before);
}

}  // namespace
