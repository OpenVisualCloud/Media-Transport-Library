/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st22_pipeline_rx.h"

#include "../../mt_log.h"

static const char* st22p_rx_frame_stat_name[ST22P_RX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_decoding", "decoded", "in_user",
};

static const char* rx_st22p_stat_name(enum st22p_rx_frame_status stat) {
  return st22p_rx_frame_stat_name[stat];
}

static uint16_t rx_st22p_next_idx(struct st22p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static struct st22p_rx_frame* rx_st22p_next_available(
    struct st22p_rx_ctx* ctx, uint16_t idx_start, enum st22p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st22p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st22p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int rx_st22p_frame_ready(void* priv, void* frame,
                                struct st22_rx_frame_meta* meta) {
  struct st22p_rx_ctx* ctx = priv;
  struct st22p_rx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_producer_idx, ST22P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->src.addr[0] = frame;
  framebuff->src.data_size = meta->frame_total_size;
  framebuff->src.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->dst.tfmt = meta->tfmt;
  /* set dst timestamp to same as src? */
  framebuff->dst.timestamp = meta->timestamp;
  framebuff->stat = ST22P_RX_FRAME_READY;
  /* point to next */
  ctx->framebuff_producer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  st22_decode_notify_frame_ready(ctx->decode_impl);

  return 0;
}

static int rx_st22p_notify_event(void* priv, enum st_event event, void* args) {
  struct st22p_rx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static struct st22_decode_frame_meta* rx_st22p_decode_get_frame(void* priv) {
  struct st22p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_decode_idx, ST22P_RX_FRAME_READY);
  /* not any ready frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_RX_FRAME_IN_DECODING;
  /* point to next */
  ctx->framebuff_decode_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->decode_frame;
}

static int rx_st22p_decode_put_frame(void* priv, struct st22_decode_frame_meta* frame,
                                     int result) {
  struct st22p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff = frame->priv;
  uint16_t decode_idx = framebuff->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_RX_FRAME_IN_DECODING != framebuff->stat) {
    err("%s(%d), frame %u not in decoding %d\n", __func__, idx, decode_idx,
        framebuff->stat);
    return -EIO;
  }

  dbg("%s(%d), frame %u result %d\n", __func__, idx, decode_idx, result);
  if (result < 0) {
    /* free the frame */
    st22_rx_put_framebuff(ctx->transport, framebuff->src.addr[0]);
    framebuff->stat = ST22P_RX_FRAME_FREE;
    rte_atomic32_inc(&ctx->stat_decode_fail);
  } else {
    framebuff->stat = ST22P_RX_FRAME_DECODED;
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
  }

  return 0;
}

static int rx_st22p_decode_dump(void* priv) {
  struct st22p_rx_ctx* ctx = priv;
  struct st22p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t decode_idx = ctx->framebuff_decode_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  notice("RX_ST22P(%s), p(%d:%s) d(%d:%s) c(%d:%s)\n", ctx->ops_name, producer_idx,
         rx_st22p_stat_name(framebuff[producer_idx].stat), decode_idx,
         rx_st22p_stat_name(framebuff[decode_idx].stat), consumer_idx,
         rx_st22p_stat_name(framebuff[consumer_idx].stat));

  int decode_fail = rte_atomic32_read(&ctx->stat_decode_fail);
  rte_atomic32_set(&ctx->stat_decode_fail, 0);
  if (decode_fail) {
    notice("RX_ST22P(%s), decode fail %d\n", ctx->ops_name, decode_fail);
  }

  int busy = rte_atomic32_read(&ctx->stat_busy);
  rte_atomic32_set(&ctx->stat_busy, 0);
  if (busy) {
    notice("RX_ST22P(%s), busy drop frame %d\n", ctx->ops_name, busy);
  }

  return 0;
}

static int rx_st22p_create_transport(struct mtl_main_impl* impl, struct st22p_rx_ctx* ctx,
                                     struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st22_rx_ops ops_rx;
  st22_rx_handle transport;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.sip_addr[i], ops->port.sip_addr[i], MTL_IP_ADDR_LEN);
    strncpy(ops_rx.port[i], ops->port.port[i], MTL_PORT_MAX_LEN);
    ops_rx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST22P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST22_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST22P_RX_FLAG_ENABLE_VSYNC) ops_rx.flags |= ST22_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    ops_rx.flags |= ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.type = ST22_TYPE_FRAME_LEVEL;
  ops_rx.pack_type = ops->pack_type;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.framebuff_max_size = ctx->max_codestream_size;
  ops_rx.notify_frame_ready = rx_st22p_frame_ready;
  ops_rx.notify_event = rx_st22p_notify_event;

  transport = st22_rx_create(impl, &ops_rx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st22p_rx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].src.fmt = ctx->decode_impl->req.req.input_fmt;
    frames[i].src.buffer_size = ops_rx.framebuff_max_size;
    frames[i].src.data_size = ops_rx.framebuff_max_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.priv = &frames[i];

    frames[i].decode_frame.src = &frames[i].src;
    frames[i].decode_frame.dst = &frames[i].dst;
    frames[i].decode_frame.priv = &frames[i];
  }

  return 0;
}

static int rx_st22p_uinit_dst_fbs(struct st22p_rx_ctx* ctx) {
  if (ctx->framebuffs) {
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].dst.addr[0]) {
        mt_rte_free(ctx->framebuffs[i].dst.addr[0]);
        ctx->framebuffs[i].dst.addr[0] = NULL;
      }
    }
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int rx_st22p_init_dst_fbs(struct mtl_main_impl* impl, struct st22p_rx_ctx* ctx,
                                 struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = mt_socket_id(impl, MTL_PORT_P);
  struct st22p_rx_frame* frames;
  void* dst;
  size_t dst_size = ctx->dst_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST22P_RX_FRAME_FREE;
    frames[i].idx = i;
    dst = mt_rte_zmalloc_socket(dst_size, soc_id);
    if (!dst) {
      err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
      rx_st22p_uinit_dst_fbs(ctx);
      return -ENOMEM;
    }
    frames[i].dst.fmt = ops->output_fmt;
    frames[i].dst.buffer_size = dst_size;
    frames[i].dst.data_size = dst_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    frames[i].dst.priv = &frames[i];
    /* init plane */
    st_frame_init_plane_single_src(&frames[i].dst, dst, mtl_hp_virt2iova(ctx->impl, dst));
    /* check plane */
    if (st_frame_sanity_check(&frames[i].dst) < 0) {
      err("%s(%d), dst frame %d sanity check fail\n", __func__, idx, i);
      rx_st22p_uinit_dst_fbs(ctx);
      return -EINVAL;
    }
  }

  info("%s(%d), size %" PRIu64 " fmt %d with %u frames\n", __func__, idx, dst_size,
       ops->output_fmt, ctx->framebuff_cnt);
  return 0;
}

static int rx_st22p_get_decoder(struct mtl_main_impl* impl, struct st22p_rx_ctx* ctx,
                                struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st22_get_decoder_request req;

  memset(&req, 0, sizeof(req));
  req.device = ops->device;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.output_fmt = ops->output_fmt;
  req.req.input_fmt = ctx->codestream_fmt;
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.req.codec_thread_cnt = ops->codec_thread_cnt;
  req.priv = ctx;
  req.get_frame = rx_st22p_decode_get_frame;
  req.put_frame = rx_st22p_decode_put_frame;
  req.dump = rx_st22p_decode_dump;

  struct st22_decode_session_impl* decode_impl = st22_get_decoder(impl, &req);
  if (!decode_impl) {
    err("%s(%d), get decoder fail\n", __func__, idx);
    return -EINVAL;
  }
  ctx->decode_impl = decode_impl;

  return 0;
}

struct st_frame* st22p_rx_get_frame(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_consumer_idx, ST22P_RX_FRAME_DECODED);
  /* not any decoded frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->dst;
}

int st22p_rx_put_frame(st22p_rx_handle handle, struct st_frame* frame) {
  struct st22p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff = frame->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in free %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* free the frame */
  st22_rx_put_framebuff(ctx->transport, framebuff->src.addr[0]);
  framebuff->stat = ST22P_RX_FRAME_FREE;
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);

  return 0;
}

st22p_rx_handle st22p_rx_create(mtl_handle mt, struct st22p_rx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st22p_rx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  size_t dst_size;
  enum st_frame_fmt codestream_fmt;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!ops->notify_frame_available) {
    err("%s, pls set notify_frame_available\n", __func__);
    return NULL;
  }

  dst_size = st_frame_size(ops->output_fmt, ops->width, ops->height, false);
  if (!dst_size) {
    err("%s(%d), get dst size fail\n", __func__, idx);
    return NULL;
  }

  if (ops->codec == ST22_CODEC_JPEGXS) {
    codestream_fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else if (ops->codec == ST22_CODEC_H264_CBR) {
    codestream_fmt = ST_FRAME_FMT_H264_CBR_CODESTREAM;
  } else {
    err("%s(%d), unknow codec %d\n", __func__, idx, ops->codec);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), mt_socket_id(impl, MTL_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->codestream_fmt = codestream_fmt;
  ctx->impl = impl;
  ctx->type = MT_ST22_HANDLE_PIPELINE_RX;
  ctx->dst_size = dst_size;
  /* use the possible max size */
  ctx->max_codestream_size = ops->max_codestream_size;
  if (!ctx->max_codestream_size) ctx->max_codestream_size = dst_size;
  rte_atomic32_set(&ctx->stat_decode_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  ctx->ops = *ops;

  /* get one suitable jpegxs decode device */
  ret = rx_st22p_get_decoder(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), get decoder fail %d\n", __func__, idx, ret);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* init fbs */
  ret = rx_st22p_init_dst_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = rx_st22p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  info("%s(%d), codestream fmt %s, output fmt: %s\n", __func__, idx,
       st_frame_fmt_name(ctx->codestream_fmt), st_frame_fmt_name(ops->output_fmt));

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ctx;
}

int st22p_rx_free(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->decode_impl) {
    st22_put_decoder(impl, ctx->decode_impl);
    ctx->decode_impl = NULL;
  }

  if (ctx->transport) {
    st22_rx_free(ctx->transport);
    ctx->transport = NULL;
  }
  rx_st22p_uinit_dst_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_rte_free(ctx);

  return 0;
}

void* st22p_rx_get_fb_addr(st22p_rx_handle handle, uint16_t idx) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx < 0 || idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].dst.addr[0];
}

size_t st22p_rx_frame_size(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->dst_size;
}

int st22p_rx_get_queue_meta(st22p_rx_handle handle, struct st_queue_meta* meta) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st22_rx_get_queue_meta(ctx->transport, meta);
}

int st22p_rx_pcapng_dump(st22p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st22_rx_pcapng_dump(ctx->transport, max_dump_packets, sync, meta);
}
