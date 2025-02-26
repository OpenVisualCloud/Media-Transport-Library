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

typedef struct mtlSt22pMuxerContext {
  const AVClass *class; /**< Class for private options. */

  int idx;
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StTxSessionPortArgs portArgs;
  /* arguments for session */
  char *codec_str;
  int fb_cnt;
  float bpp;
  int codec_thread_cnt;
  int width;
  int height;
  enum AVPixelFormat pixel_format;
  AVRational framerate;
  mtl_handle dev_handle;
  st22p_tx_handle tx_handle;

  int64_t frame_counter;
  int frame_size;
} mtlSt22pMuxerContext;

static int mtl_st22p_write_close(AVFormatContext *ctx) {
  mtlSt22pMuxerContext *s = ctx->priv_data;

  dbg("%s(%d), start\n", __func__, s->idx);
  // Destroy tx session
  if (s->tx_handle) {
    st22p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    dbg(ctx, "%s(%d), st22p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st22p_write_header(AVFormatContext *ctx) {
  mtlSt22pMuxerContext *s = ctx->priv_data;
  struct st22p_tx_ops ops_tx;
  int ret;

  memset(&ops_tx, 0, sizeof(ops_tx));

  ret = mtl_parse_tx_port(ctx, &s->devArgs, &s->portArgs, &ops_tx.port);
  if (ret < 0) {
    err(ctx, "%s, parse tx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
  ops_tx.pack_type = ST22_PACK_CODESTREAM;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;

  if (!s->codec_str) {
    ops_tx.codec = ST22_CODEC_JPEGXS;
  } else {
    ops_tx.codec = st_name_to_codec(s->codec_str);
    if (ST22_CODEC_MAX == ops_tx.codec) {
      err(ctx, "%s, unknow codec str %s\n", __func__, s->codec_str);
      return AVERROR(EIO);
    }
  }

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
  /* transport_fmt is hardcode now */
  switch (s->pixel_format) {
    case AV_PIX_FMT_YUV422P10LE:
      ops_tx.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
      break;
    case AV_PIX_FMT_RGB24:
      ops_tx.input_fmt = ST_FRAME_FMT_RGB8;
      break;
    case AV_PIX_FMT_YUV420P:
      ops_tx.input_fmt = ST_FRAME_FMT_YUV420PLANAR8;
      break;
    default:
      err(ctx, "%s, unsupported pixel format: %d\n", __func__, s->pixel_format);
      return AVERROR(EINVAL);
  }

  ops_tx.name = "st22p_ffmpeg";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  dbg(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_tx.framebuff_cnt = s->fb_cnt;
  ops_tx.codec_thread_cnt = s->codec_thread_cnt;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  s->tx_handle = st22p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st22p_tx_create failed\n", __func__);
    mtl_st22p_write_close(ctx);
    return AVERROR(EIO);
  }

  ret = mtl_start(s->dev_handle);
  if (ret < 0) {
    err(ctx, "%s, mtl start fail %d\n", __func__, ret);
    mtl_st22p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->frame_size = st22p_tx_frame_size(s->tx_handle);
  info(ctx, "%s(%d), tx_handle %p\n", __func__, s->idx, s->tx_handle);
  return 0;
}

static int mtl_st22_write_header(AVFormatContext *ctx) {
  mtlSt22pMuxerContext *s = ctx->priv_data;
  struct st22p_tx_ops ops_tx;
  int ret;
  enum AVCodecID codec_id;
  const AVCodecDescriptor *codec_desc;

  memset(&ops_tx, 0, sizeof(ops_tx));

  ret = mtl_parse_tx_port(ctx, &s->devArgs, &s->portArgs, &ops_tx.port);
  if (ret < 0) {
    err(ctx, "%s, parse tx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.flags |= ST22P_TX_FLAG_BLOCK_GET;
  ops_tx.pack_type = ST22_PACK_CODESTREAM;
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;

  codec_id = ctx->streams[0]->codecpar->codec_id;
  codec_desc = avcodec_descriptor_get(codec_id);
  if (!codec_desc) {
    err(ctx, "%s, get codec_desc fail, codec_id %d\n", __func__, codec_id);
    return AVERROR(EIO);
  }
  info(ctx, "%s, codec %s\n", __func__, codec_desc->name);
  if (codec_id == AV_CODEC_ID_H264) {
    ops_tx.codec = ST22_CODEC_H264;
    ops_tx.input_fmt = ST_FRAME_FMT_H264_CODESTREAM;
  } else if (codec_id == AV_CODEC_ID_H265) {
    ops_tx.codec = ST22_CODEC_H265;
    ops_tx.input_fmt = ST_FRAME_FMT_H265_CODESTREAM;
  } else if (!strcmp(codec_desc->name, "jpegxs")) {
    ops_tx.codec = ST22_CODEC_JPEGXS;
    ops_tx.input_fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else {
    err(ctx, "%s, unsupported codec %s\n", __func__, codec_desc->name);
    return AVERROR(EIO);
  }

  ops_tx.width = s->width = ctx->streams[0]->codecpar->width;
  ops_tx.height = s->height = ctx->streams[0]->codecpar->height;
  s->framerate = ctx->streams[0]->avg_frame_rate;
  ops_tx.fps = framerate_to_st_fps(s->framerate);
  if (ops_tx.fps == ST_FPS_MAX) {
    err(ctx, "%s, frame rate %0.2f is not supported\n", __func__, av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  s->pixel_format = ctx->streams[0]->codecpar->format;

  ops_tx.name = "st22_ffmpeg";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  ops_tx.device = ST_PLUGIN_DEVICE_AUTO;
  dbg(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_tx.framebuff_cnt = s->fb_cnt;
  ops_tx.codec_thread_cnt = s->codec_thread_cnt;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  ret = mtl_start(s->dev_handle);
  if (ret < 0) {
    err(ctx, "%s, mtl start fail %d\n", __func__, ret);
    mtl_st22p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->tx_handle = st22p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st22p_tx_create failed\n", __func__);
    mtl_st22p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->frame_size = st22p_tx_frame_size(s->tx_handle);
  info(ctx, "%s(%d), tx_handle %p\n", __func__, s->idx, s->tx_handle);
  return 0;
}

static int mtl_st22p_write_packet(AVFormatContext *ctx, AVPacket *pkt) {
  mtlSt22pMuxerContext *s = ctx->priv_data;
  struct st_frame *frame;

  if (pkt->size != s->frame_size) {
    err(ctx, "%s(%d), unexpected pkt size: %d (%d expected)\n", __func__, s->idx,
        pkt->size, s->frame_size);
    return AVERROR(EIO);
  }

  dbg("%s(%d), start\n", __func__, s->idx);
  frame = st22p_tx_get_frame(s->tx_handle);
  if (!frame) {
    info(ctx, "%s(%d), st22p_tx_get_frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s(%d), st22p_tx_get_frame: %p\n", __func__, s->idx, frame);
  /* todo: zero copy with external frame mode */
  mtl_memcpy(frame->addr[0], pkt->data, s->frame_size);

  st22p_tx_put_frame(s->tx_handle, frame);
  s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st22_write_packet(AVFormatContext *ctx, AVPacket *pkt) {
  mtlSt22pMuxerContext *s = ctx->priv_data;
  struct st_frame *frame;

  if (pkt->size > s->frame_size) {
    err(ctx, "%s(%d), invalid pkt size: %d (max %d)\n", __func__, s->idx, pkt->size,
        s->frame_size);
    return AVERROR(EIO);
  }

  dbg("%s(%d), start\n", __func__, s->idx);
  frame = st22p_tx_get_frame(s->tx_handle);
  if (!frame) {
    info(ctx, "%s(%d), st22p_tx_get_frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s(%d), st22p_tx_get_frame: %p\n", __func__, s->idx, frame);
  mtl_memcpy(frame->addr[0], pkt->data, pkt->size);
  frame->data_size = pkt->size;

  st22p_tx_put_frame(s->tx_handle, frame);
  s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(mtlSt22pMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st22p_tx_options[] = {
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
    {"bpp", "bit per pixel", OFFSET(bpp), AV_OPT_TYPE_FLOAT, {.dbl = 3.0}, 0.1, 8.0, ENC},
    {"codec_thread_cnt",
     "Codec threads count",
     OFFSET(codec_thread_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     64,
     ENC},
    {"st22_codec",
     "st22 codec",
     OFFSET(codec_str),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = ENC},
    {NULL},
};

static const AVClass mtl_st22p_muxer_class = {
    .class_name = "mtl_st22p muxer",
    .item_name = av_default_item_name,
    .option = mtl_st22p_tx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st22p_muxer = {
    .name = "mtl_st22p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st22p output device"),
    .priv_data_size = sizeof(mtlSt22pMuxerContext),
    .write_header = mtl_st22p_write_header,
    .write_packet = mtl_st22p_write_packet,
    .write_trailer = mtl_st22p_write_close,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .priv_class = &mtl_st22p_muxer_class,
};

AVOutputFormat ff_mtl_st22_muxer = {
    .name = "mtl_st22",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st22 raw output device"),
    .priv_data_size = sizeof(mtlSt22pMuxerContext),
    .write_header = mtl_st22_write_header,
    .write_packet = mtl_st22_write_packet,
    .write_trailer = mtl_st22p_write_close,
    .video_codec = AV_CODEC_ID_RAWVIDEO,
    .flags = AVFMT_NOFILE,
    .priv_class = &mtl_st22p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st22p_muxer = {
    .p.name = "mtl_st22p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st22p output device"),
    .priv_data_size = sizeof(mtlSt22pMuxerContext),
    .write_header = mtl_st22p_write_header,
    .write_packet = mtl_st22p_write_packet,
    .write_trailer = mtl_st22p_write_close,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st22p_muxer_class,
};

const FFOutputFormat ff_mtl_st22_muxer = {
    .p.name = "mtl_st22",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st22 raw output device"),
    .priv_data_size = sizeof(mtlSt22pMuxerContext),
    .write_header = mtl_st22_write_header,
    .write_packet = mtl_st22_write_packet,
    .write_trailer = mtl_st22p_write_close,
    .p.video_codec = AV_CODEC_ID_RAWVIDEO,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st22p_muxer_class,
};
#endif
