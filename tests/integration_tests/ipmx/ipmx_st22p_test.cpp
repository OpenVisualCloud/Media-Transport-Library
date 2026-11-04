/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

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
 * Plev (TR-10-11 section 12, TR-10-15 Part 1 section 9).
 *   rx_get_sr_info_errors  the getter return codes of an RX with no sender: -EBUSY,
 *                       -EINVAL, and -EIO without the IPMX flag
 *   noipmx_tx_1080p_sr_none  a plain ST 2110-22 TX into an IPMX RX: frames as usual,
 *                       no SR (TP-1 section 14.3.3)
 *
 * Run: KahawaiTest ... --gtest_filter='Ipmx_st22p.*'
 */

#ifdef MTL_HAS_IPMX

#include <fstream>
#include <sstream>
#include <string>

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

static void ipmx_st22p_sr_save_tx_info(st22p_tx_handle handle) {
  ipmx_st22p_sr_tx_info_valid =
      ipmx_st22p_tx_get_info(handle, &ipmx_st22p_sr_tx_info) == 0;
  EXPECT_TRUE(ipmx_st22p_sr_tx_info_valid);
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
  EXPECT_EQ(sr.version_changes, 0u);
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
}

static void ipmx_st22p_sr_check(st22p_rx_handle handle) {
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
                               uint16_t plev) {
  test_st22p_init_rx_digest_para(para);
  para->ipmx = true;
  para->redundant = ports > 1;
  para->tx_check = ipmx_st22p_sr_save_tx_info;
  para->rx_check = ipmx_st22p_sr_check;
  ipmx_st22p_sr_tx_info_valid = false;
  ipmx_st22p_sr_ports = ports;
  ipmx_st22p_sr_plev = plev;
}

/* the test codec at 2.4 bpp writes the TR-08 section 8.1.1 Plev of 1080p59.94: level
 * 2k-1, Sublev3bpp */
#define IPMX_ST22P_TEST_PLEV (0x1004)

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
  ipmx_st22p_sr_para(&para, 1, IPMX_ST22P_TEST_PLEV);

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
  ipmx_st22p_sr_para(&para, 2, IPMX_ST22P_TEST_PLEV);

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
  ipmx_st22p_sr_para(&para, 1, plev);
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
