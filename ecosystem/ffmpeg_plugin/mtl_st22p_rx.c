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

typedef struct MtlSt22pDemuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StRxSessionPortArgs portArgs;
  /* arguments for session */
  int width, height;
  enum AVPixelFormat pixel_format;
  AVRational framerate;
  int fb_cnt;
  int codec_thread_cnt;

  mtl_handle dev_handle;
  st22p_rx_handle rx_handle;

  int64_t frame_counter;
} MtlSt22pDemuxerContext;

static int mtl_st22p_read_close(AVFormatContext* ctx) {
  MtlSt22pDemuxerContext* s = ctx->priv_data;

  dbg("%s(%d), start\n", __func__, s->idx);
  // Destroy rx session
  if (s->rx_handle) {
    st22p_rx_free(s->rx_handle);
    s->rx_handle = NULL;
    dbg(ctx, "%s(%d), st22p_rx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st22p_read_header(AVFormatContext* ctx) {
  MtlSt22pDemuxerContext* s = ctx->priv_data;
  AVStream* st = NULL;
  enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
  int img_buf_size;
  const AVPixFmtDescriptor* pix_fmt_desc = NULL;
  struct st22p_rx_ops ops_rx;
  int ret;

  dbg(ctx, "%s, start\n", __func__);
  memset(&ops_rx, 0, sizeof(ops_rx));

  ret = mtl_parse_rx_port(ctx, &s->devArgs, &s->portArgs, &ops_rx.port);
  if (ret < 0) {
    err(ctx, "%s, parse rx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_rx.flags |= ST22P_RX_FLAG_BLOCK_GET;
  ops_rx.codec = ST22_CODEC_JPEGXS;
  ops_rx.pack_type = ST22_PACK_CODESTREAM;
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;

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
  pix_fmt = s->pixel_format;
  pix_fmt_desc = av_pix_fmt_desc_get(pix_fmt);
  switch (pix_fmt) {
    case AV_PIX_FMT_YUV422P10LE:
      ops_rx.output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
      break;
    case AV_PIX_FMT_RGB24:
      ops_rx.output_fmt = ST_FRAME_FMT_RGB8;
      break;
    default:
      err(ctx, "%s, unsupported pixel format: %s\n", __func__, pix_fmt_desc->name);
      return AVERROR(EINVAL);
  }

  img_buf_size = av_image_get_buffer_size(pix_fmt, s->width, s->height, 1);
  if (img_buf_size < 0) {
    err(ctx, "%s, av_image_get_buffer_size failed with %d\n", __func__, img_buf_size);
    return img_buf_size;
  }
  dbg(ctx, "%s, img_buf_size: %d\n", __func__, img_buf_size);

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
  ctx->packet_size = img_buf_size;
  st->codecpar->bit_rate =
      av_rescale_q(ctx->packet_size, (AVRational){8, 1}, st->time_base);

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    mtl_st22p_read_close(ctx);
    return AVERROR(EIO);
  }

  ops_rx.name = "st22p_rx_ffmpeg";
  ops_rx.priv = s;  // Handle of priv_data registered to lib
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx.framebuff_cnt = s->fb_cnt;
  ops_rx.codec_thread_cnt = s->codec_thread_cnt;

  s->rx_handle = st22p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    err(ctx, "%s, st22p_rx_create failed\n", __func__);
    mtl_st22p_read_close(ctx);
    return AVERROR(EIO);
  }

  img_buf_size = st22p_rx_frame_size(s->rx_handle);
  if (img_buf_size != ctx->packet_size) {
    err(ctx, "%s, frame size mismatch %d:%u\n", __func__, img_buf_size, ctx->packet_size);
    mtl_st22p_read_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), rx handle %p\n", __func__, s->idx, s->rx_handle);
  return 0;
}

static int mtl_st22p_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  MtlSt22pDemuxerContext* s = ctx->priv_data;
  int ret = 0;
  struct st_frame* frame;

  dbg("%s(%d), start\n", __func__, s->idx);
  frame = st22p_rx_get_frame(s->rx_handle);
  if (!frame) {
    info(ctx, "%s(%d), st22p_rx_get_frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s(%d), st22p_rx_get_frame: %p\n", __func__, s->idx, frame);
  if (frame->data_size != ctx->packet_size) {
    err(ctx, "%s(%d), unexpected frame size received: %" PRId64 " (%u expected)\n",
        __func__, s->idx, frame->data_size, ctx->packet_size);
    st22p_rx_put_frame(s->rx_handle, frame);
    return AVERROR(EIO);
  }

  ret = av_new_packet(pkt, ctx->packet_size);
  if (ret != 0) {
    err(ctx, "%s(%d), av_new_packet failed with %d\n", __func__, s->idx, ret);
    st22p_rx_put_frame(s->rx_handle, frame);
    return ret;
  }
  /* todo: zero copy with external frame mode */
  mtl_memcpy(pkt->data, frame->addr[0], ctx->packet_size);
  st22p_rx_put_frame(s->rx_handle, frame);

  pkt->pts = pkt->dts = s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, pkt->pts);
  return 0;
}

#define OFFSET(x) offsetof(MtlSt22pDemuxerContext, x)
#define DEC (AV_OPT_FLAG_DECODING_PARAM)
static const AVOption mtl_st22p_rx_options[] = {
    // mtl dev port info
    MTL_RX_DEV_ARGS,
    // mtl rx port info
    MTL_RX_PORT_ARGS,
    // session info
    {"video_size",
     "Video frame size",
     OFFSET(width),
     AV_OPT_TYPE_IMAGE_SIZE,
     {.str = "1920x1080"},
     0,
     0,
     DEC},
    {"pix_fmt",
     "Pixel format for framebuffer",
     OFFSET(pixel_format),
     AV_OPT_TYPE_PIXEL_FMT,
     {.i64 = AV_PIX_FMT_YUV422P10LE},
     -1,
     INT32_MAX,
     DEC},
    /* avoid "Option pixel_format not found." error */
    {"pixel_format",
     "Pixel format for framebuffer",
     OFFSET(pixel_format),
     AV_OPT_TYPE_PIXEL_FMT,
     {.i64 = AV_PIX_FMT_YUV422P10LE},
     -1,
     INT32_MAX,
     DEC},
    {"fps",
     "Video frame rate",
     OFFSET(framerate),
     AV_OPT_TYPE_RATIONAL,
     {.dbl = 59.94},
     0,
     1000,
     DEC},
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
     DEC},
    {"codec_thread_cnt",
     "Codec threads count",
     OFFSET(codec_thread_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     64,
     DEC},
    {NULL},
};

static const AVClass mtl_st22p_demuxer_class = {
    .class_name = "mtl_st22p demuxer",
    .item_name = av_default_item_name,
    .option = mtl_st22p_rx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

#ifndef MTL_FFMPEG_4_4
const AVInputFormat ff_mtl_st22p_demuxer =
#else
AVInputFormat ff_mtl_st22p_demuxer =
#endif
    {
        .name = "mtl_st22p",
        .long_name = NULL_IF_CONFIG_SMALL("mtl st22p input device"),
        .priv_data_size = sizeof(MtlSt22pDemuxerContext),
        .read_header = mtl_st22p_read_header,
        .read_packet = mtl_st22p_read_packet,
        .read_close = mtl_st22p_read_close,
        .flags = AVFMT_NOFILE,
        .extensions = "mtl",
        .raw_codec_id = AV_CODEC_ID_RAWVIDEO,
        .priv_class = &mtl_st22p_demuxer_class,
};
