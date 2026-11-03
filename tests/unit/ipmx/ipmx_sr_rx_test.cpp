/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * The IPMX receiver side of the RTCP Sender Report (TR-10-1 section 8.7, TR-10-2 section
 * 10): an st20 RX session with the IPMX flag reads the compound RTCP packets on media
 * port + 1 of each port and keeps the last Sender Report, its Info Block and CNAME for
 * ipmx_st20_rx_get_sr_info(). TR-10-1 section 8.7 says receivers shall tolerate the other
 * RFC 3550 RTCP packets, and a sender is not trusted: a malformed packet is counted and
 * dropped, never read past its end.
 *
 * - IpmxSrRxParseTest: ipmx_sr_rx_parse() over golden, compound and malformed packets.
 * - IpmxSrRxSessionTest: the per-port queue, the poll from the RX tasklet, the SSRC
 *   check, the counters and the getter return codes, on fake rxqs.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxSrRx*'
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ipmx/ipmx_info.h"
#include "ipmx/ipmx_sr_harness.h"
#include "ipmx/ipmx_sr_rx_harness.h"
#include "ipmx/ipmx_tr10_2_example.h"
#include "session/stderr_capture.h"

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kPtSr = 200;
constexpr uint8_t kPtRr = 201;
constexpr uint8_t kPtSdes = 202;
constexpr uint8_t kPtBye = 203;
constexpr uint8_t kPtApp = 204;
constexpr uint32_t kSsrc = 0x12345678;
constexpr int kPortP = 0;
constexpr int kPortR = 1;

void Put16(Bytes& b, uint16_t v) {
  b.push_back(v >> 8);
  b.push_back(v & 0xff);
}

void Put32(Bytes& b, uint32_t v) {
  Put16(b, v >> 16);
  Put16(b, v & 0xffff);
}

/* one RTCP packet: header with count and pt, body, optional padding octets */
Bytes Rtcp(uint8_t count, uint8_t pt, const Bytes& body, uint8_t pad = 0) {
  Bytes b;
  size_t len = 4 + body.size() + pad;
  b.push_back((2 << 6) | (pad ? 0x20 : 0) | (count & 0x1f));
  b.push_back(pt);
  Put16(b, len / 4 - 1);
  b.insert(b.end(), body.begin(), body.end());
  for (int i = 0; i < pad; i++) b.push_back(i == pad - 1 ? pad : 0);
  return b;
}

/* sender info then rc report blocks then ext, the profile-specific extension */
Bytes SrBody(uint32_t ssrc, uint32_t rtp_ts, const Bytes& ext, int rc = 0) {
  Bytes b;
  Put32(b, ssrc);
  Put32(b, 1700000000); /* NTP msw, PTP seconds */
  Put32(b, 123456789);  /* NTP lsw, PTP ns */
  Put32(b, rtp_ts);     /* RTP timestamp */
  Put32(b, rtp_ts + 1); /* packet count */
  Put32(b, rtp_ts + 2); /* octet count */
  for (int i = 0; i < rc * 24; i++) b.push_back(0xa5);
  b.insert(b.end(), ext.begin(), ext.end());
  return b;
}

Bytes Sr(uint32_t ssrc, uint32_t rtp_ts, const Bytes& ext, int rc = 0, uint8_t pad = 0) {
  return Rtcp(rc, kPtSr, SrBody(ssrc, rtp_ts, ext, rc), pad);
}

/* one chunk with a CNAME item, null terminated to the word end */
Bytes Sdes(uint32_t ssrc, const std::string& cname, uint8_t pad = 0) {
  Bytes b;
  Put32(b, ssrc);
  b.push_back(1);
  b.push_back(cname.size());
  b.insert(b.end(), cname.begin(), cname.end());
  do b.push_back(0);
  while (b.size() % 4);
  return Rtcp(1, kPtSdes, b, pad);
}

Bytes Rr() {
  Bytes b;
  Put32(b, 0xcafe);
  for (int i = 0; i < 24; i++) b.push_back(0x11);
  return Rtcp(1, kPtRr, b);
}

Bytes Bye() {
  Bytes b;
  Put32(b, kSsrc);
  return Rtcp(1, kPtBye, b);
}

Bytes App() {
  Bytes b;
  Put32(b, kSsrc);
  for (char c : std::string("TEST")) b.push_back(c);
  Put32(b, 0xdeadbeef);
  return Rtcp(0, kPtApp, b);
}

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
  return b;
}

struct ipmx_video_info VideoInfo() {
  struct ipmx_video_info info;
  memset(&info, 0, sizeof(info));
  info.version = 7;
  snprintf(info.ts_refclk, sizeof(info.ts_refclk), "localmac=02-00-00-00-00-01");
  snprintf(info.mediaclk, sizeof(info.mediaclk), "direct=0");
  snprintf(info.sampling, sizeof(info.sampling), "RGB");
  info.floating = true;
  info.depth = 0x7f;
  info.gpm = true;
  info.interlaced = true;
  info.segmented = true;
  info.par_w = 12;
  info.par_h = 11;
  snprintf(info.range, sizeof(info.range), "FULL");
  snprintf(info.colorimetry, sizeof(info.colorimetry), "BT2020");
  snprintf(info.tcs, sizeof(info.tcs), "PQ");
  info.width = 3840;
  info.height = 2160;
  info.rate_num = 0x3fffff;
  info.rate_den = 0x3ff;
  info.pixclk = 0x123456789abcull;
  info.htotal = 4400;
  info.vtotal = 2250;
  return info;
}

Bytes InfoBlock(const struct ipmx_video_info& info) {
  Bytes b(IPMX_INFO_BLOCK_LEN);
  ipmx_info_block_build(&info, b.data());
  return b;
}

/* an IPMX Info Block holding mibs after its 84 octet header */
Bytes InfoBlockWith(const std::vector<Bytes>& mibs, uint8_t version = 0) {
  Bytes b = InfoBlock(VideoInfo());
  b.resize(84);
  b[4] = version;
  for (const Bytes& m : mibs) b.insert(b.end(), m.begin(), m.end());
  b[2] = (b.size() / 4 - 1) >> 8;
  b[3] = (b.size() / 4 - 1) & 0xff;
  return b;
}

Bytes VideoMib() {
  Bytes b = InfoBlock(VideoInfo());
  return Bytes(b.begin() + 84, b.end());
}

Bytes UnknownMib(uint16_t type, int words) {
  Bytes b;
  Put16(b, type);
  Put16(b, words - 1);
  for (int i = 4; i < words * 4; i++) b.push_back(0x5a);
  return b;
}

void ExpectInfoEq(const struct ipmx_video_info& a, const struct ipmx_video_info& b) {
  EXPECT_EQ(a.version, b.version);
  EXPECT_EQ(0, memcmp(a.ts_refclk, b.ts_refclk, sizeof(a.ts_refclk)));
  EXPECT_EQ(0, memcmp(a.mediaclk, b.mediaclk, sizeof(a.mediaclk)));
  EXPECT_EQ(0, memcmp(a.sampling, b.sampling, sizeof(a.sampling)));
  EXPECT_EQ(a.floating, b.floating);
  EXPECT_EQ(a.depth, b.depth);
  EXPECT_EQ(a.gpm, b.gpm);
  EXPECT_EQ(a.interlaced, b.interlaced);
  EXPECT_EQ(a.segmented, b.segmented);
  EXPECT_EQ(a.par_w, b.par_w);
  EXPECT_EQ(a.par_h, b.par_h);
  EXPECT_EQ(0, memcmp(a.range, b.range, sizeof(a.range)));
  EXPECT_EQ(0, memcmp(a.colorimetry, b.colorimetry, sizeof(a.colorimetry)));
  EXPECT_EQ(0, memcmp(a.tcs, b.tcs, sizeof(a.tcs)));
  EXPECT_EQ(a.width, b.width);
  EXPECT_EQ(a.height, b.height);
  EXPECT_EQ(a.rate_num, b.rate_num);
  EXPECT_EQ(a.rate_den, b.rate_den);
  EXPECT_EQ(a.pixclk, b.pixclk);
  EXPECT_EQ(a.htotal, b.htotal);
  EXPECT_EQ(a.vtotal, b.vtotal);
}

/* a fixed-size, zero padded Info Block string, which a full field leaves unterminated */
template <size_t N>
std::string Str(const char (&field)[N]) {
  return std::string(field, strnlen(field, N));
}

int Parse(const Bytes& pkt, struct ipmx_rx_sr_info* sr) {
  memset(sr, 0xee, sizeof(*sr)); /* nothing kept from before */
  return ut_srrx_parse(pkt.data(), pkt.size(), sr);
}

/* a well formed IPMX SR + SDES CNAME, as an IPMX sender sends it */
Bytes IpmxSr(uint32_t ssrc = kSsrc, uint32_t rtp_ts = 1000,
             const struct ipmx_video_info& info = VideoInfo()) {
  return Cat({Sr(ssrc, rtp_ts, InfoBlock(info)), Sdes(ssrc, "192.168.1.10")});
}

/* ---- ipmx_sr_rx_parse(): golden packets ---- */

class IpmxSrRxParseTest : public ::testing::Test {};

/* the TR-10-2 section 11 example reads back as the values that section lists */
TEST_F(IpmxSrRxParseTest, Tr10_2ExampleParses) {
  Bytes pkt(kTr10_2ExampleSr, kTr10_2ExampleSr + sizeof(kTr10_2ExampleSr));
  pkt.insert(pkt.end(), kTr10_2Example, kTr10_2Example + sizeof(kTr10_2Example));
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(pkt, &sr), 0);
  EXPECT_EQ(sr.ssrc, 3254u);
  EXPECT_EQ(sr.ptp_sec, 1665165600u);
  EXPECT_EQ(sr.ptp_ns, 262167158u);
  EXPECT_EQ(sr.rtp_timestamp, 610164507u);
  EXPECT_EQ(sr.pkt_count, 0u);
  EXPECT_EQ(sr.octet_count, 0u);
  EXPECT_EQ(sr.received, 0u);
  EXPECT_EQ(sr.unknown_mib, 0u);
  EXPECT_STREQ(sr.cname, "");
  ASSERT_TRUE(sr.has_info);
  ASSERT_TRUE(sr.has_video_mib);
  EXPECT_EQ(sr.info.version, 1);
  EXPECT_EQ(Str(sr.info.ts_refclk), "localmac=00-20-FC-32-2F-40");
  EXPECT_EQ(Str(sr.info.mediaclk), "sender");
  EXPECT_EQ(Str(sr.info.sampling), "YCbCr-4:2:2");
  EXPECT_FALSE(sr.info.floating);
  EXPECT_EQ(sr.info.depth, 10);
  EXPECT_TRUE(sr.info.gpm);
  EXPECT_FALSE(sr.info.interlaced);
  EXPECT_FALSE(sr.info.segmented);
  EXPECT_EQ(sr.info.par_w, 1);
  EXPECT_EQ(sr.info.par_h, 1);
  EXPECT_EQ(Str(sr.info.range), "NARROW");
  EXPECT_EQ(Str(sr.info.colorimetry), "BT709");
  EXPECT_EQ(Str(sr.info.tcs), "SDR");
  EXPECT_EQ(sr.info.width, 1920);
  EXPECT_EQ(sr.info.height, 1080);
  EXPECT_EQ(sr.info.rate_num, 60000u);
  EXPECT_EQ(sr.info.rate_den, 1001u);
  EXPECT_EQ(sr.info.pixclk, 148550104u);
  EXPECT_EQ(sr.info.htotal, 2200);
  EXPECT_EQ(sr.info.vtotal, 1125);
}

/* the SR + SDES the MTL transmitter builds reads back as the session's own values */
TEST_F(IpmxSrRxParseTest, TxBuiltSrRoundTrips) {
  const uint8_t sip[4] = {192, 168, 1, 10};
  constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull + 123456789;
  ASSERT_EQ(ut_sr_init(), 0);
  ut_sr_ctx* tx = ut_sr_create(sip, kSsrc, 20000);
  ASSERT_NE(tx, nullptr);
  ASSERT_EQ(ut_sr_attach(tx), 0);
  uint8_t rtcp[UT_SR_PKT_MAX];
  uint16_t len = ut_sr_build(tx, rtcp, kTaiNs, 0xa1b2c3d4);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(tx, &info), 0);
  ut_sr_destroy(tx);

  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Bytes(rtcp, rtcp + len), &sr), 0);
  EXPECT_EQ(sr.ssrc, kSsrc);
  EXPECT_EQ(sr.ptp_sec, 1700000000u);
  EXPECT_EQ(sr.ptp_ns, 123456789u);
  EXPECT_EQ(sr.rtp_timestamp, 0xa1b2c3d4u);
  EXPECT_STREQ(sr.cname, "192.168.1.10");
  ASSERT_TRUE(sr.has_info);
  EXPECT_TRUE(sr.has_video_mib);
  ExpectInfoEq(sr.info, info);
}

/* every Info Block field, the flag bits and the field limits survive build then parse */
TEST_F(IpmxSrRxParseTest, EveryInfoFieldRoundTrips) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(IpmxSr(), &sr), 0);
  ASSERT_TRUE(sr.has_info);
  ExpectInfoEq(sr.info, VideoInfo());
  EXPECT_EQ(sr.pkt_count, 1001u);
  EXPECT_EQ(sr.octet_count, 1002u);
}

/* ---- compound packets: TR-10-1 section 8.7 asks receivers to tolerate the others ----
 */

/* an RR first and BYE / APP after the SR and SDES are skipped */
TEST_F(IpmxSrRxParseTest, OtherPacketsBeforeAndAfterSkipped) {
  struct ipmx_rx_sr_info sr;
  Bytes pkt = Cat({Rr(), App(), Sr(kSsrc, 5, InfoBlock(VideoInfo())), Sdes(kSsrc, "a.b"),
                   Bye(), App()});
  ASSERT_EQ(Parse(pkt, &sr), 0);
  EXPECT_EQ(sr.rtp_timestamp, 5u);
  EXPECT_STREQ(sr.cname, "a.b");
  EXPECT_TRUE(sr.has_info);
}

/* a compound with no SR (an RR only sender) has nothing to report, it is not malformed */
TEST_F(IpmxSrRxParseTest, NoSrIsNotFound) {
  struct ipmx_rx_sr_info sr;
  EXPECT_EQ(Parse(Cat({Rr(), Sdes(kSsrc, "x"), Bye()}), &sr), -ENOENT);
}

/* report blocks (RC > 0) sit before the profile extension and are skipped */
TEST_F(IpmxSrRxParseTest, ReportBlocksSkipped) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Sr(kSsrc, 1, InfoBlock(VideoInfo()), 3), &sr), 0);
  ASSERT_TRUE(sr.has_info);
  ExpectInfoEq(sr.info, VideoInfo());
}

/* the padding octets of the last packet are not read as packet content */
TEST_F(IpmxSrRxParseTest, PaddingHonored) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(
      Parse(Cat({Sr(kSsrc, 1, InfoBlock(VideoInfo())), Sdes(kSsrc, "abc", 8)}), &sr), 0);
  EXPECT_STREQ(sr.cname, "abc");
  /* padding inside the SR after the Info Block */
  ASSERT_EQ(Parse(Sr(kSsrc, 1, InfoBlock(VideoInfo()), 0, 4), &sr), 0);
  EXPECT_TRUE(sr.has_info);
}

/* a Media Info Block of another type is counted and skipped, the video one still read */
TEST_F(IpmxSrRxParseTest, UnknownMibSkipped) {
  struct ipmx_rx_sr_info sr;
  Bytes block = InfoBlockWith({UnknownMib(0x0002, 3), VideoMib(), UnknownMib(0x7fff, 1)});
  ASSERT_EQ(Parse(Sr(kSsrc, 1, block), &sr), 0);
  EXPECT_EQ(sr.unknown_mib, 2u);
  ASSERT_TRUE(sr.has_info);
  EXPECT_EQ(sr.info.width, 3840);
}

/* an Info Block with no video Media Info Block still gives its version, ts-refclk and
 * mediaclk (TR-10-1 section 8.7 allows 0 or more Media Info Blocks) */
TEST_F(IpmxSrRxParseTest, InfoBlockWithoutVideoMibKeepsBlockFields) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Sr(kSsrc, 1, InfoBlockWith({UnknownMib(0x0003, 2)}, 6)), &sr), 0);
  EXPECT_EQ(sr.unknown_mib, 1u);
  ASSERT_TRUE(sr.has_info);
  EXPECT_FALSE(sr.has_video_mib);
  EXPECT_EQ(sr.info.version, 6);
  EXPECT_EQ(Str(sr.info.ts_refclk), "localmac=02-00-00-00-00-01");
  EXPECT_EQ(Str(sr.info.mediaclk), "direct=0");
  EXPECT_EQ(Str(sr.info.sampling), "");
  EXPECT_EQ(sr.info.width, 0);
}

/* an Info Block of the header alone, no Media Info Block at all */
TEST_F(IpmxSrRxParseTest, InfoBlockHeaderOnly) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Sr(kSsrc, 1, InfoBlockWith({}, 2)), &sr), 0);
  EXPECT_TRUE(sr.has_info);
  EXPECT_FALSE(sr.has_video_mib);
  EXPECT_EQ(sr.info.version, 2);
}

/* a plain RFC 3550 SR, and one with another profile extension, are SRs without info */
TEST_F(IpmxSrRxParseTest, SrWithoutInfoBlock) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Cat({Sr(kSsrc, 77, {}), Sdes(kSsrc, "c")}), &sr), 0);
  EXPECT_EQ(sr.rtp_timestamp, 77u);
  EXPECT_FALSE(sr.has_info);
  EXPECT_FALSE(sr.has_video_mib);
  struct ipmx_video_info zero;
  memset(&zero, 0, sizeof(zero));
  EXPECT_EQ(0, memcmp(&sr.info, &zero, sizeof(zero)));
  Bytes other = UnknownMib(0x1234, 4);
  ASSERT_EQ(Parse(Sr(kSsrc, 77, other), &sr), 0);
  EXPECT_FALSE(sr.has_info);
}

/* the CNAME is the one of the SR's SSRC; another source's chunk is not it */
TEST_F(IpmxSrRxParseTest, CnameOfOtherSsrcIgnored) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Cat({Sr(kSsrc, 1, {}), Sdes(kSsrc + 1, "other")}), &sr), 0);
  EXPECT_STREQ(sr.cname, "");
}

/* a 255 octet CNAME, the longest an SDES item holds, is kept whole and terminated */
TEST_F(IpmxSrRxParseTest, LongestCnameKept) {
  struct ipmx_rx_sr_info sr;
  std::string cname(255, 'n');
  ASSERT_EQ(Parse(Cat({Sr(kSsrc, 1, {}), Sdes(kSsrc, cname)}), &sr), 0);
  EXPECT_EQ(std::string(sr.cname), cname);
}

/* only the first SR of a compound is reported */
TEST_F(IpmxSrRxParseTest, FirstSrKept) {
  struct ipmx_rx_sr_info sr;
  ASSERT_EQ(Parse(Cat({Sr(kSsrc, 1, {}), Sr(kSsrc, 2, {})}), &sr), 0);
  EXPECT_EQ(sr.rtp_timestamp, 1u);
}

/* ---- malformed packets: dropped whole, never read past the end ---- */

struct MalformedCase {
  const char* name;
  Bytes pkt;
};

/* the SR with its Info Block length field set to words - 1 */
Bytes SrInfoLen(uint16_t words_minus_1) {
  Bytes block = InfoBlock(VideoInfo());
  block[2] = words_minus_1 >> 8;
  block[3] = words_minus_1 & 0xff;
  return Sr(kSsrc, 1, block);
}

Bytes Truncated(Bytes b, size_t n) {
  b.resize(b.size() - n);
  return b;
}

Bytes WithByte(Bytes b, size_t i, uint8_t v) {
  b[i] = v;
  return b;
}

std::vector<MalformedCase> MalformedCases() {
  Bytes sr = Sr(kSsrc, 1, InfoBlock(VideoInfo()));
  Bytes sdes = Sdes(kSsrc, "abc");
  Bytes sdes_item_past = sdes;
  sdes_item_past[9] = 40; /* CNAME length past the chunk */
  Bytes padded = Sdes(kSsrc, "abc", 4);
  return {
      {"Empty", {}},
      {"ShortHeader", {0x80, 0xc8, 0x00}},
      {"Version1", WithByte(sr, 0, 0x40)},
      {"Version0InLaterPacket", Cat({sr, WithByte(sdes, 0, 0x01)})},
      {"LengthPastBuffer", Truncated(sr, 4)},
      {"TrailingOctets", Cat({sr, {0x80, 0xc8}})},
      {"SrShorterThanSenderInfo", Rtcp(0, kPtSr, {0, 0, 0, 1})},
      {"ReportBlocksPastPacket", Rtcp(2, kPtSr, SrBody(kSsrc, 1, {}, 1))},
      {"PaddingZero", WithByte(padded, padded.size() - 1, 0)},
      {"PaddingPastPacket", WithByte(padded, padded.size() - 1, 200)},
      {"SdesItemPastChunk", Cat({sr, sdes_item_past})},
      {"SdesChunkTruncated", Cat({sr, Rtcp(1, kPtSdes, {})})},
  };
}

class IpmxSrRxMalformedTest : public ::testing::TestWithParam<MalformedCase> {};

/* each is rejected as malformed, not as "no SR" */
TEST_P(IpmxSrRxMalformedTest, Rejected) {
  struct ipmx_rx_sr_info sr;
  const Bytes& pkt = GetParam().pkt;
  /* reads past the packet are caught by the ipmx_sr_parse_fuzz ASan target, not here */
  EXPECT_EQ(ut_srrx_parse(pkt.empty() ? nullptr : pkt.data(), pkt.size(), &sr), -EINVAL);
}

INSTANTIATE_TEST_SUITE_P(Cases, IpmxSrRxMalformedTest,
                         ::testing::ValuesIn(MalformedCases()),
                         [](const ::testing::TestParamInfo<MalformedCase>& info) {
                           return std::string(info.param.name);
                         });

/* ---- Info Block defects: the SR stays, the block or its MIB walk does not ---- */

struct InfoDefectCase {
  const char* name;
  Bytes pkt;
  bool has_info;      /* the block header was valid */
  bool has_video_mib; /* a valid video MIB came before the defect */
};

std::vector<InfoDefectCase> InfoDefectCases() {
  Bytes video_mib = VideoMib();
  Bytes short_mib = video_mib;
  short_mib.resize(88);
  short_mib[3] = 88 / 4 - 1;
  Bytes long_mib_len = video_mib;
  long_mib_len[3] = 23; /* 96 octets, past the block */
  Bytes long_unknown = UnknownMib(0x0005, 2);
  long_unknown[3] = 9; /* 40 octets, past the block */
  return {
      {"InfoBlockPastSr", SrInfoLen(IPMX_INFO_BLOCK_LEN / 4), false, false},
      {"InfoBlockShorterThanHeader", SrInfoLen(84 / 4 - 2), false, false},
      {"MibPastInfoBlock", Sr(kSsrc, 1, InfoBlockWith({long_mib_len})), true, false},
      {"VideoMibTooShort", Sr(kSsrc, 1, InfoBlockWith({short_mib})), true, false},
      {"MibPastInfoBlockAfterVideoMib",
       Sr(kSsrc, 1, InfoBlockWith({video_mib, long_unknown})), true, true},
  };
}

class IpmxSrRxInfoDefectTest : public ::testing::TestWithParam<InfoDefectCase> {};

/* the SR fields and CNAME are kept with info_malformed set; a header defect drops the
 * block, a MIB defect keeps the header and the MIBs before it */
TEST_P(IpmxSrRxInfoDefectTest, SrKept) {
  struct ipmx_rx_sr_info sr;
  const InfoDefectCase& c = GetParam();
  ASSERT_EQ(Parse(Cat({c.pkt, Sdes(kSsrc, "d")}), &sr), 0);
  EXPECT_EQ(sr.ssrc, kSsrc);
  EXPECT_EQ(sr.rtp_timestamp, 1u);
  EXPECT_EQ(sr.ptp_sec, 1700000000u);
  EXPECT_STREQ(sr.cname, "d");
  EXPECT_EQ(sr.info_malformed, 1u);
  EXPECT_EQ(sr.has_info, c.has_info);
  EXPECT_EQ(sr.has_video_mib, c.has_video_mib);
  if (c.has_info) {
    EXPECT_EQ(Str(sr.info.ts_refclk), "localmac=02-00-00-00-00-01");
  } else {
    struct ipmx_video_info zero;
    memset(&zero, 0, sizeof(zero));
    EXPECT_EQ(0, memcmp(&sr.info, &zero, sizeof(zero)));
  }
  EXPECT_EQ(sr.info.width, c.has_video_mib ? 3840 : 0);
}

INSTANTIATE_TEST_SUITE_P(Cases, IpmxSrRxInfoDefectTest,
                         ::testing::ValuesIn(InfoDefectCases()),
                         [](const ::testing::TestParamInfo<InfoDefectCase>& info) {
                           return std::string(info.param.name);
                         });

/* ---- the session: queue, poll, counters, getter ---- */

class IpmxSrRxSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_srrx_init(), 0);
  }
  void TearDown() override {
    ut_srrx_destroy(ctx_);
  }
  void Create(int num_port = 1, uint32_t flags = ST20_RX_FLAG_IPMX) {
    ctx_ = ut_srrx_create(num_port, flags);
    ASSERT_NE(ctx_, nullptr);
  }
  void Attach(int num_port = 1) {
    ASSERT_NO_FATAL_FAILURE(Create(num_port));
    ASSERT_EQ(ut_srrx_attach(ctx_), 0);
    ASSERT_TRUE(ut_srrx_has_state(ctx_));
  }
  void Receive(const Bytes& pkt, int port = kPortP) {
    ASSERT_EQ(ut_srrx_push(ctx_, port, pkt.data(), pkt.size()), 0);
    ASSERT_EQ(ut_srrx_poll(ctx_, port), 1);
  }
  struct ipmx_rx_sr_info Get(int port = kPortP, int expect = 0) {
    struct ipmx_rx_sr_info info;
    memset(&info, 0xee, sizeof(info));
    EXPECT_EQ(ut_srrx_get(ctx_, port, &info), expect);
    return info;
  }
  ut_srrx_ctx* ctx_ = nullptr;
};

/* each port gets one queue: the media flow on port + 1 (TR-10-1 section 8.7), same
 * destination and source, no header split as the SR is not video */
TEST_F(IpmxSrRxSessionTest, QueuePerPortOnMediaPortPlusOne) {
  ASSERT_NO_FATAL_FAILURE(Attach(2));
  EXPECT_EQ(ut_srrx_rxq_gets(ctx_), 2);
  for (int port : {kPortP, kPortR}) {
    struct ut_srrx_flow flow;
    ut_srrx_flow(ctx_, port, &flow);
    const uint8_t dip[4] = {239, 0, 0, (uint8_t)(1 + port)};
    const uint8_t sip[4] = {192, 168, 1, (uint8_t)(10 + port)};
    EXPECT_EQ(0, memcmp(flow.dip, dip, 4)) << port;
    EXPECT_EQ(0, memcmp(flow.sip, sip, 4)) << port;
    EXPECT_EQ(flow.dst_port, 20001) << port;
    EXPECT_FALSE(flow.hdr_split) << port;
    EXPECT_FALSE(flow.hdr_split_cb) << port;
    EXPECT_FALSE(flow.force_cni) << port;
  }
}

/* with the CNI tasklet a port with a CNI queue takes the SRs from it, a software ring,
 * not one more NIC queue; a port without one keeps its own queue */
TEST_F(IpmxSrRxSessionTest, CniTaskletPortUsesCniQueue) {
  ASSERT_NO_FATAL_FAILURE(Create(2));
  ut_srrx_set_cni(ctx_, kPortP, true, true);
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  struct ut_srrx_flow flow;
  ut_srrx_flow(ctx_, kPortP, &flow);
  EXPECT_TRUE(flow.force_cni);
  EXPECT_EQ(flow.dst_port, 20001);
  ut_srrx_flow(ctx_, kPortR, &flow);
  EXPECT_FALSE(flow.force_cni);
}

/* a CNI thread busy-polls for good once a CNI ring exists, so in thread mode the SRs keep
 * their own queue */
TEST_F(IpmxSrRxSessionTest, CniThreadPortUsesOwnQueue) {
  ASSERT_NO_FATAL_FAILURE(Create());
  ut_srrx_set_cni(ctx_, kPortP, true, false);
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  struct ut_srrx_flow flow;
  ut_srrx_flow(ctx_, kPortP, &flow);
  EXPECT_FALSE(flow.force_cni);
}

/* without the IPMX flag nothing is allocated and no queue taken: ST 2110 RX unchanged */
TEST_F(IpmxSrRxSessionTest, FlagOffCreatesNothing) {
  ASSERT_NO_FATAL_FAILURE(Create(1, 0));
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  EXPECT_FALSE(ut_srrx_has_state(ctx_));
  EXPECT_EQ(ut_srrx_rxq_gets(ctx_), 0);
  struct ipmx_rx_sr_info info;
  EXPECT_EQ(ut_srrx_get(ctx_, kPortP, &info), -EIO);
}

/* st22_rx_create() maps ST22_RX_FLAG_IPMX to ST20_RX_FLAG_IPMX, but the st22 SR parse
 * is not there yet, so an st22 session takes no SR queue and the getter has no state */
TEST_F(IpmxSrRxSessionTest, St22SessionCreatesNothing) {
  ASSERT_NO_FATAL_FAILURE(Create());
  ut_srrx_set_st22_flags(ctx_, ST22_RX_FLAG_IPMX);
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  EXPECT_FALSE(ut_srrx_has_state(ctx_));
  EXPECT_EQ(ut_srrx_rxq_gets(ctx_), 0);
  struct ipmx_rx_sr_info info;
  EXPECT_EQ(ut_srrx_get(ctx_, kPortP, &info), -ENOTSUP);
}

/* data path only: the app owns the flows, so no SR queue and the getter says so */
TEST_F(IpmxSrRxSessionTest, DataPathOnlyHasNoQueue) {
  ASSERT_NO_FATAL_FAILURE(Create(1, ST20_RX_FLAG_IPMX | ST20_RX_FLAG_DATA_PATH_ONLY));
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  EXPECT_FALSE(ut_srrx_has_state(ctx_));
  EXPECT_EQ(ut_srrx_rxq_gets(ctx_), 0);
  struct ipmx_rx_sr_info info;
  EXPECT_EQ(ut_srrx_get(ctx_, kPortP, &info), -ENOTSUP);
}

/* a failed queue get fails the attach, and detach then leaves nothing behind */
TEST_F(IpmxSrRxSessionTest, QueueGetFailureFails) {
  ASSERT_NO_FATAL_FAILURE(Create(2));
  ut_srrx_fail_next_get(ctx_);
  EXPECT_EQ(ut_srrx_attach(ctx_), -EIO);
  ut_srrx_detach(ctx_);
  EXPECT_FALSE(ut_srrx_has_state(ctx_));
}

/* before any SR the getter says -EBUSY but still gives the counters */
TEST_F(IpmxSrRxSessionTest, BusyBeforeFirstSr) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(Bytes{0x40, 0xc8, 0x00, 0x00}); /* malformed */
  struct ipmx_rx_sr_info info = Get(kPortP, -EBUSY);
  EXPECT_EQ(info.received, 0u);
  EXPECT_EQ(info.malformed, 1u);
}

/* the getter rejects a port the session does not use and a NULL info */
TEST_F(IpmxSrRxSessionTest, InvalidArgsRejected) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  struct ipmx_rx_sr_info info;
  EXPECT_EQ(ut_srrx_get(ctx_, kPortR, &info), -EINVAL);
  EXPECT_EQ(ut_srrx_get(ctx_, kPortP, nullptr), -EINVAL);
}

/* the public getters check the handle type; st20p reaches the transport session */
TEST_F(IpmxSrRxSessionTest, PublicGetters) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  struct ipmx_rx_sr_info info;
  EXPECT_EQ(ut_srrx_api_get(ctx_, kPortP, &info, false), -EIO);
  EXPECT_EQ(ut_srrx_api_get_pipeline(ctx_, kPortP, &info, false), -EIO);
  EXPECT_EQ(ut_srrx_api_get(ctx_, kPortP, &info, true), -EBUSY);
  Receive(IpmxSr());
  EXPECT_EQ(ut_srrx_api_get(ctx_, kPortP, &info, true), 0);
  EXPECT_EQ(ut_srrx_api_get_pipeline(ctx_, kPortP, &info, true), 0);
  EXPECT_EQ(info.received, 1u);
}

/* an SR is published with its arrival time and every mbuf goes back to the pool */
TEST_F(IpmxSrRxSessionTest, SrPublished) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_srrx_set_ptp(ctx_, 5555);
  Receive(IpmxSr(kSsrc, 42));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 1u);
  EXPECT_EQ(info.malformed, 0u);
  EXPECT_EQ(info.no_info_block, 0u);
  EXPECT_EQ(info.ssrc, kSsrc);
  EXPECT_EQ(info.rtp_timestamp, 42u);
  EXPECT_EQ(info.ptp_sec, 1700000000u);
  EXPECT_EQ(info.ptp_ns, 123456789u);
  EXPECT_EQ(info.arrival_ns, 5555u);
  EXPECT_STREQ(info.cname, "192.168.1.10");
  ASSERT_TRUE(info.has_info);
  EXPECT_TRUE(info.has_video_mib);
  ExpectInfoEq(info.info, VideoInfo());
  EXPECT_EQ(ut_srrx_mbufs_in_use(ctx_), 0u);
}

/* a burst of several SRs leaves the last one, and counts each; the fake queue reads as
 * a vector PMD, which returns nothing for a burst below its descriptor batch */
TEST_F(IpmxSrRxSessionTest, BurstKeepsLast) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  const uint32_t total = ut_srrx_burst_size() + 2;
  for (uint32_t ts = 1; ts <= total; ts++) {
    Bytes pkt = IpmxSr(kSsrc, ts);
    ASSERT_EQ(ut_srrx_push(ctx_, kPortP, pkt.data(), pkt.size()), 0);
  }
  EXPECT_EQ(ut_srrx_poll(ctx_, kPortP), ut_srrx_burst_size());
  EXPECT_EQ(ut_srrx_poll(ctx_, kPortP), 2);
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, total);
  EXPECT_EQ(info.rtp_timestamp, total);
  EXPECT_EQ(ut_srrx_mbufs_in_use(ctx_), 0u);
}

/* a malformed packet is counted and the last good SR stays */
TEST_F(IpmxSrRxSessionTest, MalformedCountedAndDropped) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(IpmxSr(kSsrc, 1));
  Receive(Truncated(IpmxSr(kSsrc, 2), 4));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 1u);
  EXPECT_EQ(info.malformed, 1u);
  EXPECT_EQ(info.rtp_timestamp, 1u);
}

/* IPv4 options, a runt and a UDP length below its header are malformed at the UDP layer
 */
TEST_F(IpmxSrRxSessionTest, BadUdpFramesMalformed) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Bytes pkt = IpmxSr();
  ASSERT_EQ(ut_srrx_push(ctx_, kPortP, pkt.data(), pkt.size()), 0);
  ASSERT_EQ(ut_srrx_poll(ctx_, kPortP), 1);
  /* build the three from a good frame: eth 14, ipv4 20, udp 8 */
  uint8_t frame[512];
  ASSERT_LE(42 + pkt.size(), sizeof(frame));
  memset(frame, 0, sizeof(frame));
  frame[12] = 0x08;
  frame[14] = 0x46; /* IHL 6 */
  ASSERT_EQ(ut_srrx_push_raw(ctx_, kPortP, frame, 42 + pkt.size()), 0);
  ASSERT_EQ(ut_srrx_push_raw(ctx_, kPortP, frame, 20), 0); /* runt */
  frame[14] = 0x45;
  frame[38] = 0;
  frame[39] = 7; /* udp length 7 */
  ASSERT_EQ(ut_srrx_push_raw(ctx_, kPortP, frame, 42 + pkt.size()), 0);
  EXPECT_EQ(ut_srrx_poll(ctx_, kPortP), 3);
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 1u);
  EXPECT_EQ(info.malformed, 3u);
}

/* Ethernet padding after the UDP datagram is not parsed as RTCP */
TEST_F(IpmxSrRxSessionTest, EthernetPaddingIgnored) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Bytes pkt = Sr(kSsrc, 3, {});
  Bytes padded = pkt;
  padded.insert(padded.end(), {0x00, 0x00, 0x00});
  uint8_t frame[256];
  memset(frame, 0, sizeof(frame));
  frame[12] = 0x08;
  frame[14] = 0x45;
  frame[38] = 0;
  frame[39] = 8 + pkt.size();
  memcpy(frame + 42, padded.data(), padded.size());
  ASSERT_EQ(ut_srrx_push_raw(ctx_, kPortP, frame, 42 + padded.size()), 0);
  ASSERT_EQ(ut_srrx_poll(ctx_, kPortP), 1);
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.malformed, 0u);
  EXPECT_EQ(info.rtp_timestamp, 3u);
}

/* an SR without an Info Block is accepted and counted in no_info_block */
TEST_F(IpmxSrRxSessionTest, NoInfoBlockCounted) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(IpmxSr(kSsrc, 1));
  Receive(Cat({Sr(kSsrc, 2, {}), Sdes(kSsrc, "x")}));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 2u);
  EXPECT_EQ(info.no_info_block, 1u);
  EXPECT_FALSE(info.has_info);
  EXPECT_EQ(info.info.width, 0);
  EXPECT_STREQ(info.cname, "x");
}

/* unknown Media Info Blocks add up over SRs */
TEST_F(IpmxSrRxSessionTest, UnknownMibCounted) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Bytes sr = Sr(kSsrc, 1, InfoBlockWith({UnknownMib(9, 2), VideoMib()}));
  Receive(sr);
  Receive(sr);
  EXPECT_EQ(Get().unknown_mib, 2u);
}

/* TR-10-1 section 8.7: the SR SSRC shall match the media; before media any SR is taken,
 * after media only the media SSRC */
TEST_F(IpmxSrRxSessionTest, SsrcMismatchDropped) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(IpmxSr(kSsrc + 5, 1));
  EXPECT_EQ(Get().received, 1u);
  ut_srrx_media(ctx_, kPortP, kSsrc);
  Receive(IpmxSr(kSsrc + 5, 2));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 1u);
  EXPECT_EQ(info.ssrc_mismatch, 1u);
  EXPECT_EQ(info.rtp_timestamp, 1u);
  Receive(IpmxSr(kSsrc, 3));
  info = Get();
  EXPECT_EQ(info.received, 2u);
  EXPECT_EQ(info.ssrc, kSsrc);
}

/* with the ops ssrc set the media RX drops other SSRCs, so that ssrc is the reference
 * from the start and a media packet of another SSRC does not move it */
TEST_F(IpmxSrRxSessionTest, OpsSsrcIsTheReference) {
  ASSERT_NO_FATAL_FAILURE(Create());
  ut_srrx_set_ops_ssrc(ctx_, kSsrc);
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  Receive(IpmxSr(kSsrc + 1, 1)); /* no media yet, still checked */
  EXPECT_EQ(Get(kPortP, -EBUSY).ssrc_mismatch, 1u);
  ut_srrx_media(ctx_, kPortP, kSsrc + 1); /* a packet the media RX dropped */
  Receive(IpmxSr(kSsrc + 1, 2));
  Receive(IpmxSr(kSsrc, 3));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.ssrc_mismatch, 2u);
  EXPECT_EQ(info.received, 1u);
  EXPECT_EQ(info.ssrc, kSsrc);
  /* a source update keeps the ops ssrc as the reference */
  ut_srrx_queue_put(ctx_);
  ASSERT_EQ(ut_srrx_queue_get(ctx_), 0);
  Receive(IpmxSr(kSsrc + 1, 4));
  EXPECT_EQ(Get(kPortP, -EBUSY).ssrc_mismatch, 1u);
}

/* arrival_ns is the NIC RX timestamp when the packet has one, else the PTP time at poll
 */
TEST_F(IpmxSrRxSessionTest, ArrivalTimeSource) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_srrx_set_ptp(ctx_, 5555);
  ut_srrx_set_hw_ts(ctx_, true, 4444);
  Receive(IpmxSr(kSsrc, 1));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_TRUE(info.arrival_hw);
  EXPECT_EQ(info.arrival_ns, 4444u);
  ut_srrx_set_hw_ts(ctx_, false, 0);
  Receive(IpmxSr(kSsrc, 2));
  info = Get();
  EXPECT_FALSE(info.arrival_hw);
  EXPECT_EQ(info.arrival_ns, 5555u);
}

/* an Info Block without a video Media Info Block is not "no info block", and its version
 * still counts */
TEST_F(IpmxSrRxSessionTest, InfoBlockWithoutVideoMibCountsVersion) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(IpmxSr(kSsrc, 1, VideoInfo())); /* version 7 */
  Receive(Sr(kSsrc, 2, InfoBlockWith({}, 8)));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.no_info_block, 0u);
  EXPECT_EQ(info.version_changes, 1u);
  EXPECT_TRUE(info.has_info);
  EXPECT_FALSE(info.has_video_mib);
}

/* the session stats dump has one IPMX SR line per port with the counters */
TEST_F(IpmxSrRxSessionTest, StatLogPerPort) {
  ASSERT_NO_FATAL_FAILURE(Attach(2));
  Receive(IpmxSr(kSsrc, 1), kPortR);
  Receive(Bytes{0x40, 0, 0, 0}, kPortR);
  std::string log = ut_session::capture_stderr([&] { ut_srrx_stat_log(ctx_); });
  EXPECT_NE(log.find("IPMX SR P total received 0 malformed 0"), std::string::npos) << log;
  EXPECT_NE(
      log.find("IPMX SR R total received 1 malformed 1 info malformed 0 no info block 0 "
               "ssrc mismatch 0 unknown mib 0 version changes 0"),
      std::string::npos)
      << log;
}

/* a defective Info Block counts in info_malformed, not in malformed or no_info_block;
 * only a valid block header counts a version change */
TEST_F(IpmxSrRxSessionTest, InfoMalformedCounted) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  Receive(IpmxSr(kSsrc, 1, VideoInfo())); /* version 7 */
  Bytes header_defect = SrInfoLen(IPMX_INFO_BLOCK_LEN / 4);
  header_defect[28 + 4] = 9; /* a version the header defect must not report */
  Receive(header_defect);
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.received, 2u);
  EXPECT_EQ(info.info_malformed, 1u);
  EXPECT_EQ(info.malformed, 0u);
  EXPECT_EQ(info.no_info_block, 0u);
  EXPECT_EQ(info.version_changes, 0u);
  EXPECT_FALSE(info.has_info);
  Bytes short_mib = VideoMib();
  short_mib.resize(88);
  short_mib[3] = 88 / 4 - 1;
  Receive(Sr(kSsrc, 3, InfoBlockWith({short_mib}, 8)));
  info = Get();
  EXPECT_EQ(info.info_malformed, 2u);
  EXPECT_EQ(info.version_changes, 1u);
  EXPECT_TRUE(info.has_info);
  EXPECT_FALSE(info.has_video_mib);
}

/* with MTL_FLAG_RX_USE_CNI the media flow is on CNI, so the SR flow is too, also with the
 * CNI thread (the media ring already keeps that thread polling) */
TEST_F(IpmxSrRxSessionTest, RxUseCniInThreadModeUsesCniQueue) {
  ASSERT_NO_FATAL_FAILURE(Create());
  ut_srrx_set_cni(ctx_, kPortP, true, false);
  ut_srrx_set_media_force_cni(ctx_, true);
  ASSERT_EQ(ut_srrx_attach(ctx_), 0);
  struct ut_srrx_flow flow;
  ut_srrx_flow(ctx_, kPortP, &flow);
  EXPECT_TRUE(flow.force_cni);
}

/* version_changes counts block version changes between Info Blocks, an SR without one
 * between them does not reset it */
TEST_F(IpmxSrRxSessionTest, VersionChangesCounted) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  struct ipmx_video_info v = VideoInfo();
  v.version = 3;
  Receive(IpmxSr(kSsrc, 1, v));
  Receive(IpmxSr(kSsrc, 2, v));
  EXPECT_EQ(Get().version_changes, 0u);
  Receive(Sr(kSsrc, 3, {}));
  Receive(IpmxSr(kSsrc, 4, v));
  EXPECT_EQ(Get().version_changes, 0u);
  v.version = 4;
  Receive(IpmxSr(kSsrc, 5, v));
  struct ipmx_rx_sr_info info = Get();
  EXPECT_EQ(info.version_changes, 1u);
  EXPECT_EQ(info.info.version, 4);
}

/* ST 2022-7: each leg is its own RTP stream, so it keeps its own SRs and SSRC */
TEST_F(IpmxSrRxSessionTest, PortsIndependent) {
  ASSERT_NO_FATAL_FAILURE(Attach(2));
  ut_srrx_media(ctx_, kPortP, kSsrc);
  Receive(IpmxSr(kSsrc, 10), kPortR);
  Get(kPortP, -EBUSY);
  struct ipmx_rx_sr_info r = Get(kPortR);
  EXPECT_EQ(r.rtp_timestamp, 10u);
  Receive(IpmxSr(kSsrc + 1, 11), kPortR); /* no media seen on R yet */
  EXPECT_EQ(Get(kPortR).ssrc_mismatch, 0u);
}

/* detach puts every queue, drops what is queued and frees the state */
TEST_F(IpmxSrRxSessionTest, DetachPutsQueues) {
  ASSERT_NO_FATAL_FAILURE(Attach(2));
  Bytes pkt = IpmxSr();
  ASSERT_EQ(ut_srrx_push(ctx_, kPortR, pkt.data(), pkt.size()), 0);
  ut_srrx_detach(ctx_);
  EXPECT_EQ(ut_srrx_rxq_puts(ctx_), 2);
  EXPECT_FALSE(ut_srrx_has_rxq(ctx_, kPortP));
  EXPECT_FALSE(ut_srrx_has_rxq(ctx_, kPortR));
  EXPECT_FALSE(ut_srrx_has_state(ctx_));
  EXPECT_EQ(ut_srrx_mbufs_in_use(ctx_), 0u);
}

/* a source update re-creates the queues and starts the port over: a new sender */
TEST_F(IpmxSrRxSessionTest, SourceUpdateRestarts) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  ut_srrx_media(ctx_, kPortP, kSsrc);
  Receive(IpmxSr(kSsrc, 1));
  ut_srrx_queue_put(ctx_);
  EXPECT_FALSE(ut_srrx_has_rxq(ctx_, kPortP));
  ASSERT_EQ(ut_srrx_queue_get(ctx_), 0);
  EXPECT_TRUE(ut_srrx_has_rxq(ctx_, kPortP));
  EXPECT_EQ(Get(kPortP, -EBUSY).received, 0u);
  Receive(IpmxSr(kSsrc + 9, 2)); /* the old media SSRC is forgotten */
  EXPECT_EQ(Get().ssrc, kSsrc + 9);
}

/* the getter never returns a mix of two SRs while the tasklet writes */
TEST_F(IpmxSrRxSessionTest, SnapshotIsConsistent) {
  ASSERT_NO_FATAL_FAILURE(Attach());
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::thread reader([&] {
    struct ipmx_rx_sr_info info;
    while (!stop.load()) {
      if (ut_srrx_get(ctx_, kPortP, &info) != 0) continue;
      /* SrBody(): pkt_count = rtp + 1, octets = rtp + 2, width tracks it too */
      if (info.pkt_count != info.rtp_timestamp + 1 ||
          info.octet_count != info.rtp_timestamp + 2 ||
          info.info.width != (uint16_t)(info.rtp_timestamp & 0xffff) ||
          info.received != info.rtp_timestamp + 1)
        torn++;
    }
  });
  struct ipmx_video_info v = VideoInfo();
  for (uint32_t ts = 0; ts < 100000; ts++) {
    v.width = ts & 0xffff;
    Bytes pkt = Sr(kSsrc, ts, InfoBlock(v));
    int pushed = ut_srrx_push(ctx_, kPortP, pkt.data(), pkt.size());
    EXPECT_EQ(pushed, 0);
    int polled = ut_srrx_poll(ctx_, kPortP);
    EXPECT_EQ(polled, 1);
    if (pushed || polled != 1) break; /* the reader must still be joined */
  }
  stop = true;
  reader.join();
  EXPECT_EQ(torn.load(), 0);
}

}  // namespace
