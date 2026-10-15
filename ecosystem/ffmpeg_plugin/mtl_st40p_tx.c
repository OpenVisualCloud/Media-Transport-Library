/*
 * mtl st2110-40 ancillary and st2110-41 fast metadata muxer
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

/* one ANC packet per frame, its Data Count is 8 bits */
#define MTL_ST40P_ANC_MAX_UDW 255
/* the largest data item of st40p_tx_put_frame, see ST40P_TX_FLAG_FAST_METADATA */
#define MTL_ST40P_FMD_MAX_ITEM 1436

typedef struct mtlSt40pMuxerContext {
  const AVClass* class; /**< Class for private options. */

  int idx;
  /* arguments for devices */
  StDevArgs devArgs;
  /* arguments for session port */
  StTxSessionPortArgs portArgs;
  /* arguments for session */
  int fb_cnt;
  AVRational framerate;
  int did;
  int sdid;
  int fast_metadata;
  int fmd_dit;
  int fmd_k_bit;

  mtl_handle dev_handle;
  st40p_tx_handle tx_handle;

  int64_t frame_counter;
} mtlSt40pMuxerContext;

static int mtl_st40p_write_close(AVFormatContext* ctx) {
  mtlSt40pMuxerContext* s = ctx->priv_data;

  // Destroy tx session, it sends the frames in flight first
  if (s->tx_handle) {
    st40p_tx_free(s->tx_handle);
    s->tx_handle = NULL;
    dbg(ctx, "%s(%d), st40p_tx_free succ\n", __func__, s->idx);
  }

  // Destroy device
  if (s->dev_handle) {
    mtl_instance_put(ctx, s->dev_handle);
    s->dev_handle = NULL;
  }

  info(ctx, "%s(%d), frame_counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

static int mtl_st40p_write_header(AVFormatContext* ctx) {
  mtlSt40pMuxerContext* s = ctx->priv_data;
  struct st40p_tx_ops ops_tx;
  int ret;
  AVCodecParameters* codecpar = ctx->streams[0]->codecpar;

  /* the bytes of a data stream or of u8 audio, -f u8 can loop the input */
  if (codecpar->codec_type != AVMEDIA_TYPE_DATA &&
      codecpar->codec_id != AV_CODEC_ID_PCM_U8) {
    err(ctx, "%s, codec_id %d is not data or pcm_u8\n", __func__, codecpar->codec_id);
    return AVERROR(EINVAL);
  }

  memset(&ops_tx, 0, sizeof(ops_tx));

  ret = mtl_parse_tx_port(ctx, &s->devArgs, &s->portArgs, &ops_tx.port);
  if (ret < 0) {
    err(ctx, "%s, parse tx port fail\n", __func__);
    return AVERROR(EIO);
  }

  ops_tx.fps = framerate_to_st_fps(s->framerate);
  if (ops_tx.fps == ST_FPS_MAX) {
    err(ctx, "%s, frame rate %0.2f is not supported\n", __func__, av_q2d(s->framerate));
    return AVERROR(EINVAL);
  }

  ops_tx.flags |= ST40P_TX_FLAG_BLOCK_GET;
  ops_tx.max_udw_buff_size = MTL_ST40P_ANC_MAX_UDW;
  if (s->fast_metadata) {
    ops_tx.flags |= ST40P_TX_FLAG_FAST_METADATA;
    ops_tx.max_udw_buff_size = MTL_ST40P_FMD_MAX_ITEM;
    ops_tx.fmd_dit = s->fmd_dit;
    ops_tx.fmd_k_bit = s->fmd_k_bit;
  }

  ops_tx.name = "st40p_ffmpeg";
  ops_tx.priv = s;  // Handle of priv_data registered to lib
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
    mtl_st40p_write_close(ctx);
    return AVERROR(EIO);
  }

  s->tx_handle = st40p_tx_create(s->dev_handle, &ops_tx);
  if (!s->tx_handle) {
    err(ctx, "%s, st40p_tx_create failed\n", __func__);
    mtl_st40p_write_close(ctx);
    return AVERROR(EIO);
  }

  info(ctx, "%s(%d), tx_handle %p\n", __func__, s->idx, s->tx_handle);
  return 0;
}

static int mtl_st40p_write_packet(AVFormatContext* ctx, AVPacket* pkt) {
  mtlSt40pMuxerContext* s = ctx->priv_data;

  /* one frame for each udw_buffer_size bytes: one ANC packet or one data item */
  for (int off = 0; off < pkt->size;) {
    struct st40_frame_info* frame = st40p_tx_get_frame(s->tx_handle);
    uint32_t size;

    if (!frame) {
      info(ctx, "%s(%d), get frame timeout\n", __func__, s->idx);
      return AVERROR(EIO);
    }

    size = FFMIN((size_t)(pkt->size - off), frame->udw_buffer_size);
    mtl_memcpy(frame->udw_buff_addr, pkt->data + off, size);
    frame->udw_buffer_fill = size;
    if (!s->fast_metadata) {
      memset(&frame->meta[0], 0, sizeof(frame->meta[0]));
      frame->meta[0].did = s->did;
      frame->meta[0].sdid = s->sdid;
      frame->meta[0].udw_size = size;
      frame->meta_num = 1;
    }
    if (st40p_tx_put_frame(s->tx_handle, frame) < 0) {
      err(ctx, "%s(%d), put frame fail\n", __func__, s->idx);
      st40p_tx_put_frame_abort(s->tx_handle, frame);
      return AVERROR(EIO);
    }
    s->frame_counter++;
    off += size;
  }

  dbg(ctx, "%s(%d), frame counter %" PRId64 "\n", __func__, s->idx, s->frame_counter);
  return 0;
}

#define OFFSET(x) offsetof(mtlSt40pMuxerContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption mtl_st40p_tx_options[] = {
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
    {"fps",
     "Frame rate",
     OFFSET(framerate),
     AV_OPT_TYPE_RATIONAL,
     {.dbl = 59.94},
     0,
     1000,
     ENC},
    {"did",
     "ANC Data Identification Word",
     OFFSET(did),
     AV_OPT_TYPE_INT,
     {.i64 = 0x43},
     0,
     0xFF,
     ENC},
    {"sdid",
     "ANC Secondary Data Identification Word",
     OFFSET(sdid),
     AV_OPT_TYPE_INT,
     {.i64 = 0x02},
     0,
     0xFF,
     ENC},
    {"fast_metadata",
     "Send st2110-41 fast metadata",
     OFFSET(fast_metadata),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     ENC},
    {"fmd_dit",
     "Fast metadata Data Item Type",
     OFFSET(fmd_dit),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     0x3FFFFF,
     ENC},
    {"fmd_k_bit",
     "Fast metadata Data Item K-bit",
     OFFSET(fmd_k_bit),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     ENC},
    {NULL},
};

static const AVClass mtl_st40p_muxer_class = {
    .class_name = "mtl_st40p muxer",
    .item_name = av_default_item_name,
    .option = mtl_st40p_tx_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

#ifdef MTL_FFMPEG_4_4
AVOutputFormat ff_mtl_st40p_muxer = {
    .name = "mtl_st40p",
    .long_name = NULL_IF_CONFIG_SMALL("mtl st40p ancillary output device"),
    .audio_codec = AV_CODEC_ID_PCM_U8,
    .priv_data_size = sizeof(mtlSt40pMuxerContext),
    .write_header = mtl_st40p_write_header,
    .write_packet = mtl_st40p_write_packet,
    .write_trailer = mtl_st40p_write_close,
    .flags = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
    .priv_class = &mtl_st40p_muxer_class,
};
#else
const FFOutputFormat ff_mtl_st40p_muxer = {
    .p.name = "mtl_st40p",
    .p.long_name = NULL_IF_CONFIG_SMALL("mtl st40p ancillary output device"),
    .p.audio_codec = AV_CODEC_ID_PCM_U8,
    .priv_data_size = sizeof(mtlSt40pMuxerContext),
    .write_header = mtl_st40p_write_header,
    .write_packet = mtl_st40p_write_packet,
    .write_trailer = mtl_st40p_write_close,
    .p.flags = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
    .p.priv_class = &mtl_st40p_muxer_class,
};
#endif
