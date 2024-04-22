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
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StTxSessionPortArgs portArgs;
  /* arguments for session */
  int fb_cnt;
  int frame_size;

  int filled;
  struct st30_frame* last_frame;

  mtl_handle dev_handle;
  st30p_tx_handle tx_handle;

  int64_t frame_counter;
} mtlSt30pMuxerContext;

static int mtl_st30p_write_close(AVFormatContext* ctx) {
  mtlSt30pMuxerContext* s = ctx->priv_data;

  dbg("%s(%d), start\n", __func__, s->idx);
  // Destroy tx session
  if (s->tx_handle) {
    if (s->last_frame) {
      st30p_tx_put_frame(s->tx_handle, s->last_frame);
      s->last_frame = NULL;
    }
    st30p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    dbg(ctx, "%s(%d), st30p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st30p_write_header(AVFormatContext* ctx) {
  mtlSt30pMuxerContext* s = ctx->priv_data;
  struct st30p_tx_ops ops_tx;
  int ret;

  dbg("%s, start\n", __func__);
  memset(&ops_tx, 0, sizeof(ops_tx));

  ret = mtl_parse_tx_port(ctx, &s->devArgs, &s->portArgs, &ops_tx.port);
  if (ret < 0) {
    err(ctx, "%s, parse tx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.flags |= ST30P_TX_FLAG_BLOCK_GET;
  // todo
  ops_tx.ptime = ST30_PTIME_1MS;
  ops_tx.channel = 2;
  ops_tx.sampling = ST30_SAMPLING_48K;
  ops_tx.fmt = ST30_FMT_PCM24;

  s->frame_size = st30_calculate_framebuff_size(ops_tx.fmt, ops_tx.ptime, ops_tx.sampling,
                                                ops_tx.channel, 10 * NS_PER_MS, NULL);

  ops_tx.name = "st30p_ffmpeg";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
  ops_tx.framebuff_cnt = s->fb_cnt;
  ops_tx.framebuff_size = s->frame_size;

  // get mtl dev
  s->dev_handle = mtl_dev_get(ctx, &s->devArgs, &s->idx);
  if (!s->dev_handle) {
    err(ctx, "%s, mtl dev get fail\n", __func__);
    return AVERROR(EIO);
  }

  s->tx_handle = st30p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st30p_tx_create failed\n", __func__);
    mtl_st30p_write_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), tx_handle %p\n", __func__, s->idx, s->tx_handle);
  return 0;
}

static struct st30_frame* mtl_st30p_fetch_frame(AVFormatContext* ctx,
                                                mtlSt30pMuxerContext* s) {
  if (s->last_frame) {
    return s->last_frame;
  } else {
    struct st30_frame* frame;
    frame = st30p_tx_get_frame(s->tx_handle);
    dbg(ctx, "%s(%d), get frame addr %p\n", __func__, s->idx, frame->addr);
    s->last_frame = frame;
    return frame;
  }
}

static int mtl_st30p_write_packet(AVFormatContext* ctx, AVPacket* pkt) {
  mtlSt30pMuxerContext* s = ctx->priv_data;
  int size = pkt->size;
  uint8_t* data = pkt->data;
  struct st30_frame* frame = mtl_st30p_fetch_frame(ctx, s);

  if (!frame) {
    info(ctx, "%s(%d), fetch frame timeout\n", __func__, s->idx);
    return AVERROR(EIO);
  }

  dbg(ctx, "%s(%d), pkt size %d frame size %d\n", __func__, s->idx, size, s->frame_size);
  while (size > 0) {
    int left = s->frame_size - s->filled;
    uint8_t* cur = (uint8_t*)frame->addr + s->filled;
    dbg(ctx, "%s(%d), size %d left %d filled %d\n", __func__, s->idx, size, left,
        s->filled);

    if (size < left) {
      mtl_memcpy(cur, data, size);
      s->filled += size;
      break;
    } else {
      mtl_memcpy(cur, data, left);
      s->frame_counter++;
      dbg(ctx, "%s(%d), put frame addr %p\n", __func__, s->idx, frame->addr);
      st30p_tx_put_frame(s->tx_handle, frame);
      s->last_frame = NULL;
      frame = mtl_st30p_fetch_frame(ctx, s);
      if (!frame) {
        info(ctx, "%s(%d), fetch frame timeout, size %d\n", __func__, s->idx, size);
        return AVERROR(EIO);
      }
      data += left;
      size -= left;
      s->filled = 0;
    }
  }

  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(mtlSt30pMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st30p_tx_options[] = {
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
     8000,
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

#define DEFAULT_CODEC_ID AV_CODEC_ID_PCM_S24BE

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st30p_muxer = {
    .name = "mtl_st30p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st30p output device"),
    .priv_data_size = sizeof(mtlSt30pMuxerContext),
    .write_header = mtl_st30p_write_header,
    .write_packet = mtl_st30p_write_packet,
    .write_trailer = mtl_st30p_write_close,
    .flags = AVFMT_NOFILE | AVFMT_NOSTREAMS,
    .control_message = NULL,
    .priv_class = &mtl_st30p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st30p_muxer = {
    .p.name = "mtl_st30p_pcm24",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st30p output device"),
    .p.audio_codec = AV_CODEC_ID_PCM_S24BE,
    .priv_data_size = sizeof(mtlSt30pMuxerContext),
    .write_header = mtl_st30p_write_header,
    .write_packet = mtl_st30p_write_packet,
    .write_trailer = mtl_st30p_write_close,
    .p.flags = AVFMT_NOFILE,
    .p.priv_class = &mtl_st30p_muxer_class,
};
#endif
