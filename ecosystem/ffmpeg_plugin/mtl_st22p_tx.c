/*
 * mtl st2110-20 video muxer
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

#include "mtl_common.h"

typedef struct mtlMuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* session arguments */
  char* port;
  char* local_addr;
  char* tx_addr;
  int udp_port;
  int payload_type;
  int fb_cnt;
  float bpp;
  int session_cnt;
  int codec_thread_cnt;

  int width;
  int height;
  enum AVPixelFormat pixel_format;
  AVRational framerate;
  mtl_handle dev_handle;
  st22p_tx_handle tx_handle;

  int64_t frame_counter;
  size_t frame_size;
} mtlMuxerContext;

static int mtl_st22p_write_header(AVFormatContext* ctx) {
  mtlMuxerContext* s = ctx->priv_data;
  struct st22p_tx_ops ops_tx;
  const AVPixFmtDescriptor* pix_fmt_desc = NULL;

  dbg("%s, start\n", __func__);
  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
  ops_tx.codec = ST22_CODEC_JPEGXS;
  ops_tx.pack_type = ST22_PACK_CODESTREAM;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_tx.codec_thread_cnt = 0;

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

  ops_tx.width = s->width = ctx->streams[0]->codecpar->width;
  ops_tx.height = s->height = ctx->streams[0]->codecpar->height;
  /* bpp */
  info(ctx, "%s, bpp: %f\n", __func__, s->bpp);
  ops_tx.codestream_size = (float)ops_tx.width * ops_tx.height * s->bpp / 8;
  s->framerate = ctx->streams[0]->avg_frame_rate;
  ops_tx.fps = framerate_to_st_fps(s->framerate);
  if (ops_tx.fps == ST_FPS_MAX) {
    err(ctx, "%s, frame rate %0.2f is not supported\n", __func__, av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  s->pixel_format = ctx->streams[0]->codecpar->format;
  av_pix_fmt_desc_get(s->pixel_format);

  /* transport_fmt is hardcode now */
  switch (s->pixel_format) {
    case AV_PIX_FMT_YUV422P10LE:
      ops_tx.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
      break;
    case AV_PIX_FMT_RGB24:
      ops_tx.input_fmt = ST_FRAME_FMT_RGB8;
      break;
    default:
      err(ctx, "%s, unsupported pixel format: %s\n", __func__, pix_fmt_desc->name);
      return AVERROR(EINVAL);
  }

  // get mtl instance
  s->dev_handle =
      mtl_instance_get(s->port, s->local_addr, s->session_cnt, 0, NULL, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl_instance_get fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.name = "st22p";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  info(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_tx.framebuff_cnt = s->fb_cnt;

  s->tx_handle = st22p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st22p_tx_create failed\n", __func__);
    return AVERROR(EIO);
  }

  s->frame_size = st22p_tx_frame_size(s->tx_handle);
  if (s->frame_size <= 0) {
    err(ctx, "%s, st22p_tx_frame_size failed\n", __func__);
    return AVERROR(EINVAL);
  }

  info(ctx, "%s(%d), st22p_tx_create succ %p\n", __func__, s->idx, s->tx_handle);
  s->frame_counter = 0;
  return 0;
}

static int mtl_st22p_write_packet(AVFormatContext* ctx, AVPacket* pkt) {
  mtlMuxerContext* s = ctx->priv_data;
  struct st_frame* frame;

  dbg("%s, start\n", __func__);
  frame = st22p_tx_get_frame(s->tx_handle);
  if (!frame) {
    info(ctx, "%s, st22p_tx_get_frame timeout\n", __func__);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s, st22p_tx_get_frame: %p\n", __func__, frame);
  if (frame->data_size != s->frame_size) {
    err(ctx,
        "%s(%d), unexpected frame size received: %" PRIu64 " (%" PRIu64 " expected)\n",
        __func__, s->idx, frame->data_size, s->frame_size);
    return AVERROR(EIO);
  }
  /* todo: zero copy with external frame mode */
  mtl_memcpy(frame->addr[0], pkt->data, s->frame_size);

  st22p_tx_put_frame(s->tx_handle, frame);
  s->frame_counter++;
  dbg(ctx, "%s, frame counter %" PRId64 "\n", __func__, s->frame_counter);
  return 0;
}

static int mtl_st22p_write_trailer(AVFormatContext* ctx) {
  mtlMuxerContext* s = ctx->priv_data;

  dbg(ctx, "%s, start\n", __func__);

  // Destroy tx session
  if (s->tx_handle) {
    st22p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    info(ctx, "%s(%d), st22p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), succ\n", __func__, s->idx);
  return 0;
}

#define OFFSET(x) offsetof(mtlMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st22p_tx_options[] = {
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
    {"bpp", "bit per pixel", OFFSET(bpp), AV_OPT_TYPE_FLOAT, {.dbl = 3.0}, 0.1, 8.0, ENC},
    {"codec_thread_cnt",
     "Codec threads count",
     OFFSET(codec_thread_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     64,
     ENC},
    {NULL},
};

static const AVClass mtl_st22p_muxer_class = {
    .class_name = "mtl muxer",
    .item_name = av_default_item_name,
    .option = mtl_st22p_tx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st22p_muxer = {
    .name = "mtl_st22p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st22p output device"),
    .priv_data_size = sizeof(mtlMuxerContext),
    .write_header = mtl_st22p_write_header,
    .write_packet = mtl_st22p_write_packet,
    .write_trailer = mtl_st22p_write_trailer,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .control_message = NULL,
    .priv_class = &mtl_st22p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st22p_muxer = {
    .p.name = "mtl_st22p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st22p output device"),
    .priv_data_size = sizeof(mtlMuxerContext),
    .write_header = mtl_st22p_write_header,
    .write_packet = mtl_st22p_write_packet,
    .write_trailer = mtl_st22p_write_trailer,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st22p_muxer_class,
};
#endif
