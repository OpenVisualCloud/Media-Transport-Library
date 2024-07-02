/*
 * mtl st2110-30 demuxer
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

typedef struct MtlSt30pDemuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StRxSessionPortArgs portArgs;
  /* arguments for session */
  int fb_cnt;
  int timeout_sec;
  int session_init_retry;
  int sample_rate;
  int channels;
  enum st30_fmt fmt;
  char* fmt_str;
  enum st30_ptime ptime;
  char* ptime_str;
  enum AVCodecID codec_id;

  mtl_handle dev_handle;
  st30p_rx_handle rx_handle;

  int64_t frame_counter;
} MtlSt30pDemuxerContext;

static int mtl_st30p_read_close(AVFormatContext* ctx) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;

  dbg("%s(%d), start\n", __func__, s->idx);
  // Destroy rx session
  if (s->rx_handle) {
    st30p_rx_free(s->rx_handle);
    s->rx_handle = NULL;
    dbg(ctx, "%s(%d), st30p_rx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st30p_read_header(AVFormatContext* ctx) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;
  struct st30p_rx_ops ops_rx;
  AVStream* st = NULL;
  int ret;
  int frame_buf_size;

  dbg("%s, start\n", __func__);
  memset(&ops_rx, 0, sizeof(ops_rx));

  ret = mtl_parse_rx_port(ctx, &s->devArgs, &s->portArgs, &ops_rx.port);
  if (ret < 0) {
    err(ctx, "%s, parse rx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_rx.flags |= ST30P_RX_FLAG_BLOCK_GET;
  if (!s->fmt_str) {
    s->fmt = ST30_FMT_PCM24;
    s->codec_id = AV_CODEC_ID_PCM_S24BE;
  } else {
    if (0 == strcmp(s->fmt_str, "pcm24")) {
      s->fmt = ST30_FMT_PCM24;
      s->codec_id = AV_CODEC_ID_PCM_S24BE;
    } else if (0 == strcmp(s->fmt_str, "pcm16")) {
      s->fmt = ST30_FMT_PCM16;
      s->codec_id = AV_CODEC_ID_PCM_S16BE;
    } else if (0 == strcmp(s->fmt_str, "pcm8")) {
      s->fmt = ST30_FMT_PCM8;
      s->codec_id = AV_CODEC_ID_PCM_S8;
    } else {
      err(ctx, "%s, invalid fmt_str: %s\n", __func__, s->fmt_str);
      return AVERROR(EINVAL);
    }
  }
  ops_rx.fmt = s->fmt;
  if (!s->ptime_str) {
    s->ptime = ST30_PTIME_1MS;
  } else {
    if (0 == strcmp(s->ptime_str, "1ms"))
      s->ptime = ST30_PTIME_1MS;
    else if (0 == strcmp(s->ptime_str, "125us"))
      s->ptime = ST30_PTIME_125US;
    else {
      err(ctx, "%s, invalid ptime_str: %s\n", __func__, s->ptime_str);
      return AVERROR(EINVAL);
    }
  }
  ops_rx.ptime = s->ptime;
  ops_rx.channel = s->channels;
  if (s->sample_rate == 48000)
    ops_rx.sampling = ST30_SAMPLING_48K;
  else if (s->sample_rate == 96000)
    ops_rx.sampling = ST30_SAMPLING_96K;
  else if (s->sample_rate == 44000)
    ops_rx.sampling = ST31_SAMPLING_44K;
  else {
    err(ctx, "%s, invalid sample_rate: %d\n", __func__, s->sample_rate);
    return AVERROR(EINVAL);
  }
  frame_buf_size = st30_calculate_framebuff_size(
      ops_rx.fmt, ops_rx.ptime, ops_rx.sampling, ops_rx.channel, 10 * NS_PER_MS, NULL);
  ctx->packet_size = frame_buf_size;

  st = avformat_new_stream(ctx, NULL);
  if (!st) {
    err(ctx, "%s, avformat_new_stream fail\n", __func__);
    return AVERROR(ENOMEM);
  }

  st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  st->codecpar->codec_id = s->codec_id;
  st->codecpar->sample_rate = s->sample_rate;
#ifdef MTL_FFMPEG_4_4
  st->codecpar->channels = s->channels;
#else
  st->codecpar->ch_layout.nb_channels = s->channels;
#endif
  st->codecpar->frame_size = frame_buf_size;
  /* pts with 10ms */
  avpriv_set_pts_info(st, 64, 1, 100);

  ops_rx.name = "st30p_rx_ffmpeg";
  ops_rx.priv = s;  // Handle of priv_data registered to lib
  ops_rx.framebuff_cnt = s->fb_cnt;
  /* set frame size to 10ms time */
  ops_rx.framebuff_size = frame_buf_size;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  s->rx_handle = st30p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    err(ctx, "%s, st30p_rx_create failed\n", __func__);
    mtl_st30p_read_close(ctx);
    return AVERROR(EIO);
  }

  if (s->timeout_sec)
    st30p_rx_set_block_timeout(s->rx_handle, s->timeout_sec * (uint64_t)NS_PER_S);

  frame_buf_size = st30p_rx_frame_size(s->rx_handle);
  if (frame_buf_size != ctx->packet_size) {
    err(ctx, "%s, frame size mismatch %d:%u\n", __func__, frame_buf_size,
        ctx->packet_size);
    mtl_st30p_read_close(ctx);
    return AVERROR(EIO);
  }

  ret = mtl_start(s->dev_handle);
  if (ret < 0) {
    err(ctx, "%s, mtl start fail %d\n", __func__, ret);
    mtl_st30p_read_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), rx handle %p\n", __func__, s->idx, s->rx_handle);
  return 0;
}

static int mtl_st30p_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;
  int ret = 0;
  struct st30_frame* frame;

  dbg("%s(%d), start\n", __func__, s->idx);

  if (0 == s->frame_counter) {
    /*
     * for unicast scenarios, retries may be necessary
     * if the transmitter is not yet initialized.
     */
    for (int i = 1; i <= s->session_init_retry; i++) {
      frame = st30p_rx_get_frame(s->rx_handle);
      if (frame) break;
      info(ctx, "%s(%d) session initialization retry %d\n", __func__, s->idx, i);
    }
  } else
    frame = st30p_rx_get_frame(s->rx_handle);

  if (!frame) {
    info(ctx, "%s(%d), st30p_rx_get_frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s, st30p_rx_get_frame: %p\n", __func__, frame);
  if (frame->data_size != ctx->packet_size) {
    err(ctx, "%s(%d), unexpected frame size received: %" PRId64 " (%u expected)\n",
        __func__, s->idx, frame->data_size, ctx->packet_size);
    st30p_rx_put_frame(s->rx_handle, frame);
    return AVERROR(EIO);
  }

  ret = av_new_packet(pkt, ctx->packet_size);
  if (ret != 0) {
    err(ctx, "%s, av_new_packet failed with %d\n", __func__, ret);
    st30p_rx_put_frame(s->rx_handle, frame);
    return ret;
  }
  /* todo: zero copy with external frame mode */
  mtl_memcpy(pkt->data, frame->addr, ctx->packet_size);
  st30p_rx_put_frame(s->rx_handle, frame);

  pkt->pts = pkt->dts = s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, pkt->pts);
  return 0;
}

#define OFFSET(x) offsetof(MtlSt30pDemuxerContext, x)
#define DEC (AV_OPT_FLAG_DECODING_PARAM)
static const AVOption mtl_st30p_rx_options[] = {
    // mtl dev port info
    MTL_RX_DEV_ARGS,
    // mtl rx port info
    MTL_RX_PORT_ARGS,
    // session info
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
     DEC},
    {"timeout_s",
     "Frame get timeout in seconds",
     OFFSET(timeout_sec),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     60 * 10,
     DEC},
    {"init_retry",
     "Number of retries to the initial read packet",
     OFFSET(session_init_retry),
     AV_OPT_TYPE_INT,
     {.i64 = 5},
     0,
     60,
     DEC},
    {"ar",
     "audio sampling rate",
     OFFSET(sample_rate),
     AV_OPT_TYPE_INT,
     {.i64 = 48000},
     1,
     INT_MAX,
     DEC},
    {"ac",
     "audio channel",
     OFFSET(channels),
     AV_OPT_TYPE_INT,
     {.i64 = 2},
     1,
     INT_MAX,
     DEC},
    {"pcm_fmt",
     "audio pcm format",
     OFFSET(fmt_str),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {"at",
     "audio packet time",
     OFFSET(ptime_str),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {NULL},
};

static const AVClass mtl_st30p_demuxer_class = {
    .class_name = "mtl_30p demuxer",
    .item_name = av_default_item_name,
    .option = mtl_st30p_rx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

#ifndef MTL_FFMPEG_4_4
const AVInputFormat ff_mtl_st30p_demuxer =
#else
AVInputFormat ff_mtl_st30p_demuxer =
#endif
    {
        .name = "mtl_st30p",
        .long_name = NULL_IF_CONFIG_SMALL("mtl st30p input device"),
        .priv_data_size = sizeof(MtlSt30pDemuxerContext),
        .read_header = mtl_st30p_read_header,
        .read_packet = mtl_st30p_read_packet,
        .read_close = mtl_st30p_read_close,
        .flags = AVFMT_NOFILE,
        .extensions = "mtl",
        .priv_class = &mtl_st30p_demuxer_class,
};
