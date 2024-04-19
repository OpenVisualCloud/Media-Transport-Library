/*
 * mtl st2110-30 video muxer
 * Copyright (c) 2024 Intel
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <mtl/st30_pipeline_api.h>

#include "mtl_common.h"

typedef struct mtlSt30pMuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* session arguments */
  char* port;
  char* local_addr;
  char* tx_addr;
  int udp_port;
  int payload_type;
  int fb_cnt;
  int session_cnt;

  mtl_handle dev_handle;
  st30p_tx_handle tx_handle;

  int64_t frame_counter;
  size_t frame_size;
} mtlSt30pMuxerContext;

static int mtl_st30p_write_close(AVFormatContext* ctx) {
  mtlSt30pMuxerContext* s = ctx->priv_data;

  dbg(ctx, "%s, start\n", __func__);

  // Destroy tx session
  if (s->tx_handle) {
    st30p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    info(ctx, "%s(%d), st30p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int mtl_st30p_write_header(AVFormatContext* ctx) {
  mtlSt30pMuxerContext* s = ctx->priv_data;
  struct st30p_tx_ops ops_tx;

  dbg("%s, start\n", __func__);
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.flags |= ST30P_TX_FLAG_BLOCK_GET;

  if (NULL == s->port) {
    err(ctx, "%s, port NULL\n", __func__);
    return AVERROR(EINVAL);
  }
  if (strlen(s->port) > MTL_PORT_MAX_LEN) {
    err(ctx, "%s, port %s too long\n", __func__, s->port);
    return AVERROR(EINVAL);
  }
  ops_tx.port.num_port = 1;
  snprintf(ops_tx.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", s->port);

  if (NULL == s->tx_addr) {
    err(ctx, "%s, tx_addr NULL\n", __func__);
    return AVERROR(EINVAL);
  } else if (sscanf(s->tx_addr, "%hhu.%hhu.%hhu.%hhu",
                    &ops_tx.port.dip_addr[MTL_PORT_P][0],
                    &ops_tx.port.dip_addr[MTL_PORT_P][1],
                    &ops_tx.port.dip_addr[MTL_PORT_P][2],
                    &ops_tx.port.dip_addr[MTL_PORT_P][3]) != MTL_IP_ADDR_LEN) {
    err(ctx, "%s, failed to parse tx IP address: %s\n", __func__, s->tx_addr);
    return AVERROR(EINVAL);
  }

  if ((s->udp_port < 0) || (s->udp_port > 0xFFFF)) {
    err(ctx, "%s, invalid UDP port: %d\n", __func__, s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_tx.port.udp_port[MTL_PORT_P] = s->udp_port;
  if ((s->payload_type < 0) || (s->payload_type > 0x7F)) {
    err(ctx, "%s, invalid payload_type: %d\n", __func__, s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_tx.port.payload_type = s->payload_type;

  // todo

  // get mtl instance
  s->dev_handle =
      mtl_instance_get(s->port, s->local_addr, s->session_cnt, 0, NULL, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl_instance_get fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.name = "st30p";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  info(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_tx.framebuff_cnt = s->fb_cnt;

  s->tx_handle = st30p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st30p_tx_create failed\n", __func__);
    mtl_st30p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->frame_size = st30p_tx_frame_size(s->tx_handle);
  if (s->frame_size <= 0) {
    err(ctx, "%s, st30p_tx_frame_size failed\n", __func__);
    mtl_st30p_write_close(ctx);
    return AVERROR(EINVAL);
  }

  info(ctx, "%s(%d), st30p_tx_create succ %p\n", __func__, s->idx, s->tx_handle);
  s->frame_counter = 0;
  return 0;
}

static int mtl_st30p_write_packet(AVFormatContext* ctx, AVPacket* pkt) {
  mtlSt30pMuxerContext* s = ctx->priv_data;
  struct st30_frame* frame;

  dbg("%s, start\n", __func__);
  frame = st30p_tx_get_frame(s->tx_handle);
  if (!frame) {
    info(ctx, "%s, st30p_tx_get_frame timeout\n", __func__);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s, st30p_tx_get_frame: %p\n", __func__, frame);
  if (frame->data_size != s->frame_size) {
    err(ctx,
        "%s(%d), unexpected frame size received: %" PRIu64 " (%" PRIu64 " expected)\n",
        __func__, s->idx, frame->data_size, s->frame_size);
    return AVERROR(EIO);
  }

  mtl_memcpy(frame->addr, pkt->data, s->frame_size);

  st30p_tx_put_frame(s->tx_handle, frame);
  s->frame_counter++;
  dbg(ctx, "%s, frame counter %" PRId64 "\n", __func__, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(mtlSt30pMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st30p_tx_options[] = {
    // mtl port info
    {"port", "ST port", OFFSET(port), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = ENC},
    {"local_addr",
     "Local IP address",
     OFFSET(local_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    // mtl TX session info
    {"tx_addr",
     "TX IP address",
     OFFSET(tx_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    {"udp_port",
     "UDP port",
     OFFSET(udp_port),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     ENC},
    {"payload_type",
     "RX session payload type",
     OFFSET(payload_type),
     AV_OPT_TYPE_INT,
     {.i64 = 112},
     -1,
     INT_MAX,
     ENC},
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
     ENC},
    {"total_sessions",
     "Total sessions count",
     OFFSET(session_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     INT_MAX,
     ENC},
    {NULL},
};

static const AVClass mtl_st30p_muxer_class = {
    .class_name = "mtl_st30p muxer",
    .item_name = av_default_item_name,
    .option = mtl_st30p_tx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st30p_muxer = {
    .name = "mtl_st30p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st30p output device"),
    .priv_data_size = sizeof(mtlSt30pMuxerContext),
    .write_header = mtl_st30p_write_header,
    .write_packet = mtl_st30p_write_packet,
    .write_trailer = mtl_st30p_write_close,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .control_message = NULL,
    .priv_class = &mtl_st30p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st30p_muxer = {
    .p.name = "mtl_st30p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st30p output device"),
    .priv_data_size = sizeof(mtlSt30pMuxerContext),
    .write_header = mtl_st30p_write_header,
    .write_packet = mtl_st30p_write_packet,
    .write_trailer = mtl_st30p_write_close,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st30p_muxer_class,
};
#endif
