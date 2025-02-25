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

#include <mtl/st_convert_api.h>

#include "mtl_common.h"

typedef struct mtlSt20pMuxerContext {
  const AVClass *class; /**< Class for private options. */

  int idx;
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StTxSessionPortArgs portArgs;
  /* arguments for session */
  int fb_cnt;
  int width;
  int height;
  enum AVPixelFormat pixel_format;
  AVRational framerate;
  mtl_handle dev_handle;
  st20p_tx_handle tx_handle;

  int64_t frame_counter;
  int frame_size;
} mtlSt20pMuxerContext;

static int mtl_st20p_write_close(AVFormatContext *ctx) {
  mtlSt20pMuxerContext *s = ctx->priv_data;

  dbg("%s(%d), start\n", __func__, s->idx);
  // Destroy tx session
  if (s->tx_handle) {
    st20p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    dbg(ctx, "%s(%d), st20p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st20p_write_header(AVFormatContext *ctx) {
  mtlSt20pMuxerContext *s = ctx->priv_data;
  struct st20p_tx_ops ops_tx;
  int ret;

  dbg(ctx, "%s, start\n", __func__);
  memset(&ops_tx, 0, sizeof(ops_tx));

  ret = mtl_parse_tx_port(ctx, &s->devArgs, &s->portArgs, &ops_tx.port);
  if (ret < 0) {
    err(ctx, "%s, parse tx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.flags |= ST20P_TX_FLAG_BLOCK_GET;

  ops_tx.width = s->width = ctx->streams[0]->codecpar->width;
  ops_tx.height = s->height = ctx->streams[0]->codecpar->height;
  s->framerate = ctx->streams[0]->avg_frame_rate;
  ops_tx.fps = framerate_to_st_fps(s->framerate);
  if (ops_tx.fps == ST_FPS_MAX) {
    err(ctx, "%s, frame rate %0.2f is not supported\n", __func__, av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  s->pixel_format = ctx->streams[0]->codecpar->format;

  /* transport_fmt is hardcode now */
  switch (s->pixel_format) {
    case AV_PIX_FMT_YUV422P10LE:
      ops_tx.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
      ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
      break;
    case AV_PIX_FMT_Y210LE: /* This format is not supported by MTL plugin.
                               This is workaround
                               for Intel(R) Tiber(TM) Broadcast Suite */
      ops_tx.transport_fmt = ST20_FMT_YUV_422_10BIT;
      ops_tx.input_fmt = ST_FRAME_FMT_Y210;
      break;
    case AV_PIX_FMT_RGB24:
      ops_tx.input_fmt = ST_FRAME_FMT_RGB8;
      ops_tx.transport_fmt = ST20_FMT_RGB_8BIT;
      break;
    default:
      err(ctx, "%s, unsupported pixel format: %d\n", __func__, s->pixel_format);
      return AVERROR(EINVAL);
  }

  ops_tx.name = "st20p_ffmpge";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  dbg(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_tx.framebuff_cnt = s->fb_cnt;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  ret = mtl_start(s->dev_handle);
  if (ret < 0) {
    err(ctx, "%s, mtl start fail %d\n", __func__, ret);
    mtl_st20p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->tx_handle = st20p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st20p_tx_create failed\n", __func__);
    mtl_st20p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->frame_size = st20p_tx_frame_size(s->tx_handle);
  info(ctx, "%s(%d), tx_handle %p\n", __func__, s->idx, s->tx_handle);
  return 0;
}

static int mtl_st20p_write_packet(AVFormatContext *ctx, AVPacket *pkt) {
  mtlSt20pMuxerContext *s = ctx->priv_data;
  struct st_frame *frame;

  if (pkt->size != s->frame_size) {
    err(ctx, "%s(%d), unexpected pkt size: %d (%d expected)\n", __func__, s->idx,
        pkt->size, s->frame_size);
    return AVERROR(EIO);
  }

  dbg("%s(%d), start\n", __func__, s->idx);
  frame = st20p_tx_get_frame(s->tx_handle);
  if (!frame) {
    info(ctx, "%s(%d), st20p_tx_get_frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s(%d), st20p_tx_get_frame: %p\n", __func__, s->idx, frame);

  /* This format is not supported by MTL plugin.
     This is workaround for Intel(R) Tiber(TM) Broadcast Suite */
  if (s->pixel_format == AV_PIX_FMT_Y210LE) {
    st20_y210_to_rfc4175_422be10((uint16_t*)pkt->data,
                                 (struct st20_rfc4175_422_10_pg2_be*)(frame->addr[0]),
                                 s->width, s->height);
  }

  /* todo: zero copy with external frame mode */
  mtl_memcpy(frame->addr[0], pkt->data, s->frame_size);

  st20p_tx_put_frame(s->tx_handle, frame);
  s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(mtlSt20pMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st20p_tx_options[] = {
    // mtl dev port info
    MTL_TX_DEV_ARGS,
    // mtl tx port info
    MTL_TX_PORT_ARGS,
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
     ENC},
    {NULL},
};

static const AVClass mtl_st20p_muxer_class = {
    .class_name = "mtl_st20p muxer",
    .item_name = av_default_item_name,
    .option = mtl_st20p_tx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st20p_muxer = {
    .name = "mtl_st20p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st20p output device"),
    .priv_data_size = sizeof(mtlSt20pMuxerContext),
    .write_header = mtl_st20p_write_header,
    .write_packet = mtl_st20p_write_packet,
    .write_trailer = mtl_st20p_write_close,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .control_message = NULL,
    .priv_class = &mtl_st20p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st20p_muxer = {
    .p.name = "mtl_st20p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st20p output device"),
    .priv_data_size = sizeof(mtlSt20pMuxerContext),
    .write_header = mtl_st20p_write_header,
    .write_packet = mtl_st20p_write_packet,
    .write_trailer = mtl_st20p_write_close,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st20p_muxer_class,
};
#endif
