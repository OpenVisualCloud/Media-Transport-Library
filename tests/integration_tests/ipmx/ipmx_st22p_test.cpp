/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

/* st22p (JPEG XS, TR-10-11) with the IPMX flag on real ports.
 *
 * Create-time rejects: st22p_tx_create() / st22p_rx_create() refuse an IPMX config that
 * breaks a TR-10-11 rule. The rule by rule checks are
 * tests/unit/ipmx/ipmx_st22_validate_test.cpp; these pin that the real create path
 * applies them.
 *
 * Streaming: an IPMX TX to RX pair of GBRPLANAR8 (RGB 4:4:4 8-bit, D11), 1080p59.94
 * unless named otherwise.
 * The library checks every frame's JPEG XS picture header against the TR-08 section
 * 8.1.1 values it put in the boxes and drops a frame that does not match, so RX must
 * get every frame TX put, less those still in flight at stop.
 *   tx_rx_1080p         the test codec, which writes a synthetic header
 *                       (ipmx_jxs_test_header.cpp) with the SHA after it; RX checks
 *                       every SHA
 *   tx_rx_1080p_redundant  the same over ST 2022-7, TX on ports P and R, RX on ports
 *                       2 and 3: both legs deliver every frame with its SHA; skipped
 *                       with fewer than 4 ports
 *   tx_rx_svt_<rate>_<3|4>bpp  the real SVT-JPEG-XS plugin over the D11 matrix,
 *                       rate 1080p50, 1080p59_94, 1080p60, 2160p59_94, 2160p60; lossy,
 *                       no SHA, random TX frames (colour bars in dump runs). Skipped
 *                       unless the plugin is loaded (KAHAWAI_CFG_PATH) and
 *                       SVT_JXS_MTL_PLUGIN_CONFIG names an IPMX config whose
 *                       stream_level is the Plev of that case (0x1004, 0x1006, 0x2404,
 *                       0x2406): one KahawaiTest run per config. A case whose config
 *                       matches but whose encoder fails to open at its size fails.
 *                       IPMX_ST22P_DURATION_S, IPMX_ST22P_RX_FB_CNT and
 *                       IPMX_FRAME_DUMP_DIR set the stream length, the RX framebuffers
 *                       and a TX / RX frame dump (see ipmx_st22p_svt())
 *   tx_rx_svt_1080p59_94_3bpp_redundant  the SVT 1080p59.94 3 bpp point over ST 2022-7,
 *                       ports as tx_rx_1080p_redundant; skipped with fewer than 4 ports
 * Each streaming case also reads the RX Sender Reports of every leg after the stop
 * (ipmx_st22p_rx_get_sr_info()): none malformed, the Info Block with 0x0003 equal to
 * ipmx_st22p_tx_get_info() and 0x0008 with T 1, K 0, Ppih High444.12 and the case's
 * Plev (TR-10-11 section 12, TR-10-15 Part 1 section 9), and the TX video/jxsv SDP
 * (ipmx_st22p_tx_get_sdp(), taken after the stop) against those Sender Reports: the
 * fmtp against 0x0003, packetmode / transmode / profile / level / sublevel against T /
 * K / Ppih / Plev of 0x0008, ts-refclk, mediaclk, rtpmap, one b=AS after c= in each leg
 * with the exact value of the case's maximum codestream at the default payload size
 * (TR-10-11 section 10, TR-08 section 8.1.2, TP-1 sections 13.2 and 13.3). With
 * IPMX_SDP_DUMP_DIR set they write that SDP to <dir>/<test name>.sdp for the VSF SDP
 * scripts.
 *   tx_rx_1080p_sdp_changes  the SDP across a mid-stream ts-refclk change, a
 *                       mid-stream block version and a destination update:
 *                       sess-version 1, 2, 3, 4, only the changed lines differ, none
 *                       for the block version (RFC 8866 section 5.2)
 *   rx_get_sr_info_errors  the getter return codes of an RX with no sender: -EBUSY,
 *                       -EINVAL, and -EIO without the IPMX flag
 *   noipmx_tx_1080p_sr_none  a plain ST 2110-22 TX into an IPMX RX: frames as usual,
 *                       no SR (TP-1 section 14.3.3)
 *
 * Run: KahawaiTest ... --gtest_filter='Ipmx_st22p.*'
 */

#ifdef MTL_HAS_IPMX

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ipmx/ipmx_api.h"
#include "log.h"
#include "st22p_test.h"

#define IPMX_ST22P_UDP_PORT (16000)
#define IPMX_ST22P_PAYLOAD_TYPE (114)

/* ---- create-time rejects ---- */

static void ipmx_st22p_tx_ops(struct st_tests_context* ctx, struct st22p_tx_ops* ops) {
  memset(ops, 0, sizeof(*ops));
  ops->name = "ipmx_st22p_reject";
  ops->port.num_port = 1;
  memcpy(ops->port.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_P]);
  ops->port.udp_port[MTL_SESSION_PORT_P] = IPMX_ST22P_UDP_PORT;
  ops->port.payload_type = IPMX_ST22P_PAYLOAD_TYPE;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->input_fmt = ST_FRAME_FMT_GBRPLANAR8;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->codec = ST22_CODEC_JPEGXS;
  ops->device = ST_PLUGIN_DEVICE_TEST;
  ops->quality = ST22_QUALITY_MODE_QUALITY;
  ops->framebuff_cnt = 3;
  ops->codestream_size = 1920 * 1080 * 2 / 10;
  ops->flags = ST22P_TX_FLAG_IPMX;
}

static void ipmx_st22p_rx_ops(struct st_tests_context* ctx, struct st22p_rx_ops* ops) {
  memset(ops, 0, sizeof(*ops));
  ops->name = "ipmx_st22p_reject";
  ops->port.num_port = 1;
  memcpy(ops->port.ip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_P],
         MTL_IP_ADDR_LEN);
  snprintf(ops->port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
           ctx->para.port[MTL_PORT_R]);
  ops->port.udp_port[MTL_SESSION_PORT_P] = IPMX_ST22P_UDP_PORT;
  ops->port.payload_type = IPMX_ST22P_PAYLOAD_TYPE;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->output_fmt = ST_FRAME_FMT_GBRPLANAR8;
  ops->pack_type = ST22_PACK_CODESTREAM;
  ops->codec = ST22_CODEC_JPEGXS;
  ops->device = ST_PLUGIN_DEVICE_TEST;
  ops->framebuff_cnt = 3;
  ops->flags = ST22P_RX_FLAG_IPMX;
}

/* the RX getter of an IPMX session no sender feeds: -EBUSY, the counters zero, and the
 * argument checks; a session without the IPMX flag has no SR state, -EIO */
TEST(Ipmx_st22p, rx_get_sr_info_errors) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  struct st22p_rx_ops ops;
  struct ipmx_rx_sr_info sr;
  ipmx_st22p_rx_ops(ctx, &ops);
  ops.name = "ipmx_st22p_sr_errors";
  st22p_rx_handle handle = st22p_rx_create(ctx->handle, &ops);
  ASSERT_NE(handle, nullptr);
  memset(&sr, 0xee, sizeof(sr));
  EXPECT_EQ(ipmx_st22p_rx_get_sr_info(handle, MTL_SESSION_PORT_P, &sr), -EBUSY);
  EXPECT_EQ(sr.received, 0u);
  EXPECT_EQ(sr.malformed, 0u);
  EXPECT_EQ(ipmx_st22p_rx_get_sr_info(handle, MTL_SESSION_PORT_R, &sr), -EINVAL);
  EXPECT_EQ(ipmx_st22p_rx_get_sr_info(handle, MTL_SESSION_PORT_P, nullptr), -EINVAL);
  st22p_rx_free(handle);

  ops.flags = 0;
  handle = st22p_rx_create(ctx->handle, &ops);
  ASSERT_NE(handle, nullptr);
  EXPECT_EQ(ipmx_st22p_rx_get_sr_info(handle, MTL_SESSION_PORT_P, &sr), -EIO);
  st22p_rx_free(handle);
}

/* TR-10-11 section 7 / TR-10-2 section 7: an odd TX port fails st22p_tx_create() */
TEST(Ipmx_st22p, create_reject_tx_odd_port) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  struct st22p_tx_ops ops;
  ipmx_st22p_tx_ops(ctx, &ops);
  ops.port.udp_port[MTL_SESSION_PORT_P] = IPMX_ST22P_UDP_PORT + 1;
  st22p_tx_handle handle = st22p_tx_create(ctx->handle, &ops);
  EXPECT_EQ(handle, nullptr);
  if (handle) st22p_tx_free(handle);
}

/* RFC 9134 sections 3.4 and 4.1: the IPMX RX needs the boxes, DISABLE_BOXES fails
 * st22p_rx_create() */
TEST(Ipmx_st22p, create_reject_rx_disable_boxes) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  struct st22p_rx_ops ops;
  ipmx_st22p_rx_ops(ctx, &ops);
  ops.flags |= ST22P_RX_FLAG_DISABLE_BOXES;
  st22p_rx_handle handle = st22p_rx_create(ctx->handle, &ops);
  EXPECT_EQ(handle, nullptr);
  if (handle) st22p_rx_free(handle);
}

/* ---- streaming ---- */

/* what the streaming cases expect of the RX Sender Reports */
static struct ipmx_video_info ipmx_st22p_sr_tx_info;
static bool ipmx_st22p_sr_tx_info_valid;
static int ipmx_st22p_sr_ports;
static uint16_t ipmx_st22p_sr_plev;
static uint64_t ipmx_st22p_sr_version_changes;
static size_t ipmx_st22p_sr_codestream_max;
static std::string ipmx_st22p_sdp_tx;

static std::string ipmx_st22p_sdp(st22p_tx_handle handle) {
  char buf[IPMX_SDP_MAX];
  int len = ipmx_st22p_tx_get_sdp(handle, buf, sizeof(buf));
  EXPECT_GT(len, 0);
  return len > 0 ? std::string(buf, len) : "";
}

/* write sdp to $IPMX_SDP_DUMP_DIR/<test name><tag>.sdp, nothing when unset */
static void ipmx_st22p_sdp_dump(const std::string& sdp, const std::string& tag) {
  const char* dir = getenv("IPMX_SDP_DUMP_DIR");
  if (!dir || !dir[0]) return;

  std::string path = std::string(dir) + "/" +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name() +
                     tag + ".sdp";
  FILE* f = fopen(path.c_str(), "w");
  ASSERT_NE(f, nullptr) << path;
  EXPECT_EQ(fwrite(sdp.data(), 1, sdp.size(), f), sdp.size()) << path;
  fclose(f);
}

/* the TX info and SDP after the stream, so they reflect every Sender Report sent */
static void ipmx_st22p_sr_save_tx_info(st22p_tx_handle handle) {
  ipmx_st22p_sr_tx_info_valid =
      ipmx_st22p_tx_get_info(handle, &ipmx_st22p_sr_tx_info) == 0;
  EXPECT_TRUE(ipmx_st22p_sr_tx_info_valid);
  ipmx_st22p_sdp_tx = ipmx_st22p_sdp(handle);
  info("%s, sdp:\n%s", __func__, ipmx_st22p_sdp_tx.c_str());
  ipmx_st22p_sdp_dump(ipmx_st22p_sdp_tx, "");
}

static std::vector<std::string> ipmx_st22p_sdp_lines(const std::string& sdp) {
  std::vector<std::string> lines;
  size_t pos = 0, end;

  while ((end = sdp.find("\r\n", pos)) != std::string::npos) {
    lines.push_back(sdp.substr(pos, end - pos));
    pos = end + 2;
  }
  return lines;
}

/* the values of the lines starting with prefix, in order */
static std::vector<std::string> ipmx_st22p_sdp_values(const std::string& sdp,
                                                      const std::string& prefix) {
  std::vector<std::string> found;
  for (const std::string& l : ipmx_st22p_sdp_lines(sdp))
    if (!l.compare(0, prefix.size(), prefix)) found.push_back(l.substr(prefix.size()));
  return found;
}

/* "<pt> k=v; flag; ..." as k to v, a flag to "" */
static std::map<std::string, std::string> ipmx_st22p_fmtp(const std::string& value) {
  std::map<std::string, std::string> params;
  size_t pos = value.find(' ');

  while (pos != std::string::npos) {
    size_t end = value.find("; ", pos + 1);
    std::string p = value.substr(pos + 1, end == std::string::npos ? end : end - pos - 1);
    size_t eq = p.find('=');
    params[p.substr(0, eq)] = eq == std::string::npos ? "" : p.substr(eq + 1);
    pos = end == std::string::npos ? end : end + 1;
  }
  return params;
}

static std::string ipmx_st22p_str(const char* field, size_t size) {
  return std::string(field, strnlen(field, size));
}

/* the RFC 9134 names of the Ppih and Plev values of an IPMX stream (ISO/IEC 21122-2,
 * as the VSF ipmx_jxsv_validate_pcap.py tables), "" for any other */
static std::string ipmx_st22p_profile_name(uint16_t ppih) {
  return ppih == 0x4a40 ? "High444.12" : "";
}

static std::string ipmx_st22p_level_name(uint16_t plev) {
  static const std::map<int, std::string> names = {
      {0x04, "1k-1"}, {0x10, "2k-1"}, {0x24, "4k-2"}, {0x34, "8k-2"}};
  auto it = names.find(plev >> 8);
  return it == names.end() ? "" : it->second;
}

static std::string ipmx_st22p_sublevel_name(uint16_t plev) {
  static const std::map<int, std::string> names = {{0x04, "Sublev3bpp"},
                                                   {0x06, "Sublev4bpp"}};
  auto it = names.find(plev & 0xff);
  return it == names.end() ? "" : it->second;
}

/* TP-1 sections 13.2 and 13.3, TR-08 section 8.1.2: the SDP media section of the leg
 * says what the last Sender Report of that leg says */
static void ipmx_st22p_sdp_check_port(const struct ipmx_rx_sr_info* sr, int port) {
  const std::string& sdp = ipmx_st22p_sdp_tx;
  ASSERT_FALSE(sdp.empty());
  auto fmtps = ipmx_st22p_sdp_values(sdp, "a=fmtp:");
  ASSERT_EQ(fmtps.size(), (size_t)ipmx_st22p_sr_ports);
  auto fmtp = ipmx_st22p_fmtp(fmtps[port]);
  const struct ipmx_video_info* in = &sr->info;
  std::string rate = in->rate_den == 1 ? std::to_string(in->rate_num)
                                       : std::to_string(in->rate_num) + "/" +
                                             std::to_string(in->rate_den);

  EXPECT_EQ(fmtp["packetmode"], sr->jxs.packetmode ? "1" : "0");
  EXPECT_EQ(fmtp["transmode"], sr->jxs.transmode ? "1" : "0");
  EXPECT_EQ(fmtp["profile"], ipmx_st22p_profile_name(sr->jxs.ppih));
  EXPECT_EQ(fmtp["level"], ipmx_st22p_level_name(sr->jxs.plev));
  EXPECT_EQ(fmtp["sublevel"], ipmx_st22p_sublevel_name(sr->jxs.plev));
  EXPECT_FALSE(fmtp["level"].empty());
  EXPECT_FALSE(fmtp["sublevel"].empty());
  EXPECT_EQ(fmtp["sampling"], ipmx_st22p_str(in->sampling, sizeof(in->sampling)));
  EXPECT_EQ(fmtp["depth"], std::to_string(in->depth));
  EXPECT_EQ(fmtp["width"], std::to_string(in->width));
  EXPECT_EQ(fmtp["height"], std::to_string(in->height));
  EXPECT_EQ(fmtp["exactframerate"], rate);
  EXPECT_EQ(fmtp["colorimetry"],
            ipmx_st22p_str(in->colorimetry, sizeof(in->colorimetry)));
  EXPECT_EQ(fmtp["TCS"], ipmx_st22p_str(in->tcs, sizeof(in->tcs)));
  EXPECT_EQ(fmtp["RANGE"], ipmx_st22p_str(in->range, sizeof(in->range)));
  EXPECT_EQ(fmtp["measuredpixclk"], std::to_string(in->pixclk));
  EXPECT_EQ(fmtp["vtotal"], std::to_string(in->vtotal));
  EXPECT_EQ(fmtp["htotal"], std::to_string(in->htotal));
  EXPECT_EQ(fmtp["SSN"], "ST2110-22:2022");
  EXPECT_EQ(fmtp["TP"], "2110TPW");
  EXPECT_EQ(fmtp.count("IPMX"), 1u);
  EXPECT_EQ(fmtp.count("PM"), 0u);
  auto refclk = ipmx_st22p_sdp_values(sdp, "a=ts-refclk:");
  auto mediaclk = ipmx_st22p_sdp_values(sdp, "a=mediaclk:");
  ASSERT_EQ(refclk.size(), (size_t)ipmx_st22p_sr_ports);
  ASSERT_EQ(mediaclk.size(), (size_t)ipmx_st22p_sr_ports);
  EXPECT_EQ(refclk[port], ipmx_st22p_str(in->ts_refclk, sizeof(in->ts_refclk)));
  EXPECT_EQ(mediaclk[port], ipmx_st22p_str(in->mediaclk, sizeof(in->mediaclk)));

  /* the max frame, 60 B of boxes and the codestream maximum, in packets of the default
   * st22 payload (MTL_PKT_MAX_RTP_BYTES less the RTP + RFC 9134 header, down to 128),
   * each with its IPv4 20 + UDP 8 + RTP + RFC 9134 headers */
  const uint64_t rtp_hdr = sizeof(struct st22_rfc9134_rtp_hdr);
  const uint64_t pkt_len = (MTL_PKT_MAX_RTP_BYTES - rtp_hdr) / 128 * 128;
  uint64_t pkts = (60 + ipmx_st22p_sr_codestream_max + pkt_len - 1) / pkt_len;
  uint64_t div = (uint64_t)in->rate_den * 1000;
  uint64_t kbps =
      (pkts * (20 + 8 + rtp_hdr + pkt_len) * 8 * in->rate_num + div - 1) / div;
  auto bitrate = ipmx_st22p_sdp_values(sdp, "b=AS:");
  ASSERT_EQ(bitrate.size(), (size_t)ipmx_st22p_sr_ports);
  EXPECT_EQ(bitrate[port], std::to_string(kbps));
}

/* every media section: rtpmap jxsv/90000 and a nonzero b=AS right after c= */
static void ipmx_st22p_sdp_check_media() {
  std::vector<std::string> lines = ipmx_st22p_sdp_lines(ipmx_st22p_sdp_tx);
  int media = 0;

  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].compare(0, 2, "m=")) continue;
    media++;
    ASSERT_LT(i + 2, lines.size());
    EXPECT_EQ(lines[i + 1].compare(0, 2, "c="), 0) << lines[i + 1];
    EXPECT_EQ(lines[i + 2].compare(0, 5, "b=AS:"), 0) << lines[i + 2];
    EXPECT_GT(strtoul(lines[i + 2].c_str() + 5, NULL, 10), 0ul) << lines[i + 2];
  }
  EXPECT_EQ(media, ipmx_st22p_sr_ports);
  EXPECT_EQ(ipmx_st22p_sdp_values(ipmx_st22p_sdp_tx, "b=").size(),
            (size_t)ipmx_st22p_sr_ports);
  for (const std::string& r : ipmx_st22p_sdp_values(ipmx_st22p_sdp_tx, "a=rtpmap:"))
    EXPECT_EQ(r.substr(r.find(' ') + 1), "jxsv/90000");
}

/* the last SR of one leg against the TX: counters clean, the 0x0003 fields equal to the
 * TX info, the 0x0008 T / K / Ppih the TX sends and the Plev of its jxpl box */
static void ipmx_st22p_sr_check_port(st22p_rx_handle handle, enum mtl_session_port port) {
  struct ipmx_rx_sr_info sr;

  ASSERT_EQ(ipmx_st22p_rx_get_sr_info(handle, port, &sr), 0) << port;
  info("%s(%d), received %" PRIu64 " malformed %" PRIu64 " info malformed %" PRIu64
       " no info %" PRIu64 " ssrc mismatch %" PRIu64 " unknown mib %" PRIu64
       ", ssrc %u rtp %u cname %s version %u ppih 0x%04x plev 0x%04x\n",
       __func__, port, sr.received, sr.malformed, sr.info_malformed, sr.no_info_block,
       sr.ssrc_mismatch, sr.unknown_mib, sr.ssrc, sr.rtp_timestamp, sr.cname,
       sr.info.version, sr.jxs.ppih, sr.jxs.plev);
  /* one SR a frame over at least 10 s at 50 fps or more; st22p has no RX frame count
   * to compare */
  EXPECT_GT(sr.received, 300u);
  EXPECT_EQ(sr.malformed, 0u);
  EXPECT_EQ(sr.info_malformed, 0u);
  EXPECT_EQ(sr.no_info_block, 0u);
  EXPECT_EQ(sr.ssrc_mismatch, 0u);
  EXPECT_EQ(sr.unknown_mib, 0u);
  EXPECT_EQ(sr.version_changes, ipmx_st22p_sr_version_changes);
  EXPECT_GT(sr.pkt_count, 0u);
  EXPECT_GT(sr.octet_count, 0u);
  EXPECT_GT(strlen(sr.cname), 0u);

  ASSERT_TRUE(sr.has_info);
  ASSERT_TRUE(sr.has_video_mib);
  ASSERT_TRUE(ipmx_st22p_sr_tx_info_valid);
  const struct ipmx_video_info* tx = &ipmx_st22p_sr_tx_info;
  EXPECT_EQ(sr.info.version, tx->version);
  EXPECT_EQ(0, memcmp(sr.info.ts_refclk, tx->ts_refclk, sizeof(tx->ts_refclk)));
  EXPECT_EQ(0, memcmp(sr.info.mediaclk, tx->mediaclk, sizeof(tx->mediaclk)));
  EXPECT_EQ(0, memcmp(sr.info.sampling, tx->sampling, sizeof(tx->sampling)));
  EXPECT_EQ(sr.info.floating, tx->floating);
  EXPECT_EQ(sr.info.depth, tx->depth);
  EXPECT_EQ(sr.info.gpm, tx->gpm);
  EXPECT_EQ(sr.info.interlaced, tx->interlaced);
  EXPECT_EQ(sr.info.segmented, tx->segmented);
  EXPECT_EQ(sr.info.par_w, tx->par_w);
  EXPECT_EQ(sr.info.par_h, tx->par_h);
  EXPECT_EQ(0, memcmp(sr.info.range, tx->range, sizeof(tx->range)));
  EXPECT_EQ(0, memcmp(sr.info.colorimetry, tx->colorimetry, sizeof(tx->colorimetry)));
  EXPECT_EQ(0, memcmp(sr.info.tcs, tx->tcs, sizeof(tx->tcs)));
  EXPECT_EQ(sr.info.width, tx->width);
  EXPECT_EQ(sr.info.height, tx->height);
  EXPECT_EQ(sr.info.rate_num, tx->rate_num);
  EXPECT_EQ(sr.info.rate_den, tx->rate_den);
  EXPECT_EQ(sr.info.pixclk, tx->pixclk);
  EXPECT_EQ(sr.info.htotal, tx->htotal);
  EXPECT_EQ(sr.info.vtotal, tx->vtotal);

  ASSERT_TRUE(sr.has_jxs_mib);
  EXPECT_TRUE(sr.jxs.transmode);
  EXPECT_FALSE(sr.jxs.packetmode);
  EXPECT_EQ(sr.jxs.ppih, 0x4a40); /* High444.12 */
  EXPECT_EQ(sr.jxs.plev, ipmx_st22p_sr_plev);
  ipmx_st22p_sdp_check_port(&sr, port);
}

static void ipmx_st22p_sr_check(st22p_rx_handle handle) {
  ipmx_st22p_sdp_check_media();
  for (int p = 0; p < ipmx_st22p_sr_ports; p++)
    ipmx_st22p_sr_check_port(handle, (enum mtl_session_port)p);
}

/*
 * an IPMX digest para over ports legs whose RX checks the SRs of each leg, with Plev
 * plev expected. The TX drops a frame whose JPEG XS header does not match its jxpl box,
 * so with frames delivered the Plev of the codestream, which the case knows, is the one
 * of the box.
 */
static void ipmx_st22p_sr_para(struct st22p_rx_digest_test_para* para, int ports,
                               uint16_t plev, size_t codestream_max) {
  test_st22p_init_rx_digest_para(para);
  para->ipmx = true;
  para->redundant = ports > 1;
  para->tx_check = ipmx_st22p_sr_save_tx_info;
  para->rx_check = ipmx_st22p_sr_check;
  ipmx_st22p_sr_tx_info_valid = false;
  ipmx_st22p_sdp_tx.clear();
  ipmx_st22p_sr_version_changes = 0;
  ipmx_st22p_sr_codestream_max = codestream_max;
  ipmx_st22p_sr_ports = ports;
  ipmx_st22p_sr_plev = plev;
}

/* the test codec at 2.4 bpp writes the TR-08 section 8.1.1 Plev of 1080p59.94: level
 * 2k-1, Sublev3bpp */
#define IPMX_ST22P_TEST_PLEV (0x1004)
/* the test codec's max codestream: the GBRPLANAR8 frame over compress_ratio 10 */
#define IPMX_ST22P_TEST_CODESTREAM_MAX (1920 * 1080 * 3 / 10)

/* a compliant 1080p59.94 JPEG XS IPMX stream from the test codec at 2.4 bpp: frames
 * arrive at the frame rate, none dropped or incomplete, every SHA matches
 * (st22p_rx_digest_test(), 10 s), and the RX reads the SRs */
TEST(Ipmx_st22p, tx_rx_1080p) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  ipmx_st22p_sr_para(&para, 1, IPMX_ST22P_TEST_PLEV, IPMX_ST22P_TEST_CODESTREAM_MAX);

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* tx_rx_1080p over ST 2022-7: both legs deliver every frame, RX merges them and checks
 * every SHA, and each leg has its own SRs */
TEST(Ipmx_st22p, tx_rx_1080p_redundant) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  if (ctx->para.num_ports < 4)
    GTEST_SKIP() << "ST 2022-7 needs 4 ports, have " << ctx->para.num_ports;

  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  ipmx_st22p_sr_para(&para, 2, IPMX_ST22P_TEST_PLEV, IPMX_ST22P_TEST_CODESTREAM_MAX);

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* ---- the SDP across changes: RFC 8866 section 5.2 sess-version ---- */

static uint64_t ipmx_st22p_sdp_sess_version(const std::string& sdp) {
  unsigned long long id = 0, version = 0;
  for (const std::string& l : ipmx_st22p_sdp_lines(sdp))
    if (sscanf(l.c_str(), "o=- %llu %llu ", &id, &version) == 2) return version;
  ADD_FAILURE() << "no o= line";
  return 0;
}

/* after has sess-version version, every line starting with a changed prefix differs
 * and every other line but o= is the same */
static void ipmx_st22p_sdp_expect_change(const std::string& before,
                                         const std::string& after, uint64_t version,
                                         const std::vector<std::string>& changed) {
  std::vector<std::string> b = ipmx_st22p_sdp_lines(before);
  std::vector<std::string> a = ipmx_st22p_sdp_lines(after);

  EXPECT_EQ(ipmx_st22p_sdp_sess_version(after), version);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); i++) {
    if (!b[i].compare(0, 2, "o=")) continue;
    bool in = false;
    for (const std::string& p : changed)
      if (!b[i].compare(0, p.size(), p)) in = true;
    if (in)
      EXPECT_NE(a[i], b[i]) << "line " << i;
    else
      EXPECT_EQ(a[i], b[i]) << "line " << i;
  }
}

static const char* const ipmx_st22p_ts_refclk =
    "ptp=IEEE1588-2008:44-49-88-FF-FE-06-95-E0:0";
static std::string ipmx_st22p_sdp_before;
static std::string ipmx_st22p_sdp_refclk;
static std::string ipmx_st22p_sdp_version;

/* mid-stream: a ts-refclk, taken at the next frame start (1 s budget), is sess-version
 * 2 with only a=ts-refclk changed and one block version up; then the next block version,
 * once the RX reads it in a Sender Report (1 s budget), is sess-version 3 with the text
 * otherwise the same. The RX SRs count two version changes. The block version without a
 * content change only exercises the sess-version rule: TR-10-1 section 8.7 ties the
 * version to the Info Block content (see ipmx_st20_tx_set_block_version()), so an app
 * must not copy this */
static void ipmx_st22p_sdp_change_mid_stream(st22p_tx_handle* tx, st22p_rx_handle* rx,
                                             int) {
  struct ipmx_video_info info;
  struct ipmx_rx_sr_info sr = {};

  ipmx_st22p_sdp_before = ipmx_st22p_sdp(tx[0]);
  EXPECT_EQ(ipmx_st22p_sdp_sess_version(ipmx_st22p_sdp_before), 1u);
  ASSERT_EQ(ipmx_st22p_tx_set_ts_refclk(tx[0], ipmx_st22p_ts_refclk), 0);
  for (int i = 0; i < 100; i++) {
    ASSERT_EQ(ipmx_st22p_tx_get_info(tx[0], &info), 0);
    if (!strcmp(info.ts_refclk, ipmx_st22p_ts_refclk)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_STREQ(info.ts_refclk, ipmx_st22p_ts_refclk);
  ipmx_st22p_sdp_refclk = ipmx_st22p_sdp(tx[0]);
  ipmx_st22p_sdp_expect_change(ipmx_st22p_sdp_before, ipmx_st22p_sdp_refclk, 2,
                               {"a=ts-refclk:"});

  ASSERT_EQ(ipmx_st22p_tx_get_info(tx[0], &info), 0);
  EXPECT_EQ(info.version, 1);
  ASSERT_EQ(ipmx_st22p_tx_set_block_version(tx[0], info.version + 1), 0);
  for (int i = 0; i < 100; i++) {
    if (ipmx_st22p_rx_get_sr_info(rx[0], MTL_SESSION_PORT_P, &sr) == 0 &&
        sr.info.version == info.version + 1)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(sr.info.version, info.version + 1);
  ipmx_st22p_sdp_version = ipmx_st22p_sdp(tx[0]);
  ipmx_st22p_sdp_expect_change(ipmx_st22p_sdp_refclk, ipmx_st22p_sdp_version, 3, {});
}

/* after the stop, so the RX keeps its source: the usual SR save, then
 * st22p_tx_update_destination() to another group and port is sess-version 4 with only
 * m=, c= and a=source-filter changed */
static void ipmx_st22p_sdp_change_destination(st22p_tx_handle handle) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  struct st_tx_dest_info dst;

  ipmx_st22p_sr_save_tx_info(handle);
  EXPECT_EQ(ipmx_st22p_sdp_tx, ipmx_st22p_sdp_version);
  memset(&dst, 0, sizeof(dst));
  memcpy(dst.dip_addr[MTL_SESSION_PORT_P], ctx->mcast_ip_addr[MTL_PORT_R],
         MTL_IP_ADDR_LEN);
  dst.udp_port[MTL_SESSION_PORT_P] = IPMX_ST22P_UDP_PORT + 100;
  ASSERT_EQ(st22p_tx_update_destination(handle, &dst), 0);
  std::string dest = ipmx_st22p_sdp(handle);
  ipmx_st22p_sdp_expect_change(ipmx_st22p_sdp_version, dest, 4,
                               {"m=", "c=", "a=source-filter:"});
  ipmx_st22p_sdp_dump(ipmx_st22p_sdp_before, "_before_ts_refclk");
  ipmx_st22p_sdp_dump(ipmx_st22p_sdp_refclk, "_after_ts_refclk");
  ipmx_st22p_sdp_dump(ipmx_st22p_sdp_version, "_after_block_version");
  ipmx_st22p_sdp_dump(dest, "_after_destination");
}

/* tx_rx_1080p with a ts-refclk change and a block version mid-stream and a
 * destination update after the stop:
 * each SDP has the next sess-version and only the changed lines; the SDP taken before
 * the update still matches the RX Sender Reports */
TEST(Ipmx_st22p, tx_rx_1080p_sdp_changes) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  ipmx_st22p_sr_para(&para, 1, IPMX_ST22P_TEST_PLEV, IPMX_ST22P_TEST_CODESTREAM_MAX);
  ipmx_st22p_sr_version_changes = 2;
  para.mid_stream = ipmx_st22p_sdp_change_mid_stream;
  para.tx_check = ipmx_st22p_sdp_change_destination;
  ipmx_st22p_sdp_before.clear();
  ipmx_st22p_sdp_refclk.clear();
  ipmx_st22p_sdp_version.clear();

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* the stream_level of the SVT plugin config file, comment lines skipped, 0 if none */
static unsigned long ipmx_st22p_svt_cfg_level(const char* path) {
  std::ifstream in(path);
  std::string line;
  const std::string key = "\"stream_level\"";

  while (std::getline(in, line)) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos || line.compare(pos, 2, "//") == 0) continue;
    pos = line.find(key);
    if (pos == std::string::npos) continue;
    pos = line.find('"', line.find(':', pos + key.size()));
    if (pos == std::string::npos) return 0;
    return strtoul(line.c_str() + pos + 1, NULL, 16);
  }
  return 0;
}

/* why the real plugin case can't run here, empty if it can: the config's level is not
 * the case's or no CPU encoder at 1080p (no plugin); a create failure at the case's
 * size later fails the case */
static std::string ipmx_st22p_svt_skip(struct st_tests_context* ctx, uint16_t plev) {
  const char* cfg = getenv("SVT_JXS_MTL_PLUGIN_CONFIG");
  char msg[256];

  if (!cfg) return "SVT_JXS_MTL_PLUGIN_CONFIG not set";
  unsigned long level = ipmx_st22p_svt_cfg_level(cfg);
  if (level != plev) {
    snprintf(msg, sizeof(msg), "%s has stream_level 0x%04lx, this case needs 0x%04x", cfg,
             level, plev);
    return msg;
  }

  /* without the IPMX flag, so only a missing encoder can fail it */
  struct st22p_tx_ops ops;
  ipmx_st22p_tx_ops(ctx, &ops);
  ops.flags = 0;
  ops.device = ST_PLUGIN_DEVICE_CPU;
  ops.codestream_size = 1920 * 1080 * 3 / 8;
  st22p_tx_handle handle = st22p_tx_create(ctx->handle, &ops);
  if (!handle) return "no CPU JPEG XS encoder of GBRPLANAR8, load the SVT plugin";
  st22p_tx_free(handle);
  return "";
}

static void ipmx_st22p_sr_none(st22p_rx_handle handle) {
  struct ipmx_rx_sr_info sr;

  memset(&sr, 0xee, sizeof(sr));
  EXPECT_EQ(ipmx_st22p_rx_get_sr_info(handle, MTL_SESSION_PORT_P, &sr), -EBUSY);
  EXPECT_EQ(sr.received, 0u);
  EXPECT_EQ(sr.malformed, 0u);
  EXPECT_EQ(sr.ssrc_mismatch, 0u);
}

/* TP-1 section 14.3.3: an IPMX receiver takes a plain ST 2110-22 sender, which sends no
 * SRs; the digest test checks the frames arrive as without the flag, the getter finds
 * no SR */
TEST(Ipmx_st22p, noipmx_tx_1080p_sr_none) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.ipmx_rx_only = true;
  para.rx_check = ipmx_st22p_sr_none;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* ---- real SVT-JPEG-XS over the D11 matrix ---- */

/* the integer of env name, 0 when unset or not positive */
static int ipmx_st22p_env_int(const char* name) {
  const char* v = getenv(name);
  return v ? std::max(atoi(v), 0) : 0;
}

/* GBRPLANAR8: the top 3/4 the 8 full colour bars (white, yellow, cyan, green, magenta,
 * red, blue, black), the bottom 1/4 a black to white ramp. In a dump run every TX
 * framebuffer holds it, so every RX frame is a decode of it */
static void ipmx_st22p_svt_pattern(uint8_t* fb, uint32_t width, uint32_t height) {
  size_t plane = (size_t)width * height;
  uint8_t* g = fb;
  uint8_t* b = fb + plane;
  uint8_t* r = fb + 2 * plane;

  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      size_t i = (size_t)y * width + x;
      if (y < height * 3 / 4) {
        uint32_t bar = x * 8 / width;
        g[i] = bar < 4 ? 0xff : 0;
        r[i] = bar & 2 ? 0 : 0xff;
        b[i] = bar & 1 ? 0 : 0xff;
      } else {
        g[i] = b[i] = r[i] = (uint8_t)((uint64_t)x * 0xff / (width - 1));
      }
    }
  }
}

/* the RX frame IPMX_FRAME_DUMP_DIR keeps: its RX index and its planes, allocated before
 * the stream so the copy in the RX thread is a memcpy only */
static int ipmx_st22p_dump_idx;
static int ipmx_st22p_dump_rx_cnt;
static std::vector<uint8_t> ipmx_st22p_dump_rx;
static int ipmx_st22p_dump_width;
static int ipmx_st22p_dump_height;

static void ipmx_st22p_dump_rx_frame(struct st_frame* frame) {
  if (frame->width != (uint32_t)ipmx_st22p_dump_width ||
      frame->height != (uint32_t)ipmx_st22p_dump_height)
    return;
  if (ipmx_st22p_dump_rx_cnt++ != ipmx_st22p_dump_idx) return;

  uint8_t* dst = ipmx_st22p_dump_rx.data();
  for (int p = 0; p < 3; p++) {
    for (uint32_t y = 0; y < frame->height; y++) {
      memcpy(dst, (uint8_t*)frame->addr[p] + (size_t)y * frame->linesize[p],
             frame->width);
      dst += frame->width;
    }
  }
  info("%s, rx frame %d timestamp %" PRIu64 " kept\n", __func__, ipmx_st22p_dump_idx,
       frame->timestamp);
}

static void ipmx_st22p_dump_write(const std::vector<uint8_t>& data, const char* tag) {
  std::string path = std::string(getenv("IPMX_FRAME_DUMP_DIR")) + "/" +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name() +
                     tag + ".gbrp";
  FILE* f = fopen(path.c_str(), "w");
  ASSERT_NE(f, nullptr) << path;
  EXPECT_EQ(fwrite(data.data(), 1, data.size(), f), data.size()) << path;
  fclose(f);
}

/* the usual SR check, then the source pattern and the kept RX frame as raw GBRPLANAR8
 * (ffmpeg gbrp) <test name>_tx.gbrp and _rx.gbrp */
static void ipmx_st22p_dump_rx_check(st22p_rx_handle handle) {
  ipmx_st22p_sr_check(handle);
  ASSERT_GT(ipmx_st22p_dump_rx_cnt, ipmx_st22p_dump_idx) << "no rx frame to dump";
  std::vector<uint8_t> tx((size_t)ipmx_st22p_dump_width * ipmx_st22p_dump_height * 3);
  ipmx_st22p_svt_pattern(tx.data(), ipmx_st22p_dump_width, ipmx_st22p_dump_height);
  ipmx_st22p_dump_write(tx, "_tx");
  ipmx_st22p_dump_write(ipmx_st22p_dump_rx, "_rx");
}

/*
 * the stream of the real SVT-JPEG-XS encoder and decoder at fps, width x height and bpp
 * over ports legs (2 is ST 2022-7):
 * the codestreams the plugin's IPMX config makes pass the library's per-frame header
 * check, so RX gets every frame (none dropped, rate within 10 %, none incomplete), and
 * the SRs carry plev. Lab knobs, all unset by default:
 *   IPMX_ST22P_DURATION_S  streaming seconds instead of 10, at least 10 (TP-1 section
 *                          13.4: ~60 s); the RX frame budget is a count in flight, so
 *                          it holds for any length
 *   IPMX_ST22P_RX_FB_CNT   RX framebuffers instead of 3, 2 to 8 (7 with ST 2022-7);
 *                          each one more also widens the frame-count tolerance by one,
 *                          so record the value with the results
 *   IPMX_FRAME_DUMP_DIR    TX frames of colour bars instead of random data, and the
 *                          source and the RX frame at mid-stream written there
 */
static void ipmx_st22p_svt(enum st_fps fps_, int width_, int height_, int bpp,
                           uint16_t plev, int ports = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  enum st_fps fps[1] = {fps_};
  int width[1] = {width_};
  int height[1] = {height_};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  /* GBRPLANAR8 is 24 bits a pixel */
  int compress_ratio[1] = {24 / bpp};
  /* the plugin's max codestream is w x h x bpp / 8 */
  size_t codestream_max = (size_t)width_ * height_ * bpp / 8;

  std::string skip = ipmx_st22p_svt_skip(ctx, plev);
  if (!skip.empty()) GTEST_SKIP() << skip;

  struct st22p_rx_digest_test_para para;
  ipmx_st22p_sr_para(&para, ports, plev, codestream_max);
  para.device = ST_PLUGIN_DEVICE_CPU;
  para.no_digest = true;
  para.duration_s = ipmx_st22p_env_int("IPMX_ST22P_DURATION_S");
  if (para.duration_s && para.duration_s < 10) {
    info("%s, IPMX_ST22P_DURATION_S %d raised to 10\n", __func__, para.duration_s);
    para.duration_s = 0;
  }
  /* the ST 2022-7 RX takes one framebuffer more, ST22_FB_MAX_COUNT in all */
  int rx_fb_max = ports > 1 ? ST22_FB_MAX_COUNT - 1 : ST22_FB_MAX_COUNT;
  if (getenv("IPMX_ST22P_RX_FB_CNT")) {
    para.rx_fb_cnt = ipmx_st22p_env_int("IPMX_ST22P_RX_FB_CNT");
    if (para.rx_fb_cnt < 2 || para.rx_fb_cnt > rx_fb_max)
      FAIL() << "IPMX_ST22P_RX_FB_CNT " << getenv("IPMX_ST22P_RX_FB_CNT")
             << " out of range 2 to " << rx_fb_max;
  }

  const char* dump = getenv("IPMX_FRAME_DUMP_DIR");
  if (dump && dump[0]) {
    int duration_s = para.duration_s ? para.duration_s : 10;
    ipmx_st22p_dump_idx = (int)(st_frame_rate(fps_) * duration_s / 2);
    ipmx_st22p_dump_rx_cnt = 0;
    ipmx_st22p_dump_width = width_;
    ipmx_st22p_dump_height = height_;
    ipmx_st22p_dump_rx.assign((size_t)width_ * height_ * 3, 0);
    para.tx_fill = ipmx_st22p_svt_pattern;
    para.rx_frame = ipmx_st22p_dump_rx_frame;
    para.rx_check = ipmx_st22p_dump_rx_check;
  }

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* The 10 D11 points (TP-1 section 19.3): TR-08 section 8.1.1 level 2k-1
 * for 1080p and 4k-2 for 2160p, Sublev3bpp (0x..04) at 3 bpp and Sublev4bpp (0x..06,
 * the TR-08 Appendix B RGB rate) at 4 bpp. The plugin reads its config once per process,
 * so each case runs only under a config of its stream_level: ipmx_1080p.json (0x1004),
 * ipmx_2160p.json (0x2404), or a copy with the low byte 06 and "bpp": 4. */

/* 1080p50, 2k-1 Sublev3bpp */
TEST(Ipmx_st22p, tx_rx_svt_1080p50_3bpp) {
  ipmx_st22p_svt(ST_FPS_P50, 1920, 1080, 3, 0x1004);
}

/* 1080p50, 2k-1 Sublev4bpp */
TEST(Ipmx_st22p, tx_rx_svt_1080p50_4bpp) {
  ipmx_st22p_svt(ST_FPS_P50, 1920, 1080, 4, 0x1006);
}

/* 1080p59.94, 2k-1 Sublev3bpp */
TEST(Ipmx_st22p, tx_rx_svt_1080p59_94_3bpp) {
  ipmx_st22p_svt(ST_FPS_P59_94, 1920, 1080, 3, 0x1004);
}

/* 1080p59.94, 2k-1 Sublev4bpp */
TEST(Ipmx_st22p, tx_rx_svt_1080p59_94_4bpp) {
  ipmx_st22p_svt(ST_FPS_P59_94, 1920, 1080, 4, 0x1006);
}

/* 1080p60, 2k-1 Sublev3bpp: the top of the 2k-1 pixel rate */
TEST(Ipmx_st22p, tx_rx_svt_1080p60_3bpp) {
  ipmx_st22p_svt(ST_FPS_P60, 1920, 1080, 3, 0x1004);
}

/* 1080p60, 2k-1 Sublev4bpp */
TEST(Ipmx_st22p, tx_rx_svt_1080p60_4bpp) {
  ipmx_st22p_svt(ST_FPS_P60, 1920, 1080, 4, 0x1006);
}

/* 2160p59.94, 4k-2 Sublev3bpp */
TEST(Ipmx_st22p, tx_rx_svt_2160p59_94_3bpp) {
  ipmx_st22p_svt(ST_FPS_P59_94, 3840, 2160, 3, 0x2404);
}

/* 2160p59.94, 4k-2 Sublev4bpp */
TEST(Ipmx_st22p, tx_rx_svt_2160p59_94_4bpp) {
  ipmx_st22p_svt(ST_FPS_P59_94, 3840, 2160, 4, 0x2406);
}

/* 2160p60, 4k-2 Sublev3bpp: the top of the matrix for encode + decode in one process */
TEST(Ipmx_st22p, tx_rx_svt_2160p60_3bpp) {
  ipmx_st22p_svt(ST_FPS_P60, 3840, 2160, 3, 0x2404);
}

/* 2160p60, 4k-2 Sublev4bpp */
TEST(Ipmx_st22p, tx_rx_svt_2160p60_4bpp) {
  ipmx_st22p_svt(ST_FPS_P60, 3840, 2160, 4, 0x2406);
}

/* 1080p59.94 Sublev3bpp over ST 2022-7: both legs deliver every SVT frame, and
 * each leg's SRs and SDP media section carry the stream, as tx_rx_1080p_redundant */
TEST(Ipmx_st22p, tx_rx_svt_1080p59_94_3bpp_redundant) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  if (ctx->para.num_ports < 4)
    GTEST_SKIP() << "ST 2022-7 needs 4 ports, have " << ctx->para.num_ports;

  ipmx_st22p_svt(ST_FPS_P59_94, 1920, 1080, 3, 0x1004, 2);
}

#endif
