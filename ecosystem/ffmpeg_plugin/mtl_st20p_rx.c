/*
 * mtl st2110-20 demuxer
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

typedef struct MtlSt20pDemuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* arguments */
  char* port;
  char* local_addr;
  char* rx_addr;
  int udp_port;
  int payload_type;
  int width;
  int height;
  char* pixel_format;
  AVRational framerate;
  int fb_cnt;
  int session_cnt;
  char* dma_dev;

  mtl_handle dev_handle;
  st20p_rx_handle rx_handle;

  int64_t frame_counter;
} MtlSt20pDemuxerContext;

static int mtl_st20p_read_header(AVFormatContext* ctx) {
  MtlSt20pDemuxerContext* s = ctx->priv_data;
  AVStream* st = NULL;
  enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
  int packet_size = 0;
  const AVPixFmtDescriptor* pix_fmt_desc = NULL;
  struct st20p_rx_ops ops_rx;

  dbg("%s, start\n", __func__);
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.flags |= ST20P_RX_FLAG_BLOCK_GET;

  if (NULL == s->port) {
    err(ctx, "%s, port NULL\n", __func__);
    return AVERROR(EINVAL);
  }
  if (strlen(s->port) > MTL_PORT_MAX_LEN) {
    err(ctx, "%s, port %s too long\n", __func__, s->port);
    return AVERROR(EINVAL);
  }
  ops_rx.port.num_port = 1;
  snprintf(ops_rx.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", s->port);

  if (NULL == s->rx_addr) {
    err(ctx, "%s, rx_addr NULL\n", __func__);
    return AVERROR(EINVAL);
  } else if (sscanf(
                 s->rx_addr, "%hhu.%hhu.%hhu.%hhu", &ops_rx.port.ip_addr[MTL_PORT_P][0],
                 &ops_rx.port.ip_addr[MTL_PORT_P][1], &ops_rx.port.ip_addr[MTL_PORT_P][2],
                 &ops_rx.port.ip_addr[MTL_PORT_P][3]) != MTL_IP_ADDR_LEN) {
    err(ctx, "%s, failed to parse rx IP address: %s\n", __func__, s->rx_addr);
    return AVERROR(EINVAL);
  }

  if ((s->udp_port < 0) || (s->udp_port > 0xFFFF)) {
    err(ctx, "%s, invalid UDP port: %d\n", __func__, s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_rx.port.udp_port[MTL_PORT_P] = s->udp_port;
  if ((s->payload_type < 0) || (s->payload_type > 0x7F)) {
    err(ctx, "%s, invalid payload_type: %d\n", __func__, s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_rx.port.payload_type = s->payload_type;

  if (s->width <= 0) {
    err(ctx, "%s, invalid width: %d\n", __func__, s->width);
    return AVERROR(EINVAL);
  }
  ops_rx.width = s->width;
  if (s->height <= 0) {
    err(ctx, "%s, invalid height: %d\n", __func__, s->height);
    return AVERROR(EINVAL);
  }
  ops_rx.height = s->height;
  ops_rx.fps = framerate_to_st_fps(s->framerate);
  if (ops_rx.fps == ST_FPS_MAX) {
    err(ctx, "%s, frame rate %0.2f is not supported\n", __func__, av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  /* transport_fmt is hardcode now */
  pix_fmt = av_get_pix_fmt(s->pixel_format);
  pix_fmt_desc = av_pix_fmt_desc_get(pix_fmt);
  switch (pix_fmt) {
    case AV_PIX_FMT_YUV422P10LE:
      ops_rx.transport_fmt = ST20_FMT_YUV_422_10BIT;
      ops_rx.output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
      break;
    case AV_PIX_FMT_RGB24:
      ops_rx.transport_fmt = ST20_FMT_RGB_8BIT;
      ops_rx.output_fmt = ST_FRAME_FMT_RGB8;
      break;
    default:
      err(ctx, "%s, unsupported pixel format: %s\n", __func__, pix_fmt_desc->name);
      return AVERROR(EINVAL);
  }

  packet_size = av_image_get_buffer_size(pix_fmt, s->width, s->height, 1);
  if (packet_size < 0) {
    err(ctx, "%s, av_image_get_buffer_size failed with %d\n", __func__, packet_size);
    return packet_size;
  }
  info(ctx, "%s, packet size: %d\n", __func__, packet_size);

  if (s->dma_dev) {
    info(ctx, "%s, try to enable DMA offload\n", __func__);
    ops_rx.flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  }

  st = avformat_new_stream(ctx, NULL);
  if (!st) {
    err(ctx, "%s, avformat_new_stream fail\n", __func__);
    return AVERROR(ENOMEM);
  }

  st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  st->codecpar->codec_id = ctx->iformat->raw_codec_id;
  st->codecpar->format = pix_fmt;
  st->codecpar->width = s->width;
  st->codecpar->height = s->height;
  avpriv_set_pts_info(st, 64, s->framerate.den, s->framerate.num);
  ctx->packet_size = packet_size;
  st->codecpar->bit_rate =
      av_rescale_q(ctx->packet_size, (AVRational){8, 1}, st->time_base);

  // get mtl instance
  s->dev_handle =
      mtl_instance_get(s->port, s->local_addr, 0, s->session_cnt, s->dma_dev, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl_instance_get fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_rx.name = "st20p_rx";
  ops_rx.priv = s;  // Handle of priv_data registered to lib
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  info(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_rx.framebuff_cnt = s->fb_cnt;

  s->rx_handle = st20p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    err(ctx, "%s, st20p_rx_create failed\n", __func__);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), st20p_rx_create succ %p\n", __func__, s->idx, s->rx_handle);
  s->frame_counter = 0;
  return 0;
}

static int mtl_st20p_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  MtlSt20pDemuxerContext* s = ctx->priv_data;
  int ret = 0;
  struct st_frame* frame;

  dbg("%s, start\n", __func__);
  frame = st20p_rx_get_frame(s->rx_handle);
  if (!frame) {
    info(ctx, "%s, st20p_rx_get_frame timeout\n", __func__);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s, st20p_rx_get_frame: %p\n", __func__, frame);
  if (frame->data_size != ctx->packet_size) {
    err(ctx, "%s(%d), unexpected frame size received: %" PRId64 " (%u expected)\n",
        __func__, s->idx, frame->data_size, ctx->packet_size);
    st20p_rx_put_frame(s->rx_handle, frame);
    return AVERROR(EIO);
  }

  ret = av_new_packet(pkt, ctx->packet_size);
  if (ret != 0) {
    err(ctx, "%s, av_new_packet failed with %d\n", __func__, ret);
    st20p_rx_put_frame(s->rx_handle, frame);
    return ret;
  }
  /* todo: zero copy with external frame mode */
  mtl_memcpy(pkt->data, frame->addr[0], ctx->packet_size);
  st20p_rx_put_frame(s->rx_handle, frame);

  pkt->pts = pkt->dts = s->frame_counter++;
  dbg(ctx, "%s, frame counter %" PRId64 "\n", pkt->pts);
  return 0;
}

static int mtl_st20p_read_close(AVFormatContext* ctx) {
  MtlSt20pDemuxerContext* s = ctx->priv_data;

  dbg("%s, start\n", __func__);
  // Destroy rx session
  if (s->rx_handle) {
    st20p_rx_free(s->rx_handle);
    s->rx_handle = NULL;
    info(ctx, "%s(%d), st20p_rx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), succ\n", __func__, s->idx);
  return 0;
}

#define OFFSET(x) offsetof(MtlSt20pDemuxerContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption mtl_options[] = {
    // mtl port info
    {"port", "mtl port", OFFSET(port), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = DEC},
    {"local_addr",
     "Local IP address",
     OFFSET(local_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    // mtl RX session info
    {"rx_addr",
     "RX session IP address",
     OFFSET(rx_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {"udp_port",
     "UDP port",
     OFFSET(udp_port),
     AV_OPT_TYPE_INT,
     {.i64 = 20000},
     -1,
     INT_MAX,
     DEC},
    {"payload_type",
     "RX session payload type",
     OFFSET(payload_type),
     AV_OPT_TYPE_INT,
     {.i64 = 112},
     -1,
     INT_MAX,
     DEC},
    {"width",
     "Video frame width",
     OFFSET(width),
     AV_OPT_TYPE_INT,
     {.i64 = 1920},
     -1,
     INT_MAX,
     DEC},
    {"height",
     "Video frame height",
     OFFSET(height),
     AV_OPT_TYPE_INT,
     {.i64 = 1080},
     -1,
     INT_MAX,
     DEC},
    {"pixel_format",
     "Video frame format",
     OFFSET(pixel_format),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {"framerate",
     "Video frame rate",
     OFFSET(framerate),
     AV_OPT_TYPE_VIDEO_RATE,
     {.str = "59.94"},
     0,
     INT_MAX,
     DEC},
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
     DEC},
    {"total_sessions",
     "Total sessions count",
     OFFSET(session_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     INT_MAX,
     DEC},
    {"dma_dev",
     "DMA device node",
     OFFSET(dma_dev),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {NULL},
};

static const AVClass mtl_demuxer_class = {
    .class_name = "mtl demuxer",
    .item_name = av_default_item_name,
    .option = mtl_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

#ifndef MTL_FFMPEG_4_4
const AVInputFormat ff_mtl_st20p_demuxer =
#else
AVInputFormat ff_mtl_st20p_demuxer =
#endif
    {
        .name = "mtl_st20p",
        .long_name = NULL_IF_CONFIG_SMALL("mtl st20p input device"),
        .priv_data_size = sizeof(MtlSt20pDemuxerContext),
        .read_header = mtl_st20p_read_header,
        .read_packet = mtl_st20p_read_packet,
        .read_close = mtl_st20p_read_close,
        .flags = AVFMT_NOFILE,
        .extensions = "mtl",
        .raw_codec_id = AV_CODEC_ID_RAWVIDEO,
        .priv_class = &mtl_demuxer_class,
};
