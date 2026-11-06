/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * The IPMX Sender Report of an st22 (JPEG XS) TX session. It is the st20 one of
 * ipmx_sr_test.cpp (TR-10-1 sections 8.7, 8.8.2), but its IPMX Info Block carries the
 * constant bit-rate compressed video Media Info Block 0x0003 (TR-10-11 section 12, the
 * TR-10-2 section 10 layout) and right after it the JPEG XS Media Info Block 0x0008
 * (TR-10-15 Part 1 section 9: T, P = K, Ppih, Plev). The SR packet and octet counts
 * (RFC 3550 section 6.4.1) follow each frame's own packet count, which depends on its
 * codestream size, so the st22 builder stamps packet 0 with the frame's box + codestream
 * bytes and the transmitter adds that frame from the stamp.
 *
 * The SR cases drive lib/src/ipmx/ipmx_sr.c and the transmitter through
 * ipmx_sr_harness.c, the builder cases tv_tasklet_st22() through st20_tx_harness.c.
 * Golden bytes come from the VSF ipmx_sender_report.py builder (nmos-testing-ipmx).
 *
 * Groups:
 *   IpmxSt22SrTest         create, the 0x0003 + 0x0008 bytes, Plev, T/K, the 0x0003 info
 *   IpmxSt22SrPlevTest     SR Plev = jxpl box Plev, 1080p/2160p, 3/4 bpp
 *   IpmxSt22SrCountTest    counts per frame size, both ST 2022-7 legs, the SR pool
 *   IpmxSt22SrPacingTest   the SR before packet 0, pads neither sent nor counted, in the
 *                          TSC, PTP and TSN paths st22 uses
 *   IpmxSt22SrApiTest      the six public ipmx_st22[p]_tx_*() wrappers
 *   IpmxSt22SrBuilderTest  tv_tasklet_st22() stamps packet 0 on each leg
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxSt22Sr*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr_harness.h"
#include "session/st20_tx_harness.h"

namespace {

constexpr uint32_t kSsrc = 0x12345678;
constexpr uint16_t kMediaPort = 20000;
constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint8_t kSipR[4] = {192, 168, 2, 10};
constexpr uint8_t kDipR[4] = {239, 0, 1, 1};
constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull + 123456789;
constexpr uint32_t kRtpTs = 0xa1b2c3d4;
constexpr int kPortP = 0;
constexpr int kPortR = 1;
/* pkt_len of a 1460 octet UDP payload, as tv_init_pkt() rounds it */
constexpr uint32_t kPktLen = 1408;
constexpr uint32_t kBoxes = 60;
/* RFC 9134 payload header, part of the RTP payload octets */
constexpr uint32_t kPayloadHdr = 4;
constexpr uint32_t kInfoOffset = 28;
constexpr uint32_t kJxsMibOffset = kInfoOffset + 176;
constexpr uint32_t kSdesOffset = kInfoOffset + 188;

constexpr size_t Bytes(uint32_t w, uint32_t h, uint32_t bpp) {
  return static_cast<size_t>(w) * h * bpp / 8;
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint16_t Be16(const uint8_t* p) {
  return (uint16_t)(p[0] << 8 | p[1]);
}

/* VSF ipmx_sender_report.py IPMXInfoBlock(version 0, "localmac=02-00-00-00-00-01",
 * "direct=0", [VideoMediaInfoBlock type 0x0003 RGB 8 1920x1080 60000/1001 pixclk
 * 124291708 1920x1080, JXSVMediaInfoBlock T 1 P 0 Ppih 0x4a40 Plev 0x1004]) */
const uint8_t kInfoBlockGolden[188] = {
    0x58, 0x31, 0x00, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x6d,
    0x61, 0x63, 0x3d, 0x30, 0x32, 0x2d, 0x30, 0x30, 0x2d, 0x30, 0x30, 0x2d, 0x30, 0x30,
    0x2d, 0x30, 0x30, 0x2d, 0x30, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x3d, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x16, 0x52, 0x47, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x01, 0x4e, 0x41, 0x52, 0x52,
    0x4f, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x54, 0x37, 0x30, 0x39, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x44, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0x80, 0x04, 0x38, 0x03, 0xa9, 0x83, 0xe9, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x68, 0x8a, 0x7c, 0x07, 0x80, 0x04, 0x38, 0x00, 0x08, 0x00, 0x02, 0x80, 0x00,
    0x4a, 0x40, 0x10, 0x04, 0x00, 0x00,
};

/* an st22 1080p59.94 session of up to codestream_max bytes, not attached */
ut_sr_ctx* CreateSt22(size_t codestream_max, uint32_t width = 1920,
                      uint32_t height = 1080) {
  ut_sr_ctx* ctx = ut_sr_create(kSip, kSsrc, kMediaPort);
  if (!ctx) return nullptr;
  /* st22_tx_create() leaves its placeholder st20 fmt, YUV 4:2:2 10-bit */
  if (ut_sr_set_video(ctx, ST20_FMT_YUV_422_10BIT, width, height, ST20_PACKING_BPM,
                      ST_FPS_P59_94) < 0 ||
      ut_sr_set_st22(ctx, codestream_max, kPktLen) < 0) {
    ut_sr_destroy(ctx);
    return nullptr;
  }
  return ctx;
}

class IpmxSt22SrBase : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = CreateSt22(Bytes(1920, 1080, 3));
    ASSERT_NE(ctx_, nullptr);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  void Attach() {
    ASSERT_EQ(ut_sr_attach(ctx_), 0);
    ASSERT_TRUE(ut_sr_attached(ctx_));
  }
  /* the RTCP part of the last SR sent on s_port */
  std::vector<uint8_t> LastRtcp(int s_port = kPortP) {
    uint8_t pkt[UT_SR_PKT_MAX];
    uint16_t len = ut_sr_last_sr_port(ctx_, s_port, pkt);
    if (len <= UT_SR_UDP_HDR_LEN) return {};
    return std::vector<uint8_t>(pkt + UT_SR_UDP_HDR_LEN, pkt + len);
  }
  /* one frame on s_port whose packet 0 carries frame_size box + codestream bytes */
  std::vector<uint8_t> Frame(int s_port, uint32_t frame_size) {
    ut_sr_set_frame_size(ctx_, s_port, frame_size);
    ut_sr_frame(ctx_, s_port, kTaiNs + frames_[s_port] * 16683333ull,
                kRtpTs + frames_[s_port] * 1501, false);
    frames_[s_port]++;
    return LastRtcp(s_port);
  }
  ut_sr_ctx* ctx_ = nullptr;
  uint32_t frames_[2] = {0, 0};
};

/* ---- the SR and its two Media Info Blocks ---- */

class IpmxSt22SrTest : public IpmxSt22SrBase {};

/* st22_tx_create() maps ST22_TX_FLAG_IPMX to ST20_TX_FLAG_IPMX; the session now gets an
 * SR state like an st20 one, TP-1 section 13.3 asks for an SR per frame */
TEST_F(IpmxSt22SrTest, AttachesAnSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
}

/* the Info Block, byte for byte the one the VSF builder makes for the same stream:
 * 0x0003 as 0x0001 with RGB 8-bit, then 0x0008, one length over both (188 octets) */
TEST_F(IpmxSt22SrTest, InfoBlockMatchesVsfGolden) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  uint8_t rtcp[UT_SR_PKT_MAX];
  ASSERT_GE(ut_sr_build(ctx_, rtcp, kTaiNs, kRtpTs), kSdesOffset);
  EXPECT_EQ(0, memcmp(rtcp + kInfoOffset, kInfoBlockGolden, sizeof(kInfoBlockGolden)));
}

/* RFC 3550 lengths in words - 1: SR 216 octets, Info Block 188, 0x0003 92, 0x0008 12
 * (TR-10-15 Part 1 section 9 says 2); the SDES CNAME follows the Info Block */
TEST_F(IpmxSt22SrTest, LengthsCoverBothMediaInfoBlocks) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  uint8_t rtcp[UT_SR_PKT_MAX];
  uint16_t len = ut_sr_build(ctx_, rtcp, kTaiNs, kRtpTs);
  EXPECT_EQ(len, kSdesOffset + 24); /* "192.168.1.10" */
  EXPECT_EQ(Be16(rtcp + 2), kSdesOffset / 4 - 1);
  EXPECT_EQ(Be16(rtcp + kInfoOffset + 2), 188 / 4 - 1);
  EXPECT_EQ(Be16(rtcp + kInfoOffset + 84), 0x0003);
  EXPECT_EQ(Be16(rtcp + kInfoOffset + 86), 22);
  EXPECT_EQ(Be16(rtcp + kJxsMibOffset), 0x0008);
  EXPECT_EQ(Be16(rtcp + kJxsMibOffset + 2), 2);
  EXPECT_EQ(rtcp[kSdesOffset + 1], 202); /* SDES */
}

/* TR-10-15 Part 1 section 9.1: High444.12, 2k-1, Sublev3bpp, sequential (T = 1) is
 * 0x80004a40 0x10040000 */
TEST_F(IpmxSt22SrTest, JxsMibIsTheSection91ExampleWord) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  uint8_t rtcp[UT_SR_PKT_MAX];
  ut_sr_build(ctx_, rtcp, kTaiNs, kRtpTs);
  EXPECT_EQ(Be32(rtcp + kJxsMibOffset + 4), 0x80004a40u);
  EXPECT_EQ(Be32(rtcp + kJxsMibOffset + 8), 0x10040000u);
}

/* T and P come from the RTP payload header template the session sends (RFC 9134 T and
 * K bits), not a second literal: flipping them there flips them in the SR */
TEST_F(IpmxSt22SrTest, TAndPFollowTheRtpHeader) {
  ut_sr_set_st22_tk(ctx_, false, true);
  ASSERT_NO_FATAL_FAILURE(Attach());
  uint8_t rtcp[UT_SR_PKT_MAX];
  ut_sr_build(ctx_, rtcp, kTaiNs, kRtpTs);
  EXPECT_EQ(Be32(rtcp + kJxsMibOffset + 4), 0x40004a40u);
}

/* the 0x0003 values (TR-10-11 section 12), also what ipmx_st22_tx_get_info() returns:
 * RGB 4:4:4 8-bit (D11, TR-10-15 Part 1 section 8, TR-08 section 8.1.1), not the st20
 * YUV 4:2:2 10-bit placeholder fmt st22_tx_create() sets;
 * M 0, which TR-10-2 section 10 defines only for ST 2110-20 */
TEST_F(IpmxSt22SrTest, InfoIsRgb8NotTheSt20Placeholder) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  EXPECT_STREQ(info.sampling, "RGB");
  EXPECT_EQ(info.depth, 8);
  EXPECT_FALSE(info.floating);
  EXPECT_FALSE(info.gpm);
  EXPECT_FALSE(info.interlaced);
  EXPECT_FALSE(info.segmented);
  EXPECT_EQ(info.par_w, 1);
  EXPECT_EQ(info.par_h, 1);
  EXPECT_STREQ(info.range, "NARROW");
  EXPECT_STREQ(info.colorimetry, "BT709");
  EXPECT_STREQ(info.tcs, "SDR");
  EXPECT_EQ(info.width, 1920);
  EXPECT_EQ(info.height, 1080);
  EXPECT_EQ(info.rate_num, 60000u);
  EXPECT_EQ(info.rate_den, 1001u);
  EXPECT_EQ(info.pixclk, 124291708u);
  EXPECT_EQ(info.htotal, 1920);
  EXPECT_EQ(info.vtotal, 1080);
}

struct PlevCase {
  const char* name;
  uint32_t width;
  uint32_t height;
  uint32_t bpp;
  uint16_t plev;
};

class IpmxSt22SrPlevTest : public ::testing::TestWithParam<PlevCase> {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
  }
};

/* the SR Plev is the one the jxpl box carries (task 16 picks it at create), Ppih is
 * High444.12, reserved bits are 0 and fbblevel, the VSF (plev >> 8 & 3) and
 * (plev >> 5 & 3) bits, is Unrestricted (TR-10-15 Part 1 section 9, no TDC profile) */
TEST_P(IpmxSt22SrPlevTest, PlevIsTheJxplBoxOne) {
  const PlevCase& c = GetParam();
  ut_sr_ctx* ctx = CreateSt22(Bytes(c.width, c.height, c.bpp), c.width, c.height);
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(ut_sr_attach(ctx), 0);
  uint8_t rtcp[UT_SR_PKT_MAX];
  ut_sr_build(ctx, rtcp, kTaiNs, kRtpTs);
  uint16_t box_plev = ut_sr_st22_box_plev(ctx);
  ut_sr_destroy(ctx);

  const uint8_t* mib = rtcp + kJxsMibOffset;
  EXPECT_EQ(box_plev, c.plev);
  EXPECT_EQ(Be16(mib + 8), c.plev);
  EXPECT_EQ(Be16(mib + 6), 0x4a40);
  EXPECT_EQ(Be16(mib + 4) & 0x3fff, 0);
  EXPECT_EQ(Be16(mib + 10), 0);
  EXPECT_EQ((c.plev >> 8) & 3, 0);
  EXPECT_EQ((c.plev >> 5) & 3, 0);
}

const PlevCase kPlevCases[] = {
    {"P1080_3bpp", 1920, 1080, 3, 0x1004},
    {"P1080_4bpp", 1920, 1080, 4, 0x1006},
    {"P2160_3bpp", 3840, 2160, 3, 0x2404},
    {"P2160_4bpp", 3840, 2160, 4, 0x2406},
};

INSTANTIATE_TEST_SUITE_P(Sizes, IpmxSt22SrPlevTest, ::testing::ValuesIn(kPlevCases),
                         [](const ::testing::TestParamInfo<PlevCase>& info) {
                           return std::string(info.param.name);
                         });

/* ---- packet and octet counts ---- */

class IpmxSt22SrCountTest : public IpmxSt22SrBase {};

/* each SR counts the frames before its own, each frame by its stamped size: packets
 * ceil(size / pkt_len), the last one short, octets the size plus a 4 octet RFC 9134
 * payload header a packet (RFC 3550 section 6.4.1, the same rule as st20) */
TEST_F(IpmxSt22SrCountTest, CountsFollowEachFrameSize) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  const uint32_t sizes[] = {3000, kPktLen, kPktLen + 1, 2 * kPktLen};
  const uint32_t pkts[] = {3, 1, 2, 2};
  uint32_t want_pkts = 0, want_octets = 0;
  for (int i = 0; i < 4; i++) {
    std::vector<uint8_t> rtcp = Frame(kPortP, sizes[i]);
    ASSERT_GE(rtcp.size(), 28u);
    EXPECT_EQ(Be32(&rtcp[20]), want_pkts) << "frame " << i;
    EXPECT_EQ(Be32(&rtcp[24]), want_octets) << "frame " << i;
    want_pkts += pkts[i];
    want_octets += sizes[i] + pkts[i] * kPayloadHdr;
  }
}

struct PacingCase {
  const char* name;
  int (*call)(ut_sr_ctx* ctx);
  bool holds; /* packet 0 waits in software for its target, TSN leaves it to the NIC */
};

class IpmxSt22SrPacingTest : public IpmxSt22SrBase,
                             public ::testing::WithParamInterface<PacingCase> {};

/* st22 uses TSC, never RL (st_tx_video_session.c:3780), so the SR is burst right before
 * packet 0 once packet 0's target is reached; with PTP the same, with TSN on packet 0's
 * launch time. Pads (ST_TX_DUMMY_PKT_IDX) are freed, never sent, and the next SR counts
 * the 3 RTP packets of the frame only */
TEST_P(IpmxSt22SrPacingTest, SrRightBeforePacket0AndPadsNotCounted) {
  const PacingCase& c = GetParam();
  constexpr uint64_t kTarget = 1000000;
  constexpr uint64_t kTarget2 = kTarget + 16683333;
  constexpr uint32_t kPad = 0xffffffffu;
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_sr_trs_setup(ctx_, 1, 1000.0L, 0);
  if (!c.holds) {
    ASSERT_EQ(ut_sr_set_launch_time(ctx_, kPortP), 0);
  }
  ut_sr_set_frame_size(ctx_, kPortP, 3000);
  for (uint32_t idx : {0u, 1u, 2u, kPad, kPad})
    ASSERT_EQ(ut_sr_trs_enqueue(ctx_, idx, kRtpTs, kTarget), 0);
  ut_sr_set_frame_size(ctx_, kPortP, kPktLen);
  ASSERT_EQ(ut_sr_trs_enqueue(ctx_, 0, kRtpTs + 1501, kTarget2), 0);

  ut_sr_set_now(ctx_, kTarget - 1000);
  c.call(ctx_);
  if (c.holds) {
    EXPECT_EQ(ut_sr_wire_count(ctx_), 0);
  }
  ut_sr_set_now(ctx_, kTarget);
  for (int i = 0; i < 8; i++) c.call(ctx_);
  ut_sr_set_now(ctx_, kTarget2);
  for (int i = 0; i < 4; i++) c.call(ctx_);

  std::string wire;
  for (int i = 0; i < ut_sr_wire_count(ctx_); i++) {
    struct ut_sr_wire w = ut_sr_wire_at(ctx_, i);
    if (w.kind == UT_SR_WIRE_SR) wire += "S";
    if (w.kind == UT_SR_WIRE_PAD) wire += "D";
    if (w.kind == UT_SR_WIRE_PKT)
      wire += w.pkt_idx == kPad ? "D" : std::to_string(w.pkt_idx);
    if (!c.holds && w.kind == UT_SR_WIRE_SR) {
      ASSERT_LT(i + 1, ut_sr_wire_count(ctx_));
      EXPECT_TRUE(w.launch);
      EXPECT_EQ(w.launch_time, ut_sr_wire_at(ctx_, i + 1).launch_time);
    }
  }
  EXPECT_EQ(wire, "S012S0");
  std::vector<uint8_t> rtcp = LastRtcp();
  ASSERT_GE(rtcp.size(), 28u);
  EXPECT_EQ(Be32(&rtcp[20]), 3u);
  EXPECT_EQ(Be32(&rtcp[24]), 3000u + 3 * kPayloadHdr);
}

const PacingCase kPacingCases[] = {
    {"Tsc", ut_sr_call_tsc, true},
    {"Ptp", ut_sr_call_ptp, true},
    {"LaunchTime", ut_sr_call_launch_time, false},
};

INSTANTIATE_TEST_SUITE_P(Paths, IpmxSt22SrPacingTest, ::testing::ValuesIn(kPacingCases),
                         [](const ::testing::TestParamInfo<PacingCase>& info) {
                           return std::string(info.param.name);
                         });

/* ST 2022-7: each leg counts from its own packet 0 stamp (task 10 leg model), and the
 * two legs' SRs differ only in the CNAME after the same SR + Info Block */
TEST_F(IpmxSt22SrCountTest, EachLegCountsItsOwnPackets) {
  ut_sr_set_redundant(ctx_, kSipR, kDipR, kMediaPort);
  ASSERT_NO_FATAL_FAILURE(Attach());
  Frame(kPortP, 3000);
  Frame(kPortR, 3000);
  std::vector<uint8_t> p = Frame(kPortP, kPktLen);
  std::vector<uint8_t> r = Frame(kPortR, kPktLen);
  ASSERT_GE(p.size(), kSdesOffset);
  ASSERT_EQ(p.size(), r.size());
  EXPECT_EQ(0, memcmp(p.data(), r.data(), kSdesOffset));
  EXPECT_EQ(Be32(&p[20]), 3u);
  EXPECT_EQ(Be32(&p[24]), 3012u);

  Frame(kPortP, 2 * kPktLen);
  p = Frame(kPortP, kPktLen);
  r = Frame(kPortR, kPktLen);
  EXPECT_EQ(Be32(&p[20]), 3u + 1 + 2);
  EXPECT_EQ(Be32(&r[20]), 3u + 1);
  EXPECT_EQ(Be32(&r[24]), 3012u + kPktLen + kPayloadHdr);
}

/* frames can be one packet when the codestream shrinks, so the TX ring can hold one SR
 * every two descriptors: 64 / 2 + 4; st20 keeps its frame packet count rule */
TEST_F(IpmxSt22SrCountTest, SrPoolCoversOnePacketFrames) {
  EXPECT_EQ(ut_sr_pool_n(ctx_), 64u / 2 + 4);
}

/* ---- the public API ---- */

class IpmxSt22SrApiTest : public IpmxSt22SrBase {
 protected:
  uint8_t SrVersion() {
    std::vector<uint8_t> rtcp = Frame(kPortP, kPktLen);
    return rtcp.size() > kInfoOffset + 4 ? rtcp[kInfoOffset + 4] : 0xff;
  }
  std::string SrRefclk() {
    std::vector<uint8_t> rtcp = Frame(kPortP, kPktLen);
    if (rtcp.size() < kInfoOffset + 72) return "";
    return std::string(
        reinterpret_cast<const char*>(&rtcp[kInfoOffset + 8]),
        strnlen(reinterpret_cast<const char*>(&rtcp[kInfoOffset + 8]), 64));
  }
};

/* ipmx_st22[p]_tx_set_block_version() as the st20 one: the next SR carries the version */
TEST_F(IpmxSt22SrApiTest, SetBlockVersionAppliesToTheNextSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  EXPECT_EQ(SrVersion(), 0);
  ASSERT_EQ(ut_sr_api_st22_set_block_version(ctx_, false, true, 5), 0);
  EXPECT_EQ(SrVersion(), 5);
  ASSERT_EQ(ut_sr_api_st22_set_block_version(ctx_, true, true, 9), 0);
  EXPECT_EQ(SrVersion(), 9);
}

/* ipmx_st22[p]_tx_set_ts_refclk() as the st20 one: the next SR carries the string and,
 * after the first SR, one more block version (TR-10-1 section 8.7) */
TEST_F(IpmxSt22SrApiTest, SetTsRefclkAppliesAndBumpsTheVersion) {
  const std::string gm = "ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0";
  ASSERT_NO_FATAL_FAILURE(Attach());
  EXPECT_EQ(SrVersion(), 0);
  ASSERT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, false, true, gm.c_str()), 0);
  EXPECT_EQ(SrRefclk(), gm);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_api_st22_get_info(ctx_, false, true, &info), 0);
  EXPECT_EQ(info.version, 1);
  ASSERT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, true, true, nullptr), 0);
  EXPECT_EQ(SrRefclk(), "localmac=02-00-00-00-00-01");
  ASSERT_EQ(ut_sr_api_st22_get_info(ctx_, true, true, &info), 0);
  EXPECT_EQ(info.version, 2);
  EXPECT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, false, true, "sender"), -EINVAL);
}

/* ipmx_st22[p]_tx_get_info() returns the 0x0003 values for both handle kinds */
TEST_F(IpmxSt22SrApiTest, GetInfoReturnsTheMib0003Values) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (bool pipeline : {false, true}) {
    struct ipmx_video_info info;
    memset(&info, 0xa5, sizeof(info));
    ASSERT_EQ(ut_sr_api_st22_get_info(ctx_, pipeline, true, &info), 0);
    EXPECT_STREQ(info.sampling, "RGB");
    EXPECT_EQ(info.depth, 8);
    EXPECT_FALSE(info.gpm);
    EXPECT_EQ(info.width, 1920);
    EXPECT_STREQ(info.ts_refclk, "localmac=02-00-00-00-00-01");
    EXPECT_EQ(ut_sr_api_st22_get_info(ctx_, pipeline, true, nullptr), -EINVAL);
  }
}

/* -EIO, as the st20 ones: a session without the IPMX flag has no SR state, and a handle
 * of another type is refused by the handle guard */
TEST_F(IpmxSt22SrApiTest, EioWithoutIpmxOrOnAWrongHandle) {
  struct ipmx_video_info info;
  for (bool pipeline : {false, true}) {
    EXPECT_EQ(ut_sr_api_st22_set_block_version(ctx_, pipeline, true, 1), -EIO);
    EXPECT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, pipeline, true, nullptr), -EIO);
    EXPECT_EQ(ut_sr_api_st22_get_info(ctx_, pipeline, true, &info), -EIO);
  }
  ASSERT_NO_FATAL_FAILURE(Attach());
  for (bool pipeline : {false, true}) {
    EXPECT_EQ(ut_sr_api_st22_set_block_version(ctx_, pipeline, false, 1), -EIO);
    EXPECT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, pipeline, false, nullptr), -EIO);
    EXPECT_EQ(ut_sr_api_st22_get_info(ctx_, pipeline, false, &info), -EIO);
  }
}

/* ---- the st22 builder stamp ---- */

class IpmxSt22SrBuilderTest : public ::testing::Test {
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

/* packet 0 of each leg carries the frame's box + codestream bytes, so the transmitter
 * counts the frame although the builder runs a ring ahead of it; the size of each
 * frame, not the session maximum; with a bulk of 4 only packet 0 is stamped */
TEST_F(IpmxSt22SrBuilderTest, StampsPacket0OfEachLegWithItsFrameSize) {
  for (unsigned int bulk : {1u, 4u}) {
    for (size_t codestream : {2440u, 9000u}) {
      struct ut_txv_st22_pkt0 r;
      ASSERT_EQ(ut_txv_run_st22_pkt0(ctx_, codestream, 1000, 2, bulk, true, &r), 0);
      for (int i = 0; i < 2; i++) {
        for (unsigned int j = 0; j < bulk; j++) {
          uint32_t want = j ? 0 : codestream + kBoxes;
          EXPECT_EQ(r.frame_size[i][j], want) << "leg " << i << " slot " << j;
        }
        EXPECT_EQ(r.pkt_idx[i][0], 0u) << "leg " << i;
      }
    }
  }
}

/* a session without the SR state (IPMX flag off) gets no stamp */
TEST_F(IpmxSt22SrBuilderTest, NoStampWithoutAnSr) {
  struct ut_txv_st22_pkt0 r;
  ASSERT_EQ(ut_txv_run_st22_pkt0(ctx_, 2440, 1000, 1, 1, false, &r), 0);
  EXPECT_EQ(r.pkt_idx[0][0], 0u);
  EXPECT_EQ(r.frame_size[0][0], 0u);
}

}  // namespace
