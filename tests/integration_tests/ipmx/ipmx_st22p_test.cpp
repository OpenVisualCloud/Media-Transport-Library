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
 * Streaming: an IPMX TX to RX pair of 1080p59.94 GBRPLANAR8 (RGB 4:4:4 8-bit, D11).
 * The library checks every frame's JPEG XS picture header against the TR-08 section
 * 8.1.1 values it put in the boxes and drops a frame that does not match, so RX must
 * get every frame TX put, less those still in flight at stop.
 *   tx_rx_1080p         the test codec, which writes a synthetic header
 *                       (ipmx_jxs_test_header.cpp) with the SHA after it; RX checks
 *                       every SHA
 *   tx_rx_1080p_redundant  the same over ST 2022-7, TX on ports P and R, RX on ports
 *                       2 and 3: both legs deliver every frame with its SHA; skipped
 *                       with fewer than 4 ports
 *   tx_rx_svt_1080p_*   the real SVT-JPEG-XS plugin at 3 and 4 bpp; lossy, no SHA.
 *                       Skipped unless the plugin is loaded (KAHAWAI_CFG_PATH) and
 *                       SVT_JXS_MTL_PLUGIN_CONFIG names an IPMX config whose
 *                       stream_level is the Plev of that case: one KahawaiTest run per
 *                       config.
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
  /* one SR a frame over 10 s at 59.94 fps; st22p has no RX frame count to compare */
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

/* why the real plugin case can't run here, empty if it can */
static std::string ipmx_st22p_svt_skip(struct st_tests_context* ctx, uint16_t plev,
                                       size_t codestream_size) {
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
  ops.codestream_size = codestream_size;
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

/* the 1080p59.94 stream of the real SVT-JPEG-XS encoder and decoder at bpp: the
 * codestreams the plugin's IPMX config makes pass the library's per-frame header check,
 * so RX gets every frame (none dropped, rate within 10 %, none incomplete), and the SRs
 * carry plev */
static void ipmx_st22p_svt_1080p(int bpp, uint16_t plev) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  /* GBRPLANAR8 is 24 bits a pixel */
  int compress_ratio[1] = {24 / bpp};

  std::string skip =
      ipmx_st22p_svt_skip(ctx, plev, (size_t)width[0] * height[0] * bpp / 8);
  if (!skip.empty()) GTEST_SKIP() << skip;

  struct st22p_rx_digest_test_para para;
  /* the plugin's max codestream is w x h x bpp / 8 */
  ipmx_st22p_sr_para(&para, 1, plev, (size_t)width[0] * height[0] * bpp / 8);
  para.device = ST_PLUGIN_DEVICE_CPU;
  para.no_digest = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* Sublev3bpp, run with SVT_JXS_MTL_PLUGIN_CONFIG=<mtl-plugin>/ipmx_1080p.json */
TEST(Ipmx_st22p, tx_rx_svt_1080p_3bpp) {
  ipmx_st22p_svt_1080p(3, 0x1004);
}

/* Sublev4bpp, the TR-08 Appendix B RGB rate, run with a config of stream_level 0x1006
 * and bpp 4 */
TEST(Ipmx_st22p, tx_rx_svt_1080p_4bpp) {
  ipmx_st22p_svt_1080p(4, 0x1006);
}

#endif
