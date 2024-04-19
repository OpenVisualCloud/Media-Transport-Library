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
  /* arguments */
  char* port;
  char* local_addr;
  char* rx_addr;
  int udp_port;
  int payload_type;
  int fb_cnt;
  int session_cnt;
  // st30p arguments
  int sample_rate;
  int channels;
  enum st30_fmt fmt;
  char* fmt_str;
  enum st30_ptime ptime;
  char* ptime_str;
  enum AVCodecID codec_id;

  mtl_handle dev_handle;
  st30p_rx_handle rx_handle;

  int frame_size;
  int64_t frame_counter;
} MtlSt30pDemuxerContext;

static int mtl_st30p_read_close(AVFormatContext* ctx) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;

  dbg("%s, start\n", __func__);
  // Destroy rx session
  if (s->rx_handle) {
    st30p_rx_free(s->rx_handle);
    s->rx_handle = NULL;
    info(ctx, "%s(%d), st30p_rx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), succ\n", __func__, s->idx);
  return 0;
}

static int mtl_st30p_read_header(AVFormatContext* ctx) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;
  struct st30p_rx_ops ops_rx;
  AVStream* st = NULL;

  dbg("%s, start\n", __func__);
  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.flags |= ST30P_RX_FLAG_BLOCK_GET;

  if (!s->port) {
    err(ctx, "%s, port NULL\n", __func__);
    return AVERROR(EINVAL);
  }
  if (strlen(s->port) > MTL_PORT_MAX_LEN) {
    err(ctx, "%s, port %s too long\n", __func__, s->port);
    return AVERROR(EINVAL);
  }
  ops_rx.port.num_port = 1;
  snprintf(ops_rx.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", s->port);

  if (!s->rx_addr) {
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
  s->frame_size = st30_calculate_framebuff_size(ops_rx.fmt, ops_rx.ptime, ops_rx.sampling,
                                                ops_rx.channel, 10 * NS_PER_MS, NULL);

  st = avformat_new_stream(ctx, NULL);
  if (!st) {
    err(ctx, "%s, avformat_new_stream fail\n", __func__);
    return AVERROR(ENOMEM);
  }

  st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  st->codecpar->codec_id = s->codec_id;
  st->codecpar->sample_rate = s->sample_rate;
  st->codecpar->ch_layout.nb_channels = s->channels;
  st->codecpar->frame_size = s->frame_size;
  /* todo: pts */
  avpriv_set_pts_info(st, 64, 1, 1000000);

  // get mtl instance
  s->dev_handle =
      mtl_instance_get(s->port, s->local_addr, 0, s->session_cnt, NULL, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl_instance_get fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_rx.name = "st30p_rx";
  ops_rx.priv = s;  // Handle of priv_data registered to lib
  info(ctx, "%s, fb_cnt: %d\n", __func__, s->fb_cnt);
  ops_rx.framebuff_cnt = s->fb_cnt;
  /* set frame size to 10ms time */
  ops_rx.framebuff_size = s->frame_size;

  s->rx_handle = st30p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    err(ctx, "%s, st30p_rx_create failed\n", __func__);
    mtl_st30p_read_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), st30p_rx_create succ %p\n", __func__, s->idx, s->rx_handle);
  s->frame_counter = 0;
  return 0;
}

static int mtl_st30p_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  MtlSt30pDemuxerContext* s = ctx->priv_data;
  int ret = 0;
  struct st30_frame* frame;

  dbg("%s, start\n", __func__);
  frame = st30p_rx_get_frame(s->rx_handle);
  if (!frame) {
    info(ctx, "%s, st30p_rx_get_frame timeout\n", __func__);
    return AVERROR(EIO);
  }
  dbg(ctx, "%s, st30p_rx_get_frame: %p\n", __func__, frame);
  if (frame->data_size != s->frame_size) {
    err(ctx, "%s(%d), unexpected frame size received: %" PRId64 " (%u expected)\n",
        __func__, s->idx, frame->data_size, s->frame_size);
    st30p_rx_put_frame(s->rx_handle, frame);
    return AVERROR(EIO);
  }

  ret = av_new_packet(pkt, s->frame_size);
  if (ret != 0) {
    err(ctx, "%s, av_new_packet failed with %d\n", __func__, ret);
    st30p_rx_put_frame(s->rx_handle, frame);
    return ret;
  }
  /* todo: zero copy with external frame mode */
  mtl_memcpy(pkt->data, frame->addr, s->frame_size);
  st30p_rx_put_frame(s->rx_handle, frame);

  pkt->pts = s->frame_counter;
  s->frame_counter++;
  dbg(ctx, "%s, frame pts %" PRId64 "\n", __func__, pkt->pts);
  return 0;
}

#define OFFSET(x) offsetof(MtlSt30pDemuxerContext, x)
#define DEC (AV_OPT_FLAG_DECODING_PARAM)
static const AVOption mtl_st30p_rx_options[] = {
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
     {.i64 = 30000},
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
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 3},
     3,
     8,
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
    {"pf",
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
    {"total_sessions",
     "Total sessions count",
     OFFSET(session_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     INT_MAX,
     DEC},
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
