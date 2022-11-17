/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st22_pipeline_tx.h"

#include "../../mt_log.h"

static const char* st22p_tx_frame_stat_name[ST22P_TX_FRAME_STATUS_MAX] = {
    "free", "in_user", "ready", "in_encoding", "encoded", "in_trans",
};

static const char* tx_st22p_stat_name(enum st22p_tx_frame_status stat) {
  return st22p_tx_frame_stat_name[stat];
}

static uint16_t tx_st22p_next_idx(struct st22p_tx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static struct st22p_tx_frame* tx_st22p_next_available(
    struct st22p_tx_ctx* ctx, uint16_t idx_start, enum st22p_tx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st22p_tx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = tx_st22p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int tx_st22p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st22_tx_frame_meta* meta) {
  struct st22p_tx_ctx* ctx = priv;
  struct st22p_tx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st22p_next_available(ctx, ctx->framebuff_consumer_idx, ST22P_TX_FRAME_ENCODED);
  /* not any encoded frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;
  if (ctx->ops.flags & (ST22P_TX_FLAG_USER_PACING | ST22P_TX_FLAG_USER_TIMESTAMP)) {
    meta->tfmt = framebuff->src.tfmt;
    meta->timestamp = framebuff->src.timestamp;
    dbg("%s(%d), frame %u succ timestamp %lu\n", __func__, ctx->idx, framebuff->idx,
        meta->timestamp);
  }
  meta->codestream_size = framebuff->dst.data_size;
  /* point to next */
  ctx->framebuff_consumer_idx = tx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  return 0;
}

static int tx_st22p_frame_done(void* priv, uint16_t frame_idx,
                               struct st22_tx_frame_meta* meta) {
  struct st22p_tx_ctx* ctx = priv;
  int ret;
  struct st22p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  st_pthread_mutex_lock(&ctx->lock);
  if (ST22P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST22P_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  st_pthread_mutex_unlock(&ctx->lock);

  framebuff->src.tfmt = meta->tfmt;
  framebuff->dst.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->dst.timestamp = meta->timestamp;

  if (ctx->ops.notify_frame_done) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->src);
  }

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ret;
}

static int tx_st22p_notify_event(void* priv, enum st_event event, void* args) {
  struct st22p_tx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static struct st22_encode_frame_meta* tx_st22p_encode_get_frame(void* priv) {
  struct st22p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st22p_next_available(ctx, ctx->framebuff_encode_idx, ST22P_TX_FRAME_READY);
  /* not any free frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_ENCODING;
  /* point to next */
  ctx->framebuff_encode_idx = tx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->encode_frame;
}

static int tx_st22p_encode_put_frame(void* priv, struct st22_encode_frame_meta* frame,
                                     int result) {
  struct st22p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff = frame->priv;
  uint16_t encode_idx = framebuff->idx;
  size_t data_size = frame->dst->data_size;
  size_t max_size = ctx->encode_impl->codestream_max_size;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_TX_FRAME_IN_ENCODING != framebuff->stat) {
    err("%s(%d), frame %u not in encoding %d\n", __func__, idx, encode_idx,
        framebuff->stat);
    return -EIO;
  }

  dbg("%s(%d), frame %u result %d data_size %ld\n", __func__, idx, encode_idx, result,
      data_size);
  if ((result < 0) || (data_size <= 0) || (data_size > max_size)) {
    info("%s(%d), invalid frame %u result %d data_size %" PRIu64 " max_size %" PRIu64
         "\n",
         __func__, idx, encode_idx, result, data_size, max_size);
    framebuff->stat = ST22P_TX_FRAME_FREE;
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
    rte_atomic32_inc(&ctx->stat_encode_fail);
  } else {
    framebuff->stat = ST22P_TX_FRAME_ENCODED;
  }

  return 0;
}

static int tx_st22p_encode_dump(void* priv) {
  struct st22p_tx_ctx* ctx = priv;
  struct st22p_tx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t encode_idx = ctx->framebuff_encode_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  notice("TX_ST22P(%s), p(%d:%s) e(%d:%s) c(%d:%s)\n", ctx->ops_name, producer_idx,
         tx_st22p_stat_name(framebuff[producer_idx].stat), encode_idx,
         tx_st22p_stat_name(framebuff[encode_idx].stat), consumer_idx,
         tx_st22p_stat_name(framebuff[consumer_idx].stat));

  int encode_fail = rte_atomic32_read(&ctx->stat_encode_fail);
  rte_atomic32_set(&ctx->stat_encode_fail, 0);
  if (encode_fail) {
    notice("RX_ST22P(%s), encode fail %d\n", ctx->ops_name, encode_fail);
  }

  return 0;
}

static int tx_st22p_create_transport(struct mtl_main_impl* impl, struct st22p_tx_ctx* ctx,
                                     struct st22p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st22_tx_ops ops_tx;
  st22_tx_handle transport;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = ops->name;
  ops_tx.priv = ctx;
  ops_tx.num_port = RTE_MIN(ops->port.num_port, MTL_PORT_MAX);
  for (int i = 0; i < ops_tx.num_port; i++) {
    memcpy(ops_tx.dip_addr[i], ops->port.dip_addr[i], MTL_IP_ADDR_LEN);
    strncpy(ops_tx.port[i], ops->port.port[i], MTL_PORT_MAX_LEN);
    ops_tx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST22P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_PORT_P][0], &ops->tx_dst_mac[MTL_PORT_P][0], 6);
    ops_tx.flags |= ST22_TX_FLAG_USER_P_MAC;
  }
  if (ops->flags & ST22P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_PORT_R][0], &ops->tx_dst_mac[MTL_PORT_R][0], 6);
    ops_tx.flags |= ST22_TX_FLAG_USER_R_MAC;
  }
  if (ops->flags & ST22P_TX_FLAG_DISABLE_BOXES)
    ops_tx.flags |= ST22_TX_FLAG_DISABLE_BOXES;
  if (ops->flags & ST22P_TX_FLAG_USER_PACING) ops_tx.flags |= ST22_TX_FLAG_USER_PACING;
  if (ops->flags & ST22P_TX_FLAG_USER_TIMESTAMP)
    ops_tx.flags |= ST22_TX_FLAG_USER_TIMESTAMP;
  if (ops->flags & ST22P_TX_FLAG_ENABLE_VSYNC) ops_tx.flags |= ST22_TX_FLAG_ENABLE_VSYNC;
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.width = ops->width;
  ops_tx.height = ops->height;
  ops_tx.fps = ops->fps;
  ops_tx.payload_type = ops->port.payload_type;
  ops_tx.type = ST22_TYPE_FRAME_LEVEL;
  ops_tx.pack_type = ops->pack_type;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  ops_tx.framebuff_max_size = ctx->encode_impl->codestream_max_size;
  ops_tx.get_next_frame = tx_st22p_next_frame;
  ops_tx.notify_frame_done = tx_st22p_frame_done;
  ops_tx.notify_event = tx_st22p_notify_event;
  if (ops->codec != ST22_CODEC_JPEGXS) {
    ops_tx.flags |= ST22_TX_FLAG_DISABLE_BOXES;
  }

  transport = st22_tx_create(impl, &ops_tx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st22p_tx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].dst.addr[0] = st22_tx_get_fb_addr(transport, i);
    frames[i].dst.fmt = ctx->encode_impl->req.req.output_fmt;
    frames[i].dst.buffer_size = ops_tx.framebuff_max_size;
    frames[i].dst.data_size = ops_tx.framebuff_max_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    frames[i].dst.priv = &frames[i];

    frames[i].encode_frame.src = &frames[i].src;
    frames[i].encode_frame.dst = &frames[i].dst;
    frames[i].encode_frame.priv = &frames[i];
  }

  return 0;
}

static int tx_st22p_uinit_src_fbs(struct st22p_tx_ctx* ctx) {
  if (ctx->framebuffs) {
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].src.addr[0]) {
        st_rte_free(ctx->framebuffs[i].src.addr[0]);
        ctx->framebuffs[i].src.addr[0] = NULL;
      }
    }
    st_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int tx_st22p_init_src_fbs(struct mtl_main_impl* impl, struct st22p_tx_ctx* ctx,
                                 struct st22p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = st_socket_id(impl, MTL_PORT_P);
  struct st22p_tx_frame* frames;
  void* src;
  size_t src_size = ctx->src_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = st_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST22P_TX_FRAME_FREE;
    frames[i].idx = i;
    src = st_rte_zmalloc_socket(src_size, soc_id);
    if (!src) {
      err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
      tx_st22p_uinit_src_fbs(ctx);
      return -ENOMEM;
    }
    frames[i].src.addr[0] = src;
    frames[i].src.fmt = ops->input_fmt;
    frames[i].src.buffer_size = src_size;
    frames[i].src.data_size = src_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.priv = &frames[i];
  }

  info("%s(%d), size %ld fmt %d with %u frames\n", __func__, idx, src_size,
       ops->input_fmt, ctx->framebuff_cnt);
  return 0;
}

static int tx_st22p_get_encoder(struct mtl_main_impl* impl, struct st22p_tx_ctx* ctx,
                                struct st22p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st22_get_encoder_request req;

  memset(&req, 0, sizeof(req));
  req.device = ops->device;
  req.req.codestream_size = ops->codestream_size;
  req.req.max_codestream_size = ops->codestream_size;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.input_fmt = ops->input_fmt;
  req.req.output_fmt = ctx->codestream_fmt;
  req.req.quality = ops->quality;
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.req.codec_thread_cnt = ops->codec_thread_cnt;

  req.priv = ctx;
  req.get_frame = tx_st22p_encode_get_frame;
  req.put_frame = tx_st22p_encode_put_frame;
  req.dump = tx_st22p_encode_dump;

  struct st22_encode_session_impl* encode_impl = st22_get_encoder(impl, &req);
  if (!encode_impl) {
    err("%s(%d), get encoder fail\n", __func__, idx);
    return -EINVAL;
  }
  ctx->encode_impl = encode_impl;

  if (!encode_impl->codestream_max_size) {
    err("%s(%d), error codestream size\n", __func__, idx);
    return -EINVAL;
  }

  return 0;
}

struct st_frame* st22p_tx_get_frame(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st22p_next_available(ctx, ctx->framebuff_producer_idx, ST22P_TX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_producer_idx = tx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->src;
}

int st22p_tx_put_frame(st22p_tx_handle handle, struct st_frame* frame) {
  struct st22p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in free %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  framebuff->stat = ST22P_TX_FRAME_READY;
  st22_encode_notify_frame_ready(ctx->encode_impl);
  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);

  return 0;
}

st22p_tx_handle st22p_tx_create(mtl_handle mt, struct st22p_tx_ops* ops) {
  struct mtl_main_impl* impl = mt;
  struct st22p_tx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  size_t src_size;
  enum st_frame_fmt codestream_fmt;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!ops->notify_frame_available) {
    err("%s, pls set notify_frame_available\n", __func__);
    return NULL;
  }

  src_size = st_frame_size(ops->input_fmt, ops->width, ops->height);
  if (!src_size) {
    err("%s(%d), get source size fail\n", __func__, idx);
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

  ctx = st_rte_zmalloc_socket(sizeof(*ctx), st_socket_id(impl, MTL_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->codestream_fmt = codestream_fmt;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = ST22_SESSION_TYPE_PIPELINE_TX;
  ctx->src_size = src_size;
  rte_atomic32_set(&ctx->stat_encode_fail, 0);
  st_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  ctx->ops = *ops;

  /* get one suitable jpegxs encode device */
  ret = tx_st22p_get_encoder(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), get encoder fail %d\n", __func__, idx, ret);
    st22p_tx_free(ctx);
    return NULL;
  }

  /* init fbs */
  ret = tx_st22p_init_src_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st22p_tx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = tx_st22p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st22p_tx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  info("%s(%d), codestream fmt %s, input fmt: %s\n", __func__, idx,
       st_frame_fmt_name(ctx->codestream_fmt), st_frame_fmt_name(ops->input_fmt));

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ctx;
}

int st22p_tx_free(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->encode_impl) {
    st22_put_encoder(impl, ctx->encode_impl);
    ctx->encode_impl = NULL;
  }

  if (ctx->transport) {
    st22_tx_free(ctx->transport);
    ctx->transport = NULL;
  }
  tx_st22p_uinit_src_fbs(ctx);

  st_pthread_mutex_destroy(&ctx->lock);
  st_rte_free(ctx);

  return 0;
}

void* st22p_tx_get_fb_addr(st22p_tx_handle handle, uint16_t idx) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx < 0 || idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].src.addr[0];
}

size_t st22p_tx_frame_size(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->src_size;
}
