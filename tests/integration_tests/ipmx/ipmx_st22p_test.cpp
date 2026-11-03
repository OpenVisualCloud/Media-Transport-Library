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
 *                       2 and 3: with IPMX SRs on both legs, both legs still deliver
 *                       every frame with its SHA; skipped with fewer than 4 ports. No
 *                       st22 SR receive yet, the SR content is pinned by the unit
 *                       cases tests/unit/ipmx/ipmx_st22_sr_test.cpp
 *   tx_rx_svt_1080p_*   the real SVT-JPEG-XS plugin at 3 and 4 bpp; lossy, no SHA.
 *                       Skipped unless the plugin is loaded (KAHAWAI_CFG_PATH) and
 *                       SVT_JXS_MTL_PLUGIN_CONFIG names an IPMX config whose
 *                       stream_level is the Plev of that case: one KahawaiTest run per
 *                       config.
 *
 * Run: KahawaiTest ... --gtest_filter='Ipmx_st22p.*'
 */

#ifdef MTL_HAS_IPMX

#include <fstream>
#include <sstream>
#include <string>

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

/* a compliant 1080p59.94 JPEG XS IPMX stream from the test codec at 2.4 bpp: frames
 * arrive at the frame rate, none dropped or incomplete, every SHA matches
 * (st22p_rx_digest_test(), 10 s) */
TEST(Ipmx_st22p, tx_rx_1080p) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_GBRPLANAR8};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.ipmx = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

/* tx_rx_1080p over ST 2022-7: with IPMX SRs on both legs, both legs still deliver every
 * frame, RX merges them and checks every SHA; the SR content is in the unit cases */
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
  test_st22p_init_rx_digest_para(&para);
  para.ipmx = true;
  para.redundant = true;

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

/* the 1080p59.94 stream of the real SVT-JPEG-XS encoder and decoder at bpp: the
 * codestreams the plugin's IPMX config makes pass the library's per-frame header check,
 * so RX gets every frame (none dropped, rate within 10 %, none incomplete) */
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
  test_st22p_init_rx_digest_para(&para);
  para.ipmx = true;
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
