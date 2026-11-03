/* SPDX-License-Identifier: LicenseRef-IPMX-TBD */

/* st22p (JPEG XS, TR-10-11) with the IPMX flag on real ports: st22p_tx_create() /
 * st22p_rx_create() reject an IPMX config that breaks a TR-10-11 rule, and a compliant
 * IPMX TX to RX pair streams with every frame checked by SHA. The rule by rule checks
 * are tests/unit/ipmx/ipmx_st22_validate_test.cpp; these pin that the real create path
 * applies them and that the flag does not break the st22 data path. */

#ifdef MTL_HAS_IPMX

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
  ops->input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
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
  ops->output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
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

/* a compliant 1080p59.94 JPEG XS IPMX stream: frames arrive at the frame rate, none
 * incomplete, every SHA matches (st22p_rx_digest_test(), 10 s) */
TEST(Ipmx_st22p, tx_rx_1080p) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  enum st_frame_fmt fmt[1] = {ST_FRAME_FMT_YUV422PLANAR10LE};
  enum st22_codec codec[1] = {ST22_CODEC_JPEGXS};
  int compress_ratio[1] = {10};

  struct st22p_rx_digest_test_para para;
  test_st22p_init_rx_digest_para(&para);
  para.ipmx = true;

  st22p_rx_digest_test(fps, width, height, fmt, codec, compress_ratio, &para);
}

#endif
