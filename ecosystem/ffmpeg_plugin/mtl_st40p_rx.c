/*
 * mtl st2110-40 ancillary and st2110-41 fast metadata demuxer
 * Copyright (c) 2026 Intel
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

#include <mtl/st40_pipeline_api.h>

#include "mtl_common.h"

/* ST40_MAX_META ANC packets of 255 bytes, more than one data item */
#define MTL_ST40P_RX_MAX_UDW (ST40_MAX_META * 255)

typedef struct MtlSt40pDemuxerContext {
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
  int fast_metadata;
  char* frame_info_path;

  FILE* frame_info_fp;
  mtl_handle dev_handle;
  st40p_rx_handle rx_handle;

  int64_t frame_counter;
} MtlSt40pDemuxerContext;

static int mtl_st40p_read_close(AVFormatContext* ctx) {
  MtlSt40pDemuxerContext* s = ctx->priv_data;

  // Destroy rx session
  if (s->rx_handle) {
    st40p_rx_free(s->rx_handle);
    s->rx_handle = NULL;
    dbg(ctx, "%s(%d), st40p_rx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  if (s->frame_info_fp) {
    fclose(s->frame_info_fp);
    s->frame_info_fp = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st40p_read_header(AVFormatContext* ctx) {
  MtlSt40pDemuxerContext* s = ctx->priv_data;
  struct st40p_rx_ops ops_rx;
  AVStream* st = NULL;
  int ret;

  memset(&ops_rx, 0, sizeof(ops_rx));

  ret = mtl_parse_rx_port(ctx, &s->devArgs, &s->portArgs, &ops_rx.port);
  if (ret < 0) {
    err(ctx, "%s, parse rx port fail\n", __func__);
    return AVERROR(EIO);
  }

  st = avformat_new_stream(ctx, NULL);
  if (!st) {
    err(ctx, "%s, avformat_new_stream fail\n", __func__);
    return AVERROR(ENOMEM);
  }

  st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
  st->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
  /* pts is the RTP timestamp */
  avpriv_set_pts_info(st, 32, 1, 90000);

  ops_rx.flags |= ST40P_RX_FLAG_BLOCK_GET;
  if (s->fast_metadata) ops_rx.flags |= ST40P_RX_FLAG_FAST_METADATA;
  ops_rx.name = "st40p_rx_ffmpeg";
  ops_rx.priv = s;  // Handle of priv_data registered to lib
  ops_rx.framebuff_cnt = s->fb_cnt;
  ops_rx.max_udw_buff_size = MTL_ST40P_RX_MAX_UDW;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  s->rx_handle = st40p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    err(ctx, "%s, st40p_rx_create failed\n", __func__);
    mtl_st40p_read_close(ctx);
    return AVERROR(EIO);
  }

  if (s->timeout_sec)
    st40p_rx_set_block_timeout(s->rx_handle, s->timeout_sec * (uint64_t)NS_PER_S);

  if (s->frame_info_path) {
    s->frame_info_fp = fopen(s->frame_info_path, "w");
    if (!s->frame_info_fp) {
      err(ctx, "%s, open %s fail\n", __func__, s->frame_info_path);
      mtl_st40p_read_close(ctx);
      return AVERROR(EIO);
    }
  }

  ret = mtl_start(s->dev_handle);
  if (ret < 0) {
    err(ctx, "%s, mtl start fail %d\n", __func__, ret);
    mtl_st40p_read_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), rx handle %p\n", __func__, s->idx, s->rx_handle);
  return 0;
}

/* the frame-info-path line of the GStreamer mtl_st40p_rx, for the same validator */
static void mtl_st40p_log_frame_info(MtlSt40pDemuxerContext* s,
                                     const struct st40_frame_info* frame) {
  fprintf(s->frame_info_fp,
          "ts=%" PRIu64
          " meta=%u rtp_marker=%u seq_discont=%u seq_lost=%u pkts_total=%u "
          "pkts_recv_p=%u pkts_recv_r=%u",
          frame->timestamp, frame->meta_num, frame->rtp_marker, frame->seq_discont,
          frame->seq_lost, frame->pkts_total, frame->pkts_recv[MTL_SESSION_PORT_P],
          frame->pkts_recv[MTL_SESSION_PORT_R]);
  if (s->fast_metadata)
    fprintf(s->frame_info_fp, " fmd_dit=%u fmd_k_bit=%u", frame->fmd_dit,
            frame->fmd_k_bit);
  fputc('\n', s->frame_info_fp);
  fflush(s->frame_info_fp);
}

static int mtl_st40p_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  MtlSt40pDemuxerContext* s = ctx->priv_data;
  struct st40_frame_info* frame = NULL;
  /*
   * for unicast scenarios, retries may be necessary
   * if the transmitter is not yet initialized.
   */
  int tries = s->frame_counter ? 1 : s->session_init_retry + 1;
  int ret;

  while (!frame && tries--) frame = st40p_rx_get_frame(s->rx_handle);
  if (!frame) {
    info(ctx, "%s(%d), st40p_rx_get_frame timeout\n", __func__, s->idx);
    /* a sender that stops ends the stream, one that never starts is an error */
    return s->frame_counter ? AVERROR_EOF : AVERROR(EIO);
  }

  if (s->frame_info_fp) mtl_st40p_log_frame_info(s, frame);

  /* the UDW of all ANC packets, or one data item with its zero padding */
  ret = av_new_packet(pkt, frame->udw_buffer_fill);
  if (ret != 0) {
    err(ctx, "%s, av_new_packet failed with %d\n", __func__, ret);
    st40p_rx_put_frame(s->rx_handle, frame);
    return ret;
  }
  mtl_memcpy(pkt->data, frame->udw_buff_addr, frame->udw_buffer_fill);
  pkt->pts = pkt->dts = frame->rtp_timestamp;
  st40p_rx_put_frame(s->rx_handle, frame);

  s->frame_counter++;
  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(MtlSt40pDemuxerContext, x)
#define DEC (AV_OPT_FLAG_DECODING_PARAM)
static const AVOption mtl_st40p_rx_options[] = {
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
     64,
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
    {"fast_metadata",
     "Receive st2110-41 fast metadata",
     OFFSET(fast_metadata),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     DEC},
    {"frame_info_path",
     "File to write one line of info per frame",
     OFFSET(frame_info_path),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {NULL},
};

static const AVClass mtl_st40p_demuxer_class = {
    .class_name = "mtl_st40p demuxer",
    .item_name = av_default_item_name,
    .option = mtl_st40p_rx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

#ifdef MTL_FFMPEG_7_0
const FFInputFormat ff_mtl_st40p_demuxer = {
    .p.name = "mtl_st40p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st40p ancillary input device"),
    .priv_data_size = sizeof(MtlSt40pDemuxerContext),
    .read_header = mtl_st40p_read_header,
    .read_packet = mtl_st40p_read_packet,
    .read_close = mtl_st40p_read_close,
    .p.flags = AVFMT_NOFILE,
    .p.extensions = "mtl",
    .p.priv_class = &mtl_st40p_demuxer_class,
};
#else  // MTL_FFMPEG_7_0
#ifndef MTL_FFMPEG_4_4
const AVInputFormat ff_mtl_st40p_demuxer =
#else
AVInputFormat ff_mtl_st40p_demuxer =
#endif
    {
        .name = "mtl_st40p",
        .long_name = NULL_IF_CONFIG_SMALL("mtl st40p ancillary input device"),
        .priv_data_size = sizeof(MtlSt40pDemuxerContext),
        .read_header = mtl_st40p_read_header,
        .read_packet = mtl_st40p_read_packet,
        .read_close = mtl_st40p_read_close,
        .flags = AVFMT_NOFILE,
        .extensions = "mtl",
        .priv_class = &mtl_st40p_demuxer_class,
};
#endif  // MTL_FFMPEG_7_0
