/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st22_pipeline_tx.h"

#include "../../mt_log.h"

static const char* st22p_tx_frame_stat_name[ST22P_TX_FRAME_STATUS_MAX] = {
    "free", "in_user", "ready", "in_encoding", "encoded", "in_trans",
};

static const char* st22p_tx_frame_stat_name_short[ST22P_TX_FRAME_STATUS_MAX] = {
    "F", "U", "R", "IE", "E", "T",
};

static const char* tx_st22p_stat_name(enum st22p_tx_frame_status stat) {
  return st22p_tx_frame_stat_name[stat];
}

static inline struct st_frame* tx_st22p_user_frame(struct st22p_tx_ctx* ctx,
                                                   struct st22p_tx_frame* framebuff) {
  return ctx->derive ? &framebuff->dst : &framebuff->src;
}

static void tx_st22p_block_wake(struct st22p_tx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void tx_st22p_notify_frame_available(struct st22p_tx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    tx_st22p_block_wake(ctx);
  }
}

static void tx_st22p_encode_block_wake(struct st22p_tx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->encode_block_wake_mutex);
  mt_pthread_cond_signal(&ctx->encode_block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->encode_block_wake_mutex);
}

static void tx_st22p_encode_notify_frame_ready(struct st22p_tx_ctx* ctx) {
  if (ctx->derive) return; /* no encoder for derive mode */

  struct st22_encode_session_impl* encoder = ctx->encode_impl;
  struct st22_encode_dev_impl* dev_impl = encoder->parent;
  struct st22_encoder_dev* dev = &dev_impl->dev;
  st22_encode_priv session = encoder->session;

  if (dev->notify_frame_available) dev->notify_frame_available(session);

  if (ctx->encode_block_get) {
    /* notify block */
    tx_st22p_encode_block_wake(ctx);
  }
}

static struct st22p_tx_frame* tx_st22p_next_available(
    struct st22p_tx_ctx* ctx, enum st22p_tx_frame_status desired) {
  struct st22p_tx_frame* framebuff;

  /* check ready frame from start */
  for (int idx = 0; idx < ctx->framebuff_cnt; idx++) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      return framebuff;
    }
  }

  /* no any desired frame */
  return NULL;
}

static struct st22p_tx_frame* tx_st22p_newest_available(
    struct st22p_tx_ctx* ctx, enum st22p_tx_frame_status desired) {
  struct st22p_tx_frame* framebuff = NULL;
  struct st22p_tx_frame* framebuff_newest = NULL;

  for (uint16_t idx = 0; idx < ctx->framebuff_cnt; idx++) {
    framebuff = &ctx->framebuffs[idx];
    if ((desired == framebuff->stat &&
         (!framebuff_newest ||
          !mt_seq32_greater(framebuff->seq_number, framebuff_newest->seq_number)))) {
      framebuff_newest = framebuff;
    }
  }

  return framebuff_newest;
}

static int tx_st22p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st22_tx_frame_meta* meta) {
  struct st22p_tx_ctx* ctx = priv;
  struct st22p_tx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st22p_newest_available(ctx, ST22P_TX_FRAME_ENCODED);
  /* not any encoded frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;

  struct st_frame* frame = tx_st22p_user_frame(ctx, framebuff);
  meta->second_field = frame->second_field;
  if (ctx->ops.flags & (ST22P_TX_FLAG_USER_PACING | ST22P_TX_FLAG_USER_TIMESTAMP)) {
    meta->tfmt = frame->tfmt;
    meta->timestamp = frame->timestamp;
    dbg("%s(%d), frame %u succ timestamp %" PRIu64 "\n", __func__, ctx->idx,
        framebuff->idx, meta->timestamp);
  }
  meta->codestream_size = framebuff->dst.data_size;
  mt_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ, frame_idx: %u\n", __func__, ctx->idx, framebuff->idx,
      framebuff->idx);
  MT_USDT_ST22P_TX_FRAME_NEXT(ctx->idx, framebuff->idx);
  return 0;
}

int st22p_tx_late_frame_drop(void* handle, uint64_t epoch_skipped) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;
  struct st22p_tx_frame* framebuff;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (!ctx->ready) return -EBUSY; /* not ready */
  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st22p_newest_available(ctx, ST22P_TX_FRAME_ENCODED);
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST22P_TX_FRAME_FREE;
  ctx->stat_drop_frame++;
  dbg("%s(%d), drop frame %u succ\n", __func__, cidx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_late) {
    ctx->ops.notify_frame_late(ctx->ops.priv, epoch_skipped);
  } else if (ctx->ops.notify_frame_done) {
    ctx->ops.notify_frame_done(ctx->ops.priv, tx_st22p_user_frame(ctx, framebuff));
  }

  /* notify app can get frame */
  tx_st22p_notify_frame_available(ctx);
  MT_USDT_ST22P_TX_FRAME_DONE(ctx->idx, framebuff->idx, framebuff->dst.rtp_timestamp);
  return 0;
}

static int tx_st22p_frame_done(void* priv, uint16_t frame_idx,
                               struct st22_tx_frame_meta* meta) {
  struct st22p_tx_ctx* ctx = priv;
  int ret;
  struct st22p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  framebuff->src.tfmt = meta->tfmt;
  framebuff->dst.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->dst.timestamp = meta->timestamp;
  framebuff->src.rtp_timestamp = framebuff->dst.rtp_timestamp = meta->rtp_timestamp;

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST22P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST22P_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_done) { /* notify app which frame done */
    struct st_frame* frame = tx_st22p_user_frame(ctx, framebuff);
    ctx->ops.notify_frame_done(ctx->ops.priv, frame);
  }

  tx_st22p_notify_frame_available(ctx);

  MT_USDT_ST22P_TX_FRAME_DONE(ctx->idx, frame_idx, meta->rtp_timestamp);

  return ret;
}

static int tx_st22p_notify_event(void* priv, enum st_event event, void* args) {
  struct st22p_tx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static int tx_st22p_encode_get_block_wait(struct st22p_tx_ctx* ctx) {
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->encode_block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->encode_block_wake_cond,
                               &ctx->encode_block_wake_mutex,
                               ctx->encode_block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->encode_block_wake_mutex);
  return 0;
}

static int tx_st22p_encode_wake_block(void* priv) {
  struct st22p_tx_ctx* ctx = priv;

  tx_st22p_encode_block_wake(ctx);
  return 0;
}

static int tx_st22p_encode_set_timeout(void* priv, uint64_t timedwait_ns) {
  struct st22p_tx_ctx* ctx = priv;
  ctx->encode_block_timeout_ns = timedwait_ns;
  return 0;
}

static struct st22_encode_frame_meta* tx_st22p_encode_get_frame(void* priv) {
  struct st22p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) {
    dbg("%s(%d), not ready %d\n", __func__, idx, ctx->type);
    if (ctx->encode_block_get) {
      tx_st22p_encode_get_block_wait(ctx);
      if (!ctx->ready) return NULL;
    }
    return NULL; /* not ready */
  }

  ctx->stat_encode_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st22p_next_available(ctx, ST22P_TX_FRAME_READY);
  if (!framebuff && ctx->encode_block_get) { /* wait here for block mode */
    mt_pthread_mutex_unlock(&ctx->lock);
    tx_st22p_encode_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff = tx_st22p_next_available(ctx, ST22P_TX_FRAME_READY);
  }
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    dbg("%s(%d), no ready frame\n", __func__, idx);
    return NULL;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_ENCODING;
  mt_pthread_mutex_unlock(&ctx->lock);

  ctx->stat_encode_get_frame_succ++;
  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  struct st22_encode_frame_meta* frame = &framebuff->encode_frame;
  MT_USDT_ST22P_TX_ENCODE_GET(idx, framebuff->idx, frame->src->addr[0],
                              frame->dst->addr[0]);
  return frame;
}

/* min frame size should be capable of bulk pkts */
#define ST22_ENCODE_MIN_FRAME_SZ ((ST_SESSION_MAX_BULK + 1) * MTL_PKT_MAX_RTP_BYTES)

static int tx_st22p_encode_put_frame(void* priv, struct st22_encode_frame_meta* frame,
                                     int result) {
  struct st22p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff = frame->priv;
  uint16_t encode_idx = framebuff->idx;
  size_t data_size = frame->dst->data_size;
  size_t max_size = ctx->encode_impl->codestream_max_size;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST22P_TX_FRAME_IN_ENCODING != framebuff->stat) {
    mt_pthread_mutex_unlock(&ctx->lock);
    err("%s(%d), frame %u not in encoding %d\n", __func__, idx, encode_idx,
        framebuff->stat);
    return -EIO;
  }

  ctx->stat_encode_put_frame++;
  dbg("%s(%d), frame %u result %d data_size %" PRIu64 "\n", __func__, idx, encode_idx,
      result, data_size);
  if ((result < 0) || (data_size <= ST22_ENCODE_MIN_FRAME_SZ) || (data_size > max_size)) {
    warn("%s(%d), invalid frame %u result %d data_size %" PRIu64
         ", allowed min %u max %" PRIu64 "\n",
         __func__, idx, encode_idx, result, data_size, ST22_ENCODE_MIN_FRAME_SZ,
         max_size);
    framebuff->stat = ST22P_TX_FRAME_FREE;
    mt_pthread_mutex_unlock(&ctx->lock);
    tx_st22p_notify_frame_available(ctx);
    mt_atomic32_inc(&ctx->stat_encode_fail);
  } else {
    framebuff->stat = ST22P_TX_FRAME_ENCODED;
    mt_pthread_mutex_unlock(&ctx->lock);
  }

  MT_USDT_ST22P_TX_ENCODE_PUT(idx, framebuff->idx, frame->src->addr[0],
                              frame->dst->addr[0], result, data_size);
  return 0;
}

static int tx_st22p_encode_dump(void* priv) {
  struct st22p_tx_ctx* ctx = priv;
  struct st22p_tx_frame* framebuff = ctx->framebuffs;
  uint16_t status_counts[ST22P_TX_FRAME_STATUS_MAX] = {0};

  if (!ctx->ready) return -EBUSY; /* not ready */

  int encode_fail = mt_atomic32_read(&ctx->stat_encode_fail);
  mt_atomic32_set(&ctx->stat_encode_fail, 0);
  if (encode_fail) {
    notice("TX_ST22P(%s), encode fail %d\n", ctx->ops_name, encode_fail);
  }

  for (uint16_t j = 0; j < ctx->framebuff_cnt; j++) {
    enum st22p_tx_frame_status stat = framebuff[j].stat;
    if (stat < ST22P_TX_FRAME_STATUS_MAX) {
      status_counts[stat]++;
    }
  }

  char status_str[256];
  int offset = 0;
  for (uint16_t i = 0; i < ST22P_TX_FRAME_STATUS_MAX; i++) {
    if (status_counts[i] > 0) {
      offset += snprintf(status_str + offset, sizeof(status_str) - offset, "%s:%u ",
                         st22p_tx_frame_stat_name_short[i], status_counts[i]);
    }
  }
  notice("TX_st22p(%d,%s), framebuffer queue: %s\n", ctx->idx, ctx->ops_name, status_str);

  notice("TX_ST22P(%s), frame get try %d succ %d, put %d\n", ctx->ops_name,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  notice("TX_ST22P(%s), encoder get try %d succ %d, put %d\n", ctx->ops_name,
         ctx->stat_encode_get_frame_try, ctx->stat_encode_get_frame_succ,
         ctx->stat_encode_put_frame);
  ctx->stat_encode_get_frame_try = 0;
  ctx->stat_encode_get_frame_succ = 0;
  ctx->stat_encode_put_frame = 0;

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
  ops_tx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  for (int i = 0; i < ops_tx.num_port; i++) {
    memcpy(ops_tx.dip_addr[i], ops->port.dip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_tx.udp_src_port[i] = ops->port.udp_src_port[i];
    ops_tx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST22P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_P][0], &ops->tx_dst_mac[MTL_PORT_P][0],
           MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST22_TX_FLAG_USER_P_MAC;
  }
  if (ops->flags & ST22P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_R][0], &ops->tx_dst_mac[MTL_PORT_R][0],
           MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST22_TX_FLAG_USER_R_MAC;
  }
  if (ops->flags & ST22P_TX_FLAG_DISABLE_BOXES)
    ops_tx.flags |= ST22_TX_FLAG_DISABLE_BOXES;
  if (ops->flags & ST22P_TX_FLAG_USER_PACING) ops_tx.flags |= ST22_TX_FLAG_USER_PACING;
  if (ops->flags & ST22P_TX_FLAG_USER_TIMESTAMP)
    ops_tx.flags |= ST22_TX_FLAG_USER_TIMESTAMP;
  if (ops->flags & ST22P_TX_FLAG_ENABLE_VSYNC) ops_tx.flags |= ST22_TX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22P_TX_FLAG_ENABLE_RTCP) {
    ops_tx.flags |= ST22_TX_FLAG_ENABLE_RTCP;
    ops_tx.rtcp = ops->rtcp;
  }
  if (ops->flags & ST22P_TX_FLAG_DISABLE_BULK) ops_tx.flags |= ST22_TX_FLAG_DISABLE_BULK;
  if (ops->flags & ST22P_TX_FLAG_FORCE_NUMA) {
    ops_tx.socket_id = ops->socket_id;
    ops_tx.flags |= ST22_TX_FLAG_FORCE_NUMA;
  }
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.width = ops->width;
  ops_tx.height = ops->height;
  ops_tx.fps = ops->fps;
  ops_tx.interlaced = ops->interlaced;
  ops_tx.payload_type = ops->port.payload_type;
  ops_tx.ssrc = ops->port.ssrc;
  ops_tx.type = ST22_TYPE_FRAME_LEVEL;
  ops_tx.pack_type = ops->pack_type;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  if (ctx->derive)
    ops_tx.framebuff_max_size = ctx->src_size;
  else
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
    frames[i].dst.fmt = ctx->codestream_fmt;
    frames[i].dst.interlaced = ops->interlaced;
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
    if (!ctx->ext_frame) {
      for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
        if (ctx->framebuffs[i].src.addr[0]) {
          mt_rte_free(ctx->framebuffs[i].src.addr[0]);
          ctx->framebuffs[i].src.addr[0] = NULL;
        }
      }
    }
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].stat != ST22P_TX_FRAME_FREE) {
        warn("%s(%d), frame %u are still in %s\n", __func__, ctx->idx, i,
             tx_st22p_stat_name(ctx->framebuffs[i].stat));
      }
    }
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int tx_st22p_init_src_fbs(struct st22p_tx_ctx* ctx, struct st22p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st22p_tx_frame* frames;
  void* src;
  size_t src_size = ctx->src_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST22P_TX_FRAME_FREE;
    frames[i].idx = i;
    frames[i].src.fmt = ops->input_fmt;
    frames[i].src.interlaced = ops->interlaced;
    frames[i].src.buffer_size = src_size;
    frames[i].src.data_size = src_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.priv = &frames[i];

    if (ctx->derive) continue; /* skip the plane init */

    if (ctx->ext_frame) { /* will use ext frame from user */
      uint8_t planes = st_frame_fmt_planes(frames[i].src.fmt);
      for (uint8_t plane = 0; plane < planes; plane++) {
        frames[i].src.addr[plane] = NULL;
        frames[i].src.iova[plane] = 0;
      }
    } else {
      src = mt_rte_zmalloc_socket(src_size, soc_id);
      if (!src) {
        err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
        tx_st22p_uinit_src_fbs(ctx);
        return -ENOMEM;
      }

      /* init plane */
      st_frame_init_plane_single_src(&frames[i].src, src,
                                     mtl_hp_virt2iova(ctx->impl, src));
      /* check plane */
      if (st_frame_sanity_check(&frames[i].src) < 0) {
        err("%s(%d), src frame %d sanity check fail\n", __func__, idx, i);
        tx_st22p_uinit_src_fbs(ctx);
        return -EINVAL;
      }
      dbg("%s(%d), src frame malloc succ at %u\n", __func__, idx, i);
    }
  }

  info("%s(%d), size %" PRIu64 " fmt %d with %u frames\n", __func__, idx, src_size,
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
  req.req.interlaced = ops->interlaced;
  req.req.socket_id = ctx->socket_id;
  req.priv = ctx;
  req.get_frame = tx_st22p_encode_get_frame;
  req.wake_block = tx_st22p_encode_wake_block;
  req.set_block_timeout = tx_st22p_encode_set_timeout;
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

  if (encode_impl->req.req.resp_flag & ST22_ENCODER_RESP_FLAG_BLOCK_GET) {
    ctx->encode_block_get = true;
    info("%s(%d), encoder use block get mode\n", __func__, idx);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int tx_st22p_get_block_wait(struct st22p_tx_ctx* ctx) {
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                               ctx->block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  return 0;
}

static int tx_st22p_usdt_dump_frame(struct st22p_tx_ctx* ctx, struct st_frame* frame) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  struct st22p_tx_ops* ops = &ctx->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st22ptx_s%d_%d_%d_XXXXXX.yuv", idx, ops->width, ops->height);
  fd = mt_mkstemps(usdt_dump_path, strlen(".yuv"));
  if (fd < 0) {
    err("%s(%d), mkstemps %s fail %d\n", __func__, idx, usdt_dump_path, fd);
    return fd;
  }

  /* write frame to dump file */
  ssize_t n = 0;
  uint8_t planes = st_frame_fmt_planes(frame->fmt);
  uint32_t h = st_frame_data_height(frame);
  for (uint8_t plane = 0; plane < planes; plane++) {
    n += write(fd, frame->addr[plane], frame->linesize[plane] * h);
  }
  MT_USDT_ST22P_TX_FRAME_DUMP(idx, usdt_dump_path, frame->addr[0], n);

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static void tx_st22p_framebuffs_flush(struct st22p_tx_ctx* ctx) {
  /* wait all frame are in free or in transmitting(flushed by transport) */
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st22p_tx_frame* framebuff = &ctx->framebuffs[i];
    int retry = 0;

    while (1) {
      if (framebuff->stat == ST22P_TX_FRAME_FREE) break;
      if (framebuff->stat == ST22P_TX_FRAME_IN_TRANSMITTING) {
        /*
         * make sure to wait when the frame is in transmit
         * without we will stop before encoder takes the frame
         * WA to use sleep here, todo: add a transport API to query the stat
         */
        mt_sleep_ms(50);
      }

      dbg("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
          tx_st22p_stat_name(framebuff->stat), retry);
      retry++;
      if (retry > 100) {
        info("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
             tx_st22p_stat_name(framebuff->stat), retry);
        break;
      }
      mt_sleep_ms(10);
    }
  }
}

struct st_frame* st22p_tx_get_frame(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st22p_next_available(ctx, ST22P_TX_FRAME_FREE);
  if (!framebuff && ctx->block_get) {
    mt_pthread_mutex_unlock(&ctx->lock);
    tx_st22p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff = tx_st22p_next_available(ctx, ST22P_TX_FRAME_FREE);
  }
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_TX_FRAME_IN_USER;
  framebuff->seq_number = ctx->framebuff_sequence_number++;
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  if (ctx->ops.interlaced) { /* init second_field but user still can customize */
    framebuff->dst.second_field = framebuff->src.second_field = ctx->second_field;
    ctx->second_field = ctx->second_field ? false : true;
  }
  ctx->stat_get_frame_succ++;
  struct st_frame* frame = tx_st22p_user_frame(ctx, framebuff);
  dbg("%s(%d), frame %u addr %p\n", __func__, idx, framebuff->idx, frame->addr[0]);
  MT_USDT_ST22P_TX_FRAME_GET(idx, framebuff->idx, frame->addr[0]);
  return frame;
}

int st22p_tx_put_frame(st22p_tx_handle handle, struct st_frame* frame) {
  struct st22p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (ctx->ext_frame) {
    err("%s(%d), EXT_FRAME enabled, use st22p_tx_put_ext_frame instead\n", __func__, idx);
    return -EIO;
  }

  if (ctx->ops.interlaced) { /* update second_field */
    framebuff->dst.second_field = framebuff->src.second_field = frame->second_field;
  }

  if (ctx->derive) {
    framebuff->stat = ST22P_TX_FRAME_ENCODED;
  } else {
    framebuff->stat = ST22P_TX_FRAME_READY;
    tx_st22p_encode_notify_frame_ready(ctx);
  }
  ctx->stat_put_frame++;

  MT_USDT_ST22P_TX_FRAME_PUT(idx, framebuff->idx, frame->addr[0], framebuff->stat,
                             frame->data_size);
  /* check if dump USDT enabled */
  if (!ctx->derive && MT_USDT_ST22P_TX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ctx->ops.fps) * 5; /* dump every 5s now */
    if ((ctx->usdt_frame_cnt % period) == (period / 2)) {
      tx_st22p_usdt_dump_frame(ctx, frame);
    }
    ctx->usdt_frame_cnt++;
  } else {
    ctx->usdt_frame_cnt = 0;
  }

  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);
  return 0;
}

int st22p_tx_put_ext_frame(st22p_tx_handle handle, struct st_frame* frame,
                           struct st_ext_frame* ext_frame) {
  struct st22p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;
  int ret = 0;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (!ctx->ext_frame) {
    err("%s(%d), EXT_FRAME flag not enabled\n", __func__, idx);
    return -EIO;
  }

  if (ctx->derive) {
    err("%s(%d), derive mode not support ext frame\n", __func__, idx);
    return -EIO;
  }

  if (ST22P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  uint8_t planes = st_frame_fmt_planes(framebuff->src.fmt);

  for (int plane = 0; plane < planes; plane++) {
    framebuff->src.addr[plane] = ext_frame->addr[plane];
    framebuff->src.iova[plane] = ext_frame->iova[plane];
    framebuff->src.linesize[plane] = ext_frame->linesize[plane];
  }
  framebuff->src.data_size = framebuff->src.buffer_size = ext_frame->size;
  framebuff->src.opaque = ext_frame->opaque;
  framebuff->src.flags |= ST_FRAME_FLAG_EXT_BUF;
  ret = st_frame_sanity_check(&framebuff->src);
  if (ret < 0) {
    err("%s, ext framebuffer sanity check fail %d fb_idx %d\n", __func__, ret,
        producer_idx);
    return ret;
  }

  if (ctx->ops.interlaced) { /* update second_field */
    framebuff->dst.second_field = framebuff->src.second_field = frame->second_field;
  }

  framebuff->stat = ST22P_TX_FRAME_READY;
  tx_st22p_encode_notify_frame_ready(ctx);
  ctx->stat_put_frame++;
  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);

  return 0;
}

st22p_tx_handle st22p_tx_create(mtl_handle mt, struct st22p_tx_ops* ops) {
  static int st22p_tx_idx;
  struct mtl_main_impl* impl = mt;
  struct st22p_tx_ctx* ctx;
  int ret;
  int idx = st22p_tx_idx;
  size_t src_size;
  enum st_frame_fmt codestream_fmt;

  /* validate the input parameters */
  if (!mt || !ops) {
    err("%s(%d), NULL input parameters \n", __func__, idx);
    return NULL;
  }

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  codestream_fmt = st_codec_codestream_fmt(ops->codec);
  if (codestream_fmt == ST_FRAME_FMT_MAX) {
    err("%s(%d), unknow codec %d\n", __func__, idx, ops->codec);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST22P_TX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST22P_TX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s(%d), ctx malloc fail on socket %d\n", __func__, idx, socket);
    return NULL;
  }

  if (codestream_fmt == ops->input_fmt) {
    ctx->derive = true;
    info("%s(%d), derive mode\n", __func__, idx);
  }

  if (ctx->derive) {
    src_size = ops->codestream_size;
    if (!src_size) {
      warn("%s(%d), codestream_size is not set by user in derive mode, use default 1M\n",
           __func__, idx);
      src_size = 0x100000;
    }
  } else {
    src_size = st_frame_size(ops->input_fmt, ops->width, ops->height, ops->interlaced);
    if (!src_size) {
      err("%s(%d), get source size fail\n", __func__, idx);
      return NULL;
    }
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->codestream_fmt = codestream_fmt;
  ctx->ready = false;
  ctx->ext_frame = (ops->flags & ST22P_TX_FLAG_EXT_FRAME) ? true : false;
  ctx->impl = impl;
  ctx->type = MT_ST22_HANDLE_PIPELINE_TX;
  ctx->src_size = src_size;
  mt_atomic32_set(&ctx->stat_encode_fail, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->encode_block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->encode_block_wake_cond);
  ctx->encode_block_timeout_ns = NS_PER_S; /* default to 1s */

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S; /* default to 1s */
  if (ops->flags & ST22P_TX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST22P_TX_%d", idx);
  }
  ctx->ops = *ops;

  /* get one suitable jpegxs encode device */
  if (!ctx->derive) {
    ret = tx_st22p_get_encoder(impl, ctx, ops);
    if (ret < 0) {
      err("%s(%d), get encoder fail %d\n", __func__, idx, ret);
      st22p_tx_free(ctx);
      return NULL;
    }
  }

  /* init fbs */
  ret = tx_st22p_init_src_fbs(ctx, ops);
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
  notice("%s(%d), codestream fmt %s, input fmt: %s, flags 0x%x\n", __func__, idx,
         st_frame_fmt_name(ctx->codestream_fmt), st_frame_fmt_name(ops->input_fmt),
         ops->flags);
  st22p_tx_idx++;

  if (!ctx->block_get) tx_st22p_notify_frame_available(ctx);

  return ctx;
}

int st22p_tx_free(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->framebuffs && mt_started(impl)) {
    tx_st22p_framebuffs_flush(ctx);
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

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  mt_pthread_mutex_destroy(&ctx->encode_block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->encode_block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

void* st22p_tx_get_fb_addr(st22p_tx_handle handle, uint16_t idx) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  if (ctx->ext_frame) {
    err("%s(%d), not known as EXT_FRAME flag enabled\n", __func__, cidx);
    return NULL;
  }

  return tx_st22p_user_frame(ctx, &ctx->framebuffs[idx])->addr[0];
}

size_t st22p_tx_frame_size(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->src_size;
}

int st22p_tx_update_destination(st22p_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st22_tx_update_destination(ctx->transport, dst);
}

int st22p_tx_wake_block(st22p_tx_handle handle) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) tx_st22p_block_wake(ctx);

  return 0;
}

int st22p_tx_set_block_timeout(st22p_tx_handle handle, uint64_t timedwait_ns) {
  struct st22p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}
