/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/*
 * The IPMX SDP of an st20 TX session (VSF TR-10-1 section 10, TR-10-2 section 7):
 * ipmx_sdp_build() turns the session values into the SDP text, ipmx_sdp_tx_get() takes
 * them from the session, from the same ipmx_video_info the Sender Reports carry, and
 * keeps the o= session version. In the VSF sdp_validation_script.py the IPMX-specific
 * checks are the standards pass relaxing mediaclk when IPMX is declared, and the config
 * pass needing IPMX declared and htotal, vtotal and measuredpixclk nonzero (their values
 * are not checked), so their values are pinned here.
 *
 * Run: ./build_unit/tests/unit/UnitTest --gtest_filter='*IpmxSdp*'
 */

#include "ipmx/ipmx_sdp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ipmx/ipmx_sr_harness.h"

namespace {

constexpr uint8_t kSip[4] = {192, 168, 1, 10};
constexpr uint16_t kPort = 20000;
constexpr uint8_t kPt = 112;
constexpr uint64_t kTaiNs = 1700000000ull * 1000000000ull;
constexpr int kPortP = 0;
constexpr int kPortR = 1;

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

/* the lines starting with prefix, in order */
std::vector<std::string> With(const std::vector<std::string>& lines,
                              const std::string& prefix) {
  std::vector<std::string> found;
  for (const std::string& l : lines)
    if (!l.compare(0, prefix.size(), prefix)) found.push_back(l.substr(prefix.size()));
  return found;
}

/* the first line starting with prefix, without it; "" if none */
std::string First(const std::vector<std::string>& lines, const std::string& prefix) {
  std::vector<std::string> found = With(lines, prefix);
  EXPECT_FALSE(found.empty()) << "no " << prefix;
  return found.empty() ? "" : found[0];
}

/* "a=fmtp:<pt> k=v; flag; ..." as k to v, a flag to "" */
std::map<std::string, std::string> Fmtp(const std::string& value) {
  std::map<std::string, std::string> params;
  size_t pos = value.find(' ') + 1;

  while (pos <= value.size()) {
    size_t end = value.find("; ", pos);
    std::string p = value.substr(pos, end == std::string::npos ? end : end - pos);
    size_t eq = p.find('=');
    EXPECT_EQ(params.count(p.substr(0, eq)), 0u) << "repeated " << p;
    params[p.substr(0, eq)] = eq == std::string::npos ? "" : p.substr(eq + 1);
    if (end == std::string::npos) break;
    pos = end + 2;
  }
  return params;
}

/* ---- ipmx_sdp_build(): the text of given values ---- */

struct ipmx_sdp_media Tr10_1LikeMedia() {
  struct ipmx_sdp_media m = {};
  struct ipmx_video_info* info = &m.info;

  snprintf(m.name, sizeof(m.name), "IPMX video 1");
  m.payload_type = kPt;
  m.num_port = 1;
  memcpy(m.leg[0].sip, kSip, 4);
  const uint8_t dip[4] = {239, 20, 0, 1};
  memcpy(m.leg[0].dip, dip, 4);
  m.leg[0].udp_port = kPort;
  snprintf(info->ts_refclk, sizeof(info->ts_refclk), "localmac=02-00-00-00-00-01");
  snprintf(info->mediaclk, sizeof(info->mediaclk), "direct=0");
  snprintf(info->sampling, sizeof(info->sampling), "YCbCr-4:2:2");
  info->depth = 10;
  info->gpm = true;
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

const char kFmtp1080p59[] =
    "a=fmtp:112 sampling=YCbCr-4:2:2; width=1920; height=1080; "
    "exactframerate=60000/1001; depth=10; TCS=SDR; colorimetry=BT709; RANGE=NARROW; "
    "PM=2110GPM; SSN=ST2110-20:2017; TP=2110TPN; IPMX; measuredpixclk=124291708; "
    "vtotal=1080; htotal=1920\r\n";

std::string Build(const struct ipmx_sdp_media& m, uint64_t id = kTaiNs,
                  uint64_t version = 1) {
  char buf[IPMX_SDP_MAX];
  int len = ipmx_sdp_build(&m, id, version, buf, sizeof(buf));
  EXPECT_GT(len, 0);
  if (len <= 0) return "";
  EXPECT_EQ((size_t)len, strlen(buf));
  return std::string(buf, len);
}

/* the TR-10-1 section 10.2 example shape, as the VSF writer orders a media section */
TEST(IpmxSdpBuildTest, SinglePortGolden) {
  const std::string expect =
      std::string(
          "v=0\r\n"
          "o=- 1700000000000000000 1 IN IP4 192.168.1.10\r\n"
          "s=IPMX video 1\r\n"
          "t=0 0\r\n"
          "m=video 20000 RTP/AVP 112\r\n"
          "c=IN IP4 239.20.0.1/64\r\n"
          "a=source-filter: incl IN IP4 239.20.0.1 192.168.1.10\r\n"
          "a=rtpmap:112 raw/90000\r\n") +
      kFmtp1080p59 +
      "a=ts-refclk:localmac=02-00-00-00-00-01\r\n"
      "a=mediaclk:direct=0\r\n";
  EXPECT_EQ(Build(Tr10_1LikeMedia()), expect);
}

/* ST 2022-7 (ST 2110-10 section 8.3, RFC 7104): one SDP, a media section per leg with its
 * own addresses, the same stream values and ts-refclk */
TEST(IpmxSdpBuildTest, RedundantGolden) {
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  const uint8_t sip_r[4] = {192, 168, 2, 10}, dip_r[4] = {192, 168, 2, 20};
  m.num_port = 2;
  memcpy(m.leg[1].sip, sip_r, 4);
  memcpy(m.leg[1].dip, dip_r, 4);
  m.leg[1].udp_port = 20002;

  const std::string media_end = std::string("a=rtpmap:112 raw/90000\r\n") + kFmtp1080p59 +
                                "a=ts-refclk:localmac=02-00-00-00-00-01\r\n"
                                "a=mediaclk:direct=0\r\n";
  const std::string expect =
      "v=0\r\n"
      "o=- 7 9 IN IP4 192.168.1.10\r\n"
      "s=IPMX video 1\r\n"
      "t=0 0\r\n"
      "a=group:DUP primary secondary\r\n"
      "m=video 20000 RTP/AVP 112\r\n"
      "c=IN IP4 239.20.0.1/64\r\n"
      "a=source-filter: incl IN IP4 239.20.0.1 192.168.1.10\r\n" +
      media_end +
      "a=mid:primary\r\n"
      "m=video 20002 RTP/AVP 112\r\n"
      "c=IN IP4 192.168.2.20\r\n"
      "a=source-filter: incl IN IP4 192.168.2.20 192.168.2.10\r\n" +
      media_end + "a=mid:secondary\r\n";
  EXPECT_EQ(Build(m, 7, 9), expect);
}

/* -ENOSPC unless the NUL fits too, and then nothing half written */
TEST(IpmxSdpBuildTest, ShortBufferIsEnospcAndEmpty) {
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  const std::string sdp = Build(m);
  std::vector<char> buf(sdp.size() + 1, 'x');

  for (size_t len : {(size_t)1, (size_t)10, sdp.size()}) {
    buf.assign(buf.size(), 'x');
    EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf.data(), len), -ENOSPC) << len;
    EXPECT_EQ(buf[0], '\0') << len;
  }
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf.data(), buf.size()), (int)sdp.size());
  EXPECT_EQ(std::string(buf.data()), sdp);
}

/* RFC 8866 section 5.3: s= is never empty; a name cannot break the line structure */
TEST(IpmxSdpBuildTest, SessionNameEmptyOrWithControlChars) {
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  m.name[0] = '\0';
  EXPECT_EQ(With(Lines(Build(m)), "s="), std::vector<std::string>{"-"});
  snprintf(m.name, sizeof(m.name), "a\r\nb\tc");
  EXPECT_EQ(With(Lines(Build(m)), "s="), std::vector<std::string>{"a__b_c"});
}

/* the VSF parser decodes s= as UTF-8: bytes from 0x7f up become '_', no invalid text */
TEST(IpmxSdpBuildTest, SessionNameNonAscii) {
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  snprintf(m.name, sizeof(m.name),
           "cam\xc3\xa9\xff\x7f"
           "1");
  EXPECT_EQ(With(Lines(Build(m)), "s="), std::vector<std::string>{"cam____1"});
}

/* the fmtp has no interlace, segmented or PAR parameter: such info is refused, not
 * signalled as progressive 1:1 */
TEST(IpmxSdpBuildTest, InterlacedSegmentedOrParRefused) {
  char buf[IPMX_SDP_MAX];
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  m.info.interlaced = true;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
  m = Tr10_1LikeMedia();
  m.info.segmented = true;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
  m = Tr10_1LikeMedia();
  m.info.par_w = 4;
  m.info.par_h = 3;
  EXPECT_EQ(ipmx_sdp_build(&m, kTaiNs, 1, buf, sizeof(buf)), -ENOTSUP);
}

/* ST 2110-20 section 7.2: exactframerate is an integer when it is one */
TEST(IpmxSdpBuildTest, IntegerFrameRate) {
  struct ipmx_sdp_media m = Tr10_1LikeMedia();
  m.info.rate_num = 50;
  m.info.rate_den = 1;
  auto fmtp = Fmtp(First(Lines(Build(m)), "a=fmtp:"));
  EXPECT_EQ(fmtp["exactframerate"], "50");
}

/* the largest values every field can hold, full strings without a NUL, two legs: still
 * within IPMX_SDP_MAX, and no string read past its field */
TEST(IpmxSdpBuildTest, LargestFitsIpmxSdpMax) {
  struct ipmx_sdp_media m = {};
  struct ipmx_video_info* info = &m.info;

  memset(m.name, 'n', sizeof(m.name) - 1);
  m.payload_type = 127;
  m.wide = true;
  m.num_port = 2;
  for (int i = 0; i < 2; i++) {
    memset(m.leg[i].sip, 255, 4);
    memset(m.leg[i].dip, 239, 4);
    m.leg[i].udp_port = 65535;
  }
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

  const std::string sdp = Build(m, UINT64_MAX, UINT64_MAX);
  EXPECT_LT(sdp.size(), (size_t)IPMX_SDP_MAX);
  auto lines = Lines(sdp);
  auto fmtp = Fmtp(First(lines, "a=fmtp:"));
  EXPECT_EQ(fmtp["sampling"], std::string(16, 's'));
  EXPECT_EQ(fmtp["colorimetry"], std::string(20, 'c'));
  EXPECT_EQ(First(lines, "a=ts-refclk:"), std::string(64, 'r'));
  EXPECT_EQ(First(lines, "a=mediaclk:"), std::string(12, 'm'));
  EXPECT_EQ(First(lines, "s="), std::string(31, 'n'));
}

/* ---- ipmx_sdp_tx_get(): the SDP of a session ---- */

class IpmxSdpSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(ut_sr_init(), 0);
    ctx_ = ut_sr_create(kSip, 1, kPort);
    ASSERT_NE(ctx_, nullptr);
    ut_sr_set_payload_type(ctx_, kPortP, kPt);
    ut_sr_set_name(ctx_, "cam1");
    ut_sr_set_now(ctx_, kTaiNs);
  }
  void TearDown() override {
    ut_sr_destroy(ctx_);
  }
  std::string Sdp() {
    char buf[IPMX_SDP_MAX];
    int len = ut_sr_get_sdp(ctx_, buf, sizeof(buf));
    EXPECT_GT(len, 0);
    if (len <= 0) return "";
    EXPECT_EQ((size_t)len, strlen(buf));
    return std::string(buf, len);
  }
  std::map<std::string, std::string> SdpFmtp() {
    auto fmtp = With(Lines(Sdp()), "a=fmtp:");
    EXPECT_EQ(fmtp.size(), 1u);
    return fmtp.empty() ? std::map<std::string, std::string>() : Fmtp(fmtp[0]);
  }
  /* "o=- <id> <version> IN IP4 <addr>" split */
  std::vector<std::string> Origin() {
    auto o = With(Lines(Sdp()), "o=");
    EXPECT_EQ(o.size(), 1u);
    std::vector<std::string> f;
    size_t pos = 0, end;
    std::string s = o.empty() ? "" : o[0] + " ";
    while ((end = s.find(' ', pos)) != std::string::npos) {
      f.push_back(s.substr(pos, end - pos));
      pos = end + 1;
    }
    EXPECT_EQ(f.size(), 6u);
    f.resize(6);
    return f;
  }
  std::string SessVersion() {
    return Origin()[2];
  }
  void Frame() {
    ut_sr_frame(ctx_, kPortP, kTaiNs + frames_ * 16683333ull, 1000 + frames_ * 1501,
                false);
    frames_++;
  }
  ut_sr_ctx* ctx_ = nullptr;
  int frames_ = 0;
};

/* RFC 8866 section 5.2: the origin is the session's P source address, the id the PTP
 * time at create; one media section for one port */
TEST_F(IpmxSdpSessionTest, SessionLevelLines) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  ut_sr_set_now(ctx_, kTaiNs + 5 * 1000000000ull);
  auto lines = Lines(Sdp());
  ASSERT_GE(lines.size(), 4u);
  EXPECT_EQ(lines[0], "v=0");
  EXPECT_EQ(lines[1], "o=- 1700000000000000000 1 IN IP4 192.168.1.10");
  EXPECT_EQ(lines[2], "s=cam1");
  EXPECT_EQ(lines[3], "t=0 0");
  EXPECT_TRUE(With(lines, "a=group:").empty());
  EXPECT_TRUE(With(lines, "a=mid:").empty());
  EXPECT_EQ(With(lines, "m=").size(), 1u);
}

/* a PTP time of 0 at create, as an app ptp_get_time_fn can give, still makes a nonzero
 * id: the VSF parser rejects o= with a zero id */
TEST_F(IpmxSdpSessionTest, SessionIdNeverZero) {
  ut_sr_set_now(ctx_, 0);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(Origin()[1], "1");
}

/* the payload type and destination of the wire header */
TEST_F(IpmxSdpSessionTest, MediaLineFromTheWireHeader) {
  ut_sr_set_payload_type(ctx_, kPortP, 98);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  auto lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "m="), std::vector<std::string>{"video 20000 RTP/AVP 98"});
  EXPECT_EQ(With(lines, "a=rtpmap:"), std::vector<std::string>{"98 raw/90000"});
  EXPECT_EQ(With(lines, "a=fmtp:98 ").size(), 1u);
}

/* RFC 8866 section 5.7: multicast IPv4 carries the TTL the session sends with (64),
 * unicast none; the source filter is there in both */
TEST_F(IpmxSdpSessionTest, MulticastHasTtlUnicastNone) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  auto lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "c="), std::vector<std::string>{"IN IP4 239.0.0.1/64"});
  EXPECT_EQ(With(lines, "a=source-filter: "),
            std::vector<std::string>{"incl IN IP4 239.0.0.1 192.168.1.10"});

  const uint8_t unicast[4] = {192, 168, 1, 20};
  ut_sr_set_dest(ctx_, kPortP, unicast, kPort);
  lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "c="), std::vector<std::string>{"IN IP4 192.168.1.20"});
  EXPECT_EQ(With(lines, "a=source-filter: "),
            std::vector<std::string>{"incl IN IP4 192.168.1.20 192.168.1.10"});
}

/* ST 2022-7: DUP group, each leg its own addresses and mid, the Info Block values and
 * ts-refclk the same on both */
TEST_F(IpmxSdpSessionTest, RedundantTwoMediaSections) {
  const uint8_t sip_r[4] = {192, 168, 2, 10}, dip_r[4] = {239, 0, 0, 2};
  ut_sr_set_redundant(ctx_, sip_r, dip_r, 20002);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  auto lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "a=group:"), std::vector<std::string>{"DUP primary secondary"});
  EXPECT_EQ(First(lines, "o="), "- 1700000000000000000 1 IN IP4 192.168.1.10");
  EXPECT_EQ(With(lines, "m="), (std::vector<std::string>{"video 20000 RTP/AVP 112",
                                                         "video 20002 RTP/AVP 112"}));
  EXPECT_EQ(With(lines, "c="),
            (std::vector<std::string>{"IN IP4 239.0.0.1/64", "IN IP4 239.0.0.2/64"}));
  EXPECT_EQ(With(lines, "a=source-filter: "),
            (std::vector<std::string>{"incl IN IP4 239.0.0.1 192.168.1.10",
                                      "incl IN IP4 239.0.0.2 192.168.2.10"}));
  EXPECT_EQ(With(lines, "a=mid:"), (std::vector<std::string>{"primary", "secondary"}));
  auto fmtp = With(lines, "a=fmtp:");
  ASSERT_EQ(fmtp.size(), 2u);
  EXPECT_EQ(fmtp[0], fmtp[1]);
  auto refclk = With(lines, "a=ts-refclk:");
  ASSERT_EQ(refclk.size(), 2u);
  EXPECT_EQ(refclk[0], "localmac=02-00-00-00-00-01");
  EXPECT_EQ(refclk[1], refclk[0]);
  EXPECT_EQ(With(lines, "a=mediaclk:"),
            (std::vector<std::string>{"direct=0", "direct=0"}));
  /* each media section ends with its mid */
  auto m1 = std::find(lines.begin(), lines.end(), "m=video 20002 RTP/AVP 112");
  ASSERT_NE(m1, lines.end());
  EXPECT_EQ(*(m1 - 1), "a=mid:primary");
  EXPECT_EQ(lines.back(), "a=mid:secondary");
}

/* TR-10-1 sections 10.1 and 10.2: the IPMX declaration and measuredpixclk, vtotal and
 * htotal with their non-baseband values; the VSF config check only wants them nonzero */
TEST_F(IpmxSdpSessionTest, IpmxParametersPresent) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  auto fmtp = SdpFmtp();
  ASSERT_EQ(fmtp.count("IPMX"), 1u);
  EXPECT_EQ(fmtp["IPMX"], "");
  EXPECT_EQ(fmtp["measuredpixclk"], "124291708");
  EXPECT_EQ(fmtp["vtotal"], "1080");
  EXPECT_EQ(fmtp["htotal"], "1920");
  EXPECT_EQ(fmtp["SSN"], "ST2110-20:2017");
}

/* ST 2110-21 section 9: TP from the session pacing */
TEST_F(IpmxSdpSessionTest, PacingToTp) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SdpFmtp()["TP"], "2110TPN");
  ut_sr_detach(ctx_);
  ut_sr_set_pacing(ctx_, ST21_PACING_WIDE);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SdpFmtp()["TP"], "2110TPW");
}

/* ST 2110-20 section 7.3: PM from the packing; GPM_SL is general packing */
struct PackingCase {
  enum st20_packing packing;
  const char* pm;
};

class IpmxSdpPackingTest : public IpmxSdpSessionTest,
                           public ::testing::WithParamInterface<PackingCase> {};

TEST_P(IpmxSdpPackingTest, PackingToPm) {
  ASSERT_EQ(ut_sr_set_video(ctx_, ST20_FMT_YUV_422_10BIT, 1920, 1080, GetParam().packing,
                            ST_FPS_P59_94),
            0);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SdpFmtp()["PM"], GetParam().pm);
}

INSTANTIATE_TEST_SUITE_P(Packings, IpmxSdpPackingTest,
                         ::testing::Values(PackingCase{ST20_PACKING_BPM, "2110BPM"},
                                           PackingCase{ST20_PACKING_GPM, "2110GPM"},
                                           PackingCase{ST20_PACKING_GPM_SL, "2110GPM"}));

/* field by field against ipmx_st20_tx_get_info() and against the expected value, over
 * the D6 formats, sizes and rates */
struct FieldsCase {
  enum st20_fmt fmt;
  const char* sampling;
  int depth;
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  const char* rate;
  uint64_t pixclk;
};

class IpmxSdpFieldsTest : public IpmxSdpSessionTest,
                          public ::testing::WithParamInterface<FieldsCase> {};

TEST_P(IpmxSdpFieldsTest, FmtpMatchesTheInfo) {
  const FieldsCase& c = GetParam();
  ASSERT_EQ(ut_sr_set_video(ctx_, c.fmt, c.width, c.height, ST20_PACKING_BPM, c.fps), 0);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  auto lines = Lines(Sdp());
  auto fmtp_line = With(lines, "a=fmtp:");
  ASSERT_EQ(fmtp_line.size(), 1u);
  auto fmtp = Fmtp(fmtp_line[0]);

  EXPECT_EQ(fmtp["sampling"], c.sampling);
  EXPECT_EQ(fmtp["sampling"], info.sampling);
  EXPECT_EQ(fmtp["depth"], std::to_string(c.depth));
  EXPECT_EQ(fmtp["depth"], std::to_string(info.depth));
  EXPECT_EQ(fmtp["width"], std::to_string(c.width));
  EXPECT_EQ(fmtp["width"], std::to_string(info.width));
  EXPECT_EQ(fmtp["height"], std::to_string(c.height));
  EXPECT_EQ(fmtp["height"], std::to_string(info.height));
  EXPECT_EQ(fmtp["exactframerate"], c.rate);
  EXPECT_EQ(fmtp["TCS"], info.tcs);
  EXPECT_EQ(fmtp["TCS"], "SDR");
  EXPECT_EQ(fmtp["colorimetry"], info.colorimetry);
  EXPECT_EQ(fmtp["colorimetry"], "BT709");
  EXPECT_EQ(fmtp["RANGE"], info.range);
  EXPECT_EQ(fmtp["RANGE"], "NARROW");
  EXPECT_EQ(fmtp["PM"], "2110BPM");
  EXPECT_EQ(fmtp["TP"], "2110TPN");
  EXPECT_EQ(fmtp["measuredpixclk"], std::to_string(c.pixclk));
  EXPECT_EQ(fmtp["measuredpixclk"], std::to_string(info.pixclk));
  EXPECT_EQ(fmtp["htotal"], std::to_string(info.htotal));
  EXPECT_EQ(fmtp["vtotal"], std::to_string(info.vtotal));
  EXPECT_EQ(fmtp.size(), 15u);
  EXPECT_EQ(With(lines, "a=ts-refclk:"), std::vector<std::string>{info.ts_refclk});
  EXPECT_EQ(With(lines, "a=mediaclk:"), std::vector<std::string>{info.mediaclk});
}

std::vector<FieldsCase> D6Cases() {
  struct Format {
    enum st20_fmt fmt;
    const char* sampling;
    int depth;
  };
  struct Rate {
    enum st_fps fps;
    const char* rate;
    uint64_t pixclk_1080; /* round(1920 x 1080 x rate) */
    uint64_t pixclk_2160;
  };
  const Format formats[] = {{ST20_FMT_RGB_8BIT, "RGB", 8},
                            {ST20_FMT_RGB_10BIT, "RGB", 10},
                            {ST20_FMT_YUV_422_10BIT, "YCbCr-4:2:2", 10}};
  const Rate rates[] = {{ST_FPS_P50, "50", 103680000, 414720000},
                        {ST_FPS_P59_94, "60000/1001", 124291708, 497166833},
                        {ST_FPS_P60, "60", 124416000, 497664000}};
  std::vector<FieldsCase> cases;

  for (const Format& f : formats)
    for (const Rate& r : rates) {
      cases.push_back(
          {f.fmt, f.sampling, f.depth, 1920, 1080, r.fps, r.rate, r.pixclk_1080});
      cases.push_back(
          {f.fmt, f.sampling, f.depth, 3840, 2160, r.fps, r.rate, r.pixclk_2160});
    }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(D6, IpmxSdpFieldsTest, ::testing::ValuesIn(D6Cases()));

/* ---- the o= session version: RFC 8866 section 5.2, up on a content change ---- */

/* no change, no new version, and the id stays */
TEST_F(IpmxSdpSessionTest, VersionSameWithoutChange) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  const std::string first = Sdp();
  Frame();
  Frame();
  EXPECT_EQ(Sdp(), first);
  EXPECT_EQ(SessVersion(), "1");
}

/* a new block version is new content, the SDP text aside */
TEST_F(IpmxSdpSessionTest, VersionUpOnBlockVersion) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SessVersion(), "1");
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 3), 0);
  EXPECT_EQ(SessVersion(), "2");
  EXPECT_EQ(SessVersion(), "2");
  EXPECT_EQ(Origin()[1], "1700000000000000000");
}

/* the SDP follows the ts-refclk on the wire: a staged setter string shows, with the new
 * version, once the next Sender Report carries it */
TEST_F(IpmxSdpSessionTest, VersionUpOnTsRefclkOnTheWire) {
  const char* ptp = "ptp=IEEE1588-2008:00-11-22-FF-FE-33-44-55:0";
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  Frame();
  EXPECT_EQ(SessVersion(), "1");
  ASSERT_EQ(ut_sr_set_ts_refclk(ctx_, ptp), 0);
  auto lines = Lines(Sdp());
  EXPECT_EQ(First(lines, "a=ts-refclk:"), "localmac=02-00-00-00-00-01");
  EXPECT_EQ(SessVersion(), "1");
  Frame();
  lines = Lines(Sdp());
  EXPECT_EQ(First(lines, "a=ts-refclk:"), ptp);
  EXPECT_EQ(SessVersion(), "2");
  struct ipmx_video_info info;
  ASSERT_EQ(ut_sr_get_info(ctx_, &info), 0);
  EXPECT_EQ(info.version, 1); /* the library bump of the Info Block */
}

/* st20_tx_update_destination() rewrites the header template, the SDP follows */
TEST_F(IpmxSdpSessionTest, VersionUpOnDestinationUpdate) {
  const uint8_t dip[4] = {239, 1, 1, 1};
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SessVersion(), "1");
  ut_sr_set_dest(ctx_, kPortP, dip, 20100);
  auto lines = Lines(Sdp());
  EXPECT_EQ(With(lines, "m="), std::vector<std::string>{"video 20100 RTP/AVP 112"});
  EXPECT_EQ(With(lines, "c="), std::vector<std::string>{"IN IP4 239.1.1.1/64"});
  EXPECT_EQ(SessVersion(), "2");
}

/* the version counts the SDPs that differ, not the changes in between */
TEST_F(IpmxSdpSessionTest, VersionUpOnceForChangesBetweenCalls) {
  const uint8_t dip[4] = {239, 1, 1, 1};
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(SessVersion(), "1");
  ASSERT_EQ(ut_sr_set_block_version(ctx_, 3), 0);
  ut_sr_set_dest(ctx_, kPortP, dip, 20100);
  EXPECT_EQ(SessVersion(), "2");
}

/* a short buffer does not count as a change either */
TEST_F(IpmxSdpSessionTest, ShortBufferEnospc) {
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  char buf[16];
  memset(buf, 'x', sizeof(buf));
  EXPECT_EQ(ut_sr_get_sdp(ctx_, buf, sizeof(buf)), -ENOSPC);
  EXPECT_EQ(buf[0], '\0');
  EXPECT_EQ(SessVersion(), "1");
}

/* -EIO without IPMX on the session, -EINVAL for no buffer */
TEST_F(IpmxSdpSessionTest, InvalidArgs) {
  char buf[IPMX_SDP_MAX];
  EXPECT_EQ(ut_sr_get_sdp(ctx_, buf, sizeof(buf)), -EIO);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  EXPECT_EQ(ut_sr_get_sdp(ctx_, nullptr, sizeof(buf)), -EINVAL);
  EXPECT_EQ(ut_sr_get_sdp(ctx_, buf, 0), -EINVAL);
}

/* the public getters check the handle type; st20p reaches the transport session */
TEST_F(IpmxSdpSessionTest, PublicGetters) {
  char buf[IPMX_SDP_MAX];
  EXPECT_EQ(ut_sr_api_get_sdp(ctx_, false, true, buf, sizeof(buf)), -EIO);
  ASSERT_EQ(ut_sr_attach(ctx_), 0);
  const std::string sdp = Sdp();
  EXPECT_EQ(ut_sr_api_get_sdp(ctx_, false, true, buf, sizeof(buf)), (int)sdp.size());
  EXPECT_EQ(std::string(buf), sdp);
  EXPECT_EQ(ut_sr_api_get_sdp(ctx_, true, true, buf, sizeof(buf)), (int)sdp.size());
  EXPECT_EQ(std::string(buf), sdp);
  EXPECT_EQ(ut_sr_api_get_sdp(ctx_, false, false, buf, sizeof(buf)), -EIO);
  EXPECT_EQ(ut_sr_api_get_sdp(ctx_, true, false, buf, sizeof(buf)), -EIO);
}

}  // namespace
