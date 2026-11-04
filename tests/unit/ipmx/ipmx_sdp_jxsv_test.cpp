/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/*
 * The video/jxsv IPMX SDP of an st22 (JPEG XS) TX session: VSF TR-10-11 section 10,
 * which builds it as ST 2110-22 section 7 with the TR-10-1 section 10 additions, and
 * the RFC 9134 parameters TR-08 section 7 lists. The st20 SDP of ipmx_sdp_test.cpp
 * stays byte for byte; on st22 ipmx_sdp_build() changes the rtpmap to jxsv/90000, adds
 * b=AS after c= in each media section and writes the jxsv fmtp. Its values come from
 * the same ipmx_video_info (MIB 0x0003) and struct ipmx_jxs_info (MIB 0x0008) the
 * Sender Report carries, kept once in struct ipmx_sr_tx at create.
 *
 * b=AS is the ceiling in kbit/s of the maximum IP-layer rate: max packets per frame
 * ceil((boxes + framebuff_max_size) / pkt_len) x (IPv4 20 + UDP 8 + RTP 12 + RFC 9134 4
 * + pkt_len) x frame rate. pkt_len 1280 is the payload tv_init_pkt() gives st22 with the
 * default UDP size (MTL_PKT_MAX_RTP_BYTES 1352 less the 16 octet RTP + RFC 9134 header,
 * rounded down to 128).
 *
 * The session cases drive lib/src/ipmx/ipmx_sdp.c and ipmx_sr.c through
 * ipmx_sr_harness.c. The goldens were checked offline with the VSF
 * sdp_validation_script.py (standards and config pass, no FAIL, no Warning).
 *
 * Groups:
 *   IpmxSdpJxsvKbpsTest     the b=AS formula and its rounding
 *   IpmxSdpJxsvPlevTest     ipmx_sdp_build(): Plev to level / sublevel, each one
 *                           ipmx_jxs_plev() picks
 *   IpmxSdpJxsvBuildTest    ipmx_sdp_build() of given values: T / K, TP, the -ENOTSUP
 *                           guards, -ENOSPC, IPMX_SDP_MAX
 *   IpmxSdpJxsvSessionTest  the SDP of an st22 session: the goldens, fmtp against the
 *                           Sender Report, sess-version bumps, the public getters
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxSdpJxsv*'
 */

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ipmx/ipmx_sdp.h"
#include "ipmx/ipmx_sr_harness.h"

namespace {

constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint16_t kPort = 20000;
constexpr uint8_t kPt = 112;
constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull;
constexpr uint32_t kPktLen = 1280;
constexpr int kPortP = 0;
constexpr uint32_t kInfoOffset = 28;
constexpr uint32_t kJxsMibOffset = kInfoOffset + 176;

constexpr size_t Bytes(uint32_t w, uint32_t h, uint32_t bpp) {
  return static_cast<size_t>(w) * h * bpp / 8;
}

uint16_t Be16(const uint8_t* p) {
  return (uint16_t)(p[0] << 8 | p[1]);
}

std::vector<std::string> Lines(const std::string& sdp) {
  std::vector<std::string> lines;
  size_t pos = 0, end;

  while ((end = sdp.find("\r\n", pos)) != std::string::npos) {
    lines.push_back(sdp.substr(pos, end - pos));
    pos = end + 2;
  }
  EXPECT_EQ(pos, sdp.size()) << "text after the last CRLF";
  return lines;
}

std::vector<std::string> With(const std::vector<std::string>& lines,
                              const std::string& prefix) {
  std::vector<std::string> found;
  for (const std::string& l : lines)
    if (!l.compare(0, prefix.size(), prefix)) found.push_back(l.substr(prefix.size()));
  return found;
}

/* "<pt> k=v; flag; ..." as k to v in order, a flag to "" */
std::vector<std::pair<std::string, std::string>> FmtpList(const std::string& value) {
  std::vector<std::pair<std::string, std::string>> params;
  size_t pos = value.find(' ') + 1;

  while (pos <= value.size()) {
    size_t end = value.find("; ", pos);
    std::string p = value.substr(pos, end == std::string::npos ? end : end - pos);
    size_t eq = p.find('=');
    params.emplace_back(p.substr(0, eq), eq == std::string::npos ? "" : p.substr(eq + 1));
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return params;
}

std::map<std::string, std::string> Fmtp(const std::string& value) {
  std::map<std::string, std::string> params;
  for (auto& p : FmtpList(value)) params.insert(p);
  return params;
}

/* ---- b=AS ---- */

/* one full packet a frame: 1324 octets x 8 x 1000 fps is exactly 10592 kbit/s, no
 * rounding up of an exact value */
TEST(IpmxSdpJxsvKbpsTest, ExactValueNotRoundedUp) {
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(1280, 1280, 1000, 1), 10592u);
}

/* a fraction of a kbit/s rounds up, so b=AS never under-states the rate */
TEST(IpmxSdpJxsvKbpsTest, FractionRoundsUp) {
  /* 10592 bits x 1001 fps = 10602.592 kbit/s */
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(1280, 1280, 1001, 1), 10603u);
}

/* one byte past a packet is one more packet with its headers: the last packet counts
 * full, as the maximum */
TEST(IpmxSdpJxsvKbpsTest, PartialPacketCountsAsFull) {
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(1281, 1280, 1000, 1), 2u * 10592u);
}

/* the session sizes the goldens use, at pkt_len 1280 and 60 bytes of boxes:
 * 608, 811 and 2431 packets a frame */
TEST(IpmxSdpJxsvKbpsTest, GoldenSessionRates) {
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(60 + Bytes(1920, 1080, 3), kPktLen, 60000, 1001), 386011u);
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(60 + Bytes(1920, 1080, 4), kPktLen, 60000, 1001), 514892u);
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(60 + Bytes(3840, 2160, 3), kPktLen, 60, 1), 1544950u);
}

/* a frame past 4 GiB counts in 64 bits, not wrapped to a few packets */
TEST(IpmxSdpJxsvKbpsTest, FrameAbove4GiB) {
  const uint64_t frame_max = (1ull << 32) + 1280; /* 3355445 packets */
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(frame_max, 1280, 1, 1), 35540874u);
}

/* a rate past UINT32_MAX kbit/s is clamped to UINT32_MAX, not wrapped */
TEST(IpmxSdpJxsvKbpsTest, ClampedToUint32Max) {
  const uint64_t frame_max = 4000000000ull; /* 7944000000 kbit/s at 240 fps */
  EXPECT_EQ(ipmx_sdp_jxsv_kbps(frame_max, 1280, 240, 1), UINT32_MAX);
}

/* ---- ipmx_sdp_build() of given values ---- */

struct ipmx_sdp_media JxsvMedia() {
  struct ipmx_sdp_media m = {};
  struct ipmx_video_info* info = &m.info;

  snprintf(m.name, sizeof(m.name), "jxs1");
  m.payload_type = kPt;
  m.num_port = 1;
  memcpy(m.leg[0].sip, kSip, 4);
  const uint8_t dip[4] = {239, 0, 0, 1};
  memcpy(m.leg[0].dip, dip, 4);
  m.leg[0].udp_port = kPort;
  m.jxsv = true;
  m.jxs.transmode = true;
  m.jxs.packetmode = false;
  m.jxs.ppih = 0x4a40;
  m.jxs.plev = 0x1004;
  m.bitrate_kbps = 386011;
  snprintf(info->ts_refclk, sizeof(info->ts_refclk), "localmac=02-00-00-00-00-01");
  snprintf(info->mediaclk, sizeof(info->mediaclk), "direct=0");
  snprintf(info->sampling, sizeof(info->sampling), "RGB");
  info->depth = 8;
  info->par_w = 1;
  info->par_h = 1;
  snprintf(info->range, sizeof(info->range), "NARROW");
  snprintf(info->colorimetry, sizeof(info->colorimetry), "BT709");
  snprintf(info->tcs, sizeof(info->tcs), "SDR");
  info->width = 1920;
  info->height = 1080;
  info->rate_num = 60000;
  info->rate_den = 1001;
  info->pixclk = 124291708;
  info->htotal = 1920;
  info->vtotal = 1080;
  return m;
}

std::string Build(const struct ipmx_sdp_media& m) {
  char buf[IPMX_SDP_MAX];
  int len = ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf));
  EXPECT_GT(len, 0);
  if (len <= 0) return "";
  EXPECT_EQ((size_t)len, strlen(buf));
  return std::string(buf, len);
}

std::map<std::string, std::string> BuildFmtp(const struct ipmx_sdp_media& m) {
  auto fmtp = With(Lines(Build(m)), "a=fmtp:");
  EXPECT_EQ(fmtp.size(), 1u);
  return fmtp.empty() ? std::map<std::string, std::string>() : Fmtp(fmtp[0]);
}

/* the Plev high byte to the RFC 9134 level, the low byte to the sublevel, as the VSF
 * ipmx_jxsv_validate_pcap.py SDP_LEVEL_TO_PLEV_HI / SDP_SUBLEVEL_TO_PLEV_LO tables:
 * every level and sublevel ipmx_jxs_plev() picks */
struct PlevCase {
  uint16_t plev;
  const char* level;
  const char* sublevel;
};

class IpmxSdpJxsvPlevTest : public ::testing::TestWithParam<PlevCase> {};

TEST_P(IpmxSdpJxsvPlevTest, LevelAndSublevelFromPlev) {
  struct ipmx_sdp_media m = JxsvMedia();
  m.jxs.plev = GetParam().plev;
  auto fmtp = BuildFmtp(m);
  EXPECT_EQ(fmtp["level"], GetParam().level);
  EXPECT_EQ(fmtp["sublevel"], GetParam().sublevel);
}

INSTANTIATE_TEST_SUITE_P(Plev, IpmxSdpJxsvPlevTest,
                         ::testing::Values(PlevCase{0x0404, "1k-1", "Sublev3bpp"},
                                           PlevCase{0x0406, "1k-1", "Sublev4bpp"},
                                           PlevCase{0x1004, "2k-1", "Sublev3bpp"},
                                           PlevCase{0x1006, "2k-1", "Sublev4bpp"},
                                           PlevCase{0x2404, "4k-2", "Sublev3bpp"},
                                           PlevCase{0x2406, "4k-2", "Sublev4bpp"},
                                           PlevCase{0x3404, "8k-2", "Sublev3bpp"},
                                           PlevCase{0x3406, "8k-2", "Sublev4bpp"}));

/* a profile, level, sublevel or fbblevel the IPMX create never picks has no SDP name
 * here: refused, buf untouched, rather than a wrong name */
TEST(IpmxSdpJxsvBuildTest, UnmappedProfileOrPlevRefused) {
  const uint16_t plevs[] = {
      0x2004, /* 4k-1 */
      0x1003, /* Sublev2bpp */
      0x1080, /* Full */
      0x1104, /* fbblevel bits 9..8 */
      0x1024, /* fbblevel bits 6..5 */
  };
  char buf[IPMX_SDP_MAX];

  for (uint16_t plev : plevs) {
    struct ipmx_sdp_media m = JxsvMedia();
    m.jxs.plev = plev;
    memset(buf, 'x', sizeof(buf));
    EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP) << plev;
    EXPECT_EQ(buf[0], 'x') << plev;
  }
  struct ipmx_sdp_media m = JxsvMedia();
  m.jxs.ppih = 0x3540; /* Main422.10 */
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
}

/* RFC 9134 section 7.1: packetmode is the K bit, transmode the T bit */
TEST(IpmxSdpJxsvBuildTest, PacketmodeAndTransmodeFromKAndT) {
  struct ipmx_sdp_media m = JxsvMedia();
  m.jxs.transmode = false;
  m.jxs.packetmode = true;
  auto fmtp = BuildFmtp(m);
  EXPECT_EQ(fmtp["packetmode"], "1");
  EXPECT_EQ(fmtp["transmode"], "0");
}

/* TR-08 section 9 Table 4: IPMX video senders are type W, whatever the session pacing
 * says; st20 keeps TP from the pacing */
TEST(IpmxSdpJxsvBuildTest, TpAlwaysWide) {
  struct ipmx_sdp_media m = JxsvMedia();
  m.wide = false;
  EXPECT_EQ(BuildFmtp(m)["TP"], "2110TPW");
}

/* the jxsv fmtp has no interlace, segmented or PAR parameter either */
TEST(IpmxSdpJxsvBuildTest, InterlacedSegmentedOrParRefused) {
  char buf[IPMX_SDP_MAX];
  struct ipmx_sdp_media m = JxsvMedia();
  m.info.interlaced = true;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
  m = JxsvMedia();
  m.info.segmented = true;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
  m = JxsvMedia();
  m.info.par_w = 4;
  m.info.par_h = 3;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
}

/* -ENOSPC unless the NUL fits too, buf then empty */
TEST(IpmxSdpJxsvBuildTest, ShortBufferIsEnospcAndEmpty) {
  struct ipmx_sdp_media m = JxsvMedia();
  const std::string sdp = Build(m);
  std::vector<char> buf(sdp.size() + 1, 'x');

  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf.data(), sdp.size()), -ENOSPC);
  EXPECT_EQ(buf[0], '\0');
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf.data(), buf.size()), (int)sdp.size());
}

/* the largest jxsv values over two legs still fit IPMX_SDP_MAX, which the public doc
 * promises for st22 too */
TEST(IpmxSdpJxsvBuildTest, LargestFitsIpmxSdpMax) {
  struct ipmx_sdp_media m = JxsvMedia();
  struct ipmx_video_info* info = &m.info;

  memset(m.name, 'n', sizeof(m.name) - 1);
  m.payload_type = 127;
  m.num_port = 2;
  for (int i = 0; i < 2; i++) {
    memset(m.leg[i].sip, 255, 4);
    memset(m.leg[i].dip, 239, 4);
    m.leg[i].udp_port = 65535;
  }
  m.jxs.packetmode = true;
  m.jxs.plev = 0x3406;
  m.bitrate_kbps = UINT32_MAX;
  memset(info->ts_refclk, 'r', sizeof(info->ts_refclk));
  memset(info->mediaclk, 'm', sizeof(info->mediaclk));
  memset(info->sampling, 's', sizeof(info->sampling));
  memset(info->range, 'g', sizeof(info->range));
  memset(info->colorimetry, 'c', sizeof(info->colorimetry));
  memset(info->tcs, 't', sizeof(info->tcs));
  info->depth = 255;
  info->width = 65535;
  info->height = 65535;
  info->rate_num = UINT32_MAX;
  info->rate_den = UINT32_MAX;
  info->pixclk = UINT64_MAX;
  info->htotal = 65535;
  info->vtotal = 65535;

  char buf[IPMX_SDP_MAX];
  int len = ipmx_sdp_build(&m, UINT64_MAX, UINT64_MAX, buf, sizeof(buf));
  EXPECT_GT(len, 0);
  EXPECT_LT(len, IPMX_SDP_MAX);
}

/* ---- the SDP of an st22 session ---- */

class IpmxSdpJxsvSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  /* an st22 IPMX session as st22_tx_create() leaves it: its placeholder st20 fmt, the
   * jxpl box Plev picked for the size, rate and codestream_max, not attached */
  void Create(uint32_t width, uint32_t height, enum st_fps fps, size_t codestream_max) {
    ctx_ = ut_sr_create(kSip, 1, kPort);
    ASSERT_NE(ctx_, nullptr);
    ut_sr_set_payload_type(ctx_, kPortP, kPt);
    ut_sr_set_name(ctx_, "jxs1");
    ut_sr_set_now(ctx_, kTaiNs);
    ASSERT_EQ(ut_sr_set_video(ctx_, ST20_FMT_YUV_422_10BIT, width, height,
                              ST20_PACKING_BPM, fps),
              0);
    ASSERT_EQ(ut_sr_set_st22(ctx_, codestream_max, kPktLen), 0);
  }
  void Create1080p3bpp() {
    Create(1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 3));
  }
  std::string Sdp() {
    char buf[IPMX_SDP_MAX];
    int len = ut_sr_get_sdp(ctx_, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    if (len <= 0) return "";
    EXPECT_EQ((size_t)len, strlen(buf));
    return std::string(buf, len);
  }
  std::string SessVersion() {
    auto o = With(Lines(Sdp()), "o=- ");
    EXPECT_EQ(o.size(), 1u);
    if (o.empty()) return "";
    size_t a = o[0].find(' ') + 1;
    return o[0].substr(a, o[0].find(' ', a) - a);
  }
  void Frame() {
    ut_sr_set_frame_size(ctx_, kPortP, 60 + Bytes(1920, 1080, 3));
    ut_sr_frame(ctx_, kPortP, kTaiNs + frames_ * 16683333ull, 1000 + frames_ * 1501,
                false);
    frames_++;
  }
  ut_sr_ctx* ctx_ = nullptr;
  int frames_ = 0;
};

std::string Golden(const std::string& bitrate, const std::string& fmtp) {
  return "v=0\r\n"
         "o=- 1700000000000000000 1 IN IP4 192.168.1.10\r\n"
         "s=jxs1\r\n"
         "t=0 0\r\n"
         "m=video 20000 RTP/AVP 112\r\n"
         "c=IN IP4 239.0.0.1/64\r\n"
         "b=AS:" +
         bitrate +
         "\r\n"
         "a=source-filter: incl IN IP4 239.0.0.1 192.168.1.10\r\n"
         "a=rtpmap:112 jxsv/90000\r\n"
         "a=fmtp:112 " +
         fmtp +
         "\r\n"
         "a=ts-refclk:localmac=02-00-00-00-00-01\r\n"
         "a=mediaclk:direct=0\r\n";
}

const char kFmtp1080p3bpp[] =
    "packetmode=0; transmode=1; profile=High444.12; level=2k-1; sublevel=Sublev3bpp; "
    "sampling=RGB; depth=8; width=1920; height=1080; exactframerate=60000/1001; "
    "colorimetry=BT709; TCS=SDR; RANGE=NARROW; SSN=ST2110-22:2022; TP=2110TPW; IPMX; "
    "measuredpixclk=124291708; vtotal=1080; htotal=1920";

/* 1080p59.94 RGB 8-bit at 3 bpp (the SVT ipmx_1080p.json stream): 2k-1 Sublev3bpp,
 * 608 packets of 1324 IP octets a frame, b=AS 386011 */
TEST_F(IpmxSdpJxsvSessionTest, Golden1080p59_94_3bpp) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Sdp(), Golden("386011", kFmtp1080p3bpp));
}

/* the same at 4 bpp (TR-08 Appendix B RGB rate): Sublev4bpp, 811 packets, 514892 */
TEST_F(IpmxSdpJxsvSessionTest, Golden1080p59_94_4bpp) {
  ASSERT_NO_FATAL_FAILURE(Create(1920, 1080, ST_FPS_P59_94, Bytes(1920, 1080, 4)));
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Sdp(), Golden("514892",
                          "packetmode=0; transmode=1; profile=High444.12; level=2k-1; "
                          "sublevel=Sublev4bpp; sampling=RGB; depth=8; width=1920; "
                          "height=1080; exactframerate=60000/1001; colorimetry=BT709; "
                          "TCS=SDR; RANGE=NARROW; SSN=ST2110-22:2022; TP=2110TPW; IPMX; "
                          "measuredpixclk=124291708; vtotal=1080; htotal=1920"));
}

/* 2160p60 at 3 bpp: level 4k-2 (the 4k-1 rate is too low), an integer frame rate,
 * 2431 packets, 1544950 */
TEST_F(IpmxSdpJxsvSessionTest, Golden2160p60_3bpp) {
  ASSERT_NO_FATAL_FAILURE(Create(3840, 2160, ST_FPS_P60, Bytes(3840, 2160, 3)));
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Sdp(), Golden("1544950",
                          "packetmode=0; transmode=1; profile=High444.12; level=4k-2; "
                          "sublevel=Sublev3bpp; sampling=RGB; depth=8; width=3840; "
                          "height=2160; exactframerate=60; colorimetry=BT709; TCS=SDR; "
                          "RANGE=NARROW; SSN=ST2110-22:2022; TP=2110TPW; IPMX; "
                          "measuredpixclk=497664000; vtotal=2160; htotal=3840"));
}

/* ST 2022-7 (ST 2110-10 section 8.3): DUP group and mids as on st20, each leg its own
 * c= and source-filter, the same b=AS and fmtp on both */
TEST_F(IpmxSdpJxsvSessionTest, GoldenRedundant) {
  const uint8_t sip_r[4] = {192, 168, 2, 10}, dip_r[4] = {239, 0, 0, 2};
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ut_sr_set_redundant(ctx_, sip_r, dip_r, 20002);
  ut_sr_set_payload_type(ctx_, 1, kPt);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);

  const std::string media_end = std::string("b=AS:386011\r\n");
  const std::string tail = std::string("a=rtpmap:112 jxsv/90000\r\na=fmtp:112 ") +
                           kFmtp1080p3bpp +
                           "\r\n"
                           "a=ts-refclk:localmac=02-00-00-00-00-01\r\n"
                           "a=mediaclk:direct=0\r\n";
  const std::string expect =
      "v=0\r\n"
      "o=- 1700000000000000000 1 IN IP4 192.168.1.10\r\n"
      "s=jxs1\r\n"
      "t=0 0\r\n"
      "a=group:DUP primary secondary\r\n"
      "m=video 20000 RTP/AVP 112\r\n"
      "c=IN IP4 239.0.0.1/64\r\n" +
      media_end + "a=source-filter: incl IN IP4 239.0.0.1 192.168.1.10\r\n" + tail +
      "a=mid:primary\r\n"
      "m=video 20002 RTP/AVP 112\r\n"
      "c=IN IP4 239.0.0.2/64\r\n" +
      media_end + "a=source-filter: incl IN IP4 239.0.0.2 192.168.2.10\r\n" + tail +
      "a=mid:secondary\r\n";
  EXPECT_EQ(Sdp(), expect);
}

/* TR-08 section 8.1.2 and TR-10-15 Part 1 section 9: the fmtp says what the Sender
 * Report says, MIB 0x0003 fields from get_info() and T / K / Ppih / Plev from the 0x0008
 * bytes of the SR, in the D-e order */
TEST_F(IpmxSdpJxsvSessionTest, FmtpMatchesTheSenderReport) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ut_sr_set_st22_tk(ctx_, true, false);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  uint8_t rtcp[UT_SR_PKT_MAX];
  ut_sr_build(ctx_, rtcp, kTaiNs, 1000);
  const uint8_t* mib = rtcp + kJxsMibOffset;
  ASSERT_EQ(Be16(mib), 0x0008);

  auto fmtp_line = With(Lines(Sdp()), "a=fmtp:");
  ASSERT_EQ(fmtp_line.size(), 1u);
  auto list = FmtpList(fmtp_line[0]);
  std::vector<std::string> keys;
  for (auto& p : list) keys.push_back(p.first);
  EXPECT_EQ(keys,
            (std::vector<std::string>{
                "packetmode", "transmode", "profile", "level", "sublevel", "sampling",
                "depth", "width", "height", "exactframerate", "colorimetry", "TCS",
                "RANGE", "SSN", "TP", "IPMX", "measuredpixclk", "vtotal", "htotal"}));
  auto fmtp = Fmtp(fmtp_line[0]);
  EXPECT_EQ(fmtp["transmode"], (mib[4] & 0x80) ? "1" : "0");
  EXPECT_EQ(fmtp["packetmode"], (mib[4] & 0x40) ? "1" : "0");
  EXPECT_EQ(Be16(mib + 6), 0x4a40);
  EXPECT_EQ(fmtp["profile"], "High444.12");
  EXPECT_EQ(Be16(mib + 8), ut_sr_st22_box_plev(ctx_));
  EXPECT_EQ(Be16(mib + 8), 0x1004);
  EXPECT_EQ(fmtp["level"], "2k-1");
  EXPECT_EQ(fmtp["sublevel"], "Sublev3bpp");
  EXPECT_EQ(fmtp["sampling"], info.sampling);
  EXPECT_EQ(fmtp["depth"], std::to_string(info.depth));
  EXPECT_EQ(fmtp["width"], std::to_string(info.width));
  EXPECT_EQ(fmtp["height"], std::to_string(info.height));
  EXPECT_EQ(fmtp["colorimetry"], info.colorimetry);
  EXPECT_EQ(fmtp["TCS"], info.tcs);
  EXPECT_EQ(fmtp["RANGE"], info.range);
  EXPECT_EQ(fmtp["measuredpixclk"], std::to_string(info.pixclk));
  EXPECT_EQ(fmtp["vtotal"], std::to_string(info.vtotal));
  EXPECT_EQ(fmtp["htotal"], std::to_string(info.htotal));
  EXPECT_EQ(fmtp.count("PM"), 0u);
  EXPECT_EQ(fmtp.count("fbblevel"), 0u);
}

/* T 0 / K 1 in the RTP header template reach both the SR and the SDP */
TEST_F(IpmxSdpJxsvSessionTest, TkFromTheRtpHeader) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ut_sr_set_st22_tk(ctx_, false, true);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  auto fmtp = Fmtp(With(Lines(Sdp()), "a=fmtp:").at(0));
  EXPECT_EQ(fmtp["transmode"], "0");
  EXPECT_EQ(fmtp["packetmode"], "1");
}

/* b=AS is fixed at create from framebuff_max_size: frames of other sizes leave it */
TEST_F(IpmxSdpJxsvSessionTest, BitrateFixedAtCreate) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  const std::string first = Sdp();
  ut_sr_set_frame_size(ctx_, kPortP, 1000);
  ut_sr_frame(ctx_, kPortP, kTaiNs, 1000, false);
  EXPECT_EQ(Sdp(), first);
  EXPECT_EQ(With(Lines(first), "b=AS:"), std::vector<std::string>{"386011"});
}

/* st22p forces NARROW pacing, but the SDP stays TPW */
TEST_F(IpmxSdpJxsvSessionTest, NarrowPacingStillTpw) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ut_sr_set_pacing(ctx_, ST21_PACING_NARROW);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Fmtp(With(Lines(Sdp()), "a=fmtp:").at(0))["TP"], "2110TPW");
}

/* RFC 8866 section 5.2, as on st20: the sess-version goes up on a new block version */
TEST_F(IpmxSdpJxsvSessionTest, VersionUpOnBlockVersion) {
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SessVersion(), "1");
  EXPECT_EQ(SessVersion(), "1");
  ASSERT_EQ(ut_sr_api_st22_set_block_version(ctx_, true, true, 4), 0);
  EXPECT_EQ(SessVersion(), "2");
}

/* a ts-refclk shows, with a new version, once the next SR carries it */
TEST_F(IpmxSdpJxsvSessionTest, VersionUpOnTsRefclkOnTheWire) {
  const char* ptp = "ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0";
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  Frame();
  EXPECT_EQ(SessVersion(), "1");
  ASSERT_EQ(ut_sr_api_st22_set_ts_refclk(ctx_, false, true, ptp), 0);
  EXPECT_EQ(SessVersion(), "1");
  Frame();
  EXPECT_EQ(With(Lines(Sdp()), "a=ts-refclk:"), std::vector<std::string>{ptp});
  EXPECT_EQ(SessVersion(), "2");
}

/* st22_tx_update_destination() rewrites the header template the SDP reads */
TEST_F(IpmxSdpJxsvSessionTest, VersionUpOnDestinationUpdate) {
  const uint8_t dip[4] = {239, 1, 1, 1};
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SessVersion(), "1");
  ut_sr_set_dest(ctx_, kPortP, dip, 20100);
  auto lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "m="), std::vector<std::string>{"video 20100 RTP/AVP 112"});
  EXPECT_EQ(With(lines, "c="), std::vector<std::string>{"IN IP4 239.1.1.1/64"});
  EXPECT_EQ(With(lines, "b=AS:"), std::vector<std::string>{"386011"});
  EXPECT_EQ(SessVersion(), "2");
}

/* ipmx_st22[p]_tx_get_sdp(): the session SDP through both handle kinds; -EIO without the
 * IPMX flag or on a handle of another type, -EINVAL without a buffer */
TEST_F(IpmxSdpJxsvSessionTest, PublicGetters) {
  char buf[IPMX_SDP_MAX];
  ASSERT_NO_FATAL_FAILURE(Create1080p3bpp());
  for (bool pipeline : {false, true})
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, true, buf, sizeof(buf)), -EIO);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  const std::string sdp = Sdp();
  for (bool pipeline : {false, true}) {
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, true, buf, sizeof(buf)),
              (int)sdp.size());
    EXPECT_EQ(std::string(buf), sdp);
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, false, buf, sizeof(buf)), -EIO);
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, true, nullptr, sizeof(buf)),
              -EINVAL);
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, true, buf, 0), -EINVAL);
    EXPECT_EQ(ut_sr_api_st22_get_sdp(ctx_, pipeline, true, buf, 16), -ENOSPC);
    EXPECT_EQ(buf[0], '\0');
  }
}

}  // namespace
