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

static void rx_st22p_block_wake(struct st22p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void rx_st22p_decode_block_wake(struct st22p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->decode_block_wake_mutex);
  mt_pthread_cond_signal(&ctx->decode_block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->decode_block_wake_mutex);
}

static void rx_st22p_decode_notify_frame_ready(struct st22p_rx_ctx* ctx) {
  if (ctx->derive) return; /* no decoder for derive mode */

  struct st22_decode_session_impl* decoder = ctx->decode_impl;
  struct st22_decode_dev_impl* dev_impl = decoder->parent;
  struct st22_decoder_dev* dev = &dev_impl->dev;
  st22_decode_priv session = decoder->session;

  if (dev->notify_frame_available) dev->notify_frame_available(session);

  if (ctx->decode_block_get) {
    /* notify block */
    rx_st22p_decode_block_wake(ctx);
  }
}

static void rx_st22p_notify_frame_available(struct st22p_rx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    rx_st22p_block_wake(ctx);
  }
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
  int ret;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_producer_idx, ST22P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    mt_atomic32_inc(&ctx->stat_busy);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  MT_USDT_ST22P_RX_FRAME_AVAILABLE(ctx->idx, framebuff->idx, frame, meta->rtp_timestamp,
                                   meta->frame_total_size);

  if (ctx->ext_frame) {
    struct st_ext_frame ext_frame;
    memset(&ext_frame, 0x0, sizeof(ext_frame));
    ret = ctx->ops.query_ext_frame(ctx->ops.priv, &ext_frame, meta);
    if (ret < 0) {
      err("%s(%d), query_ext_frame for frame %u fail %d\n", __func__, ctx->idx,
          framebuff->idx, ret);
      mt_pthread_mutex_unlock(&ctx->lock);
      return ret;
    }

    uint8_t planes = st_frame_fmt_planes(framebuff->dst.fmt);
    for (int plane = 0; plane < planes; plane++) {
      framebuff->dst.addr[plane] = ext_frame.addr[plane];
      framebuff->dst.iova[plane] = ext_frame.iova[plane];
      framebuff->dst.linesize[plane] = ext_frame.linesize[plane];
    }
    framebuff->dst.data_size = framebuff->dst.buffer_size = ext_frame.size;
    framebuff->dst.opaque = ext_frame.opaque;
    framebuff->dst.flags |= ST_FRAME_FLAG_EXT_BUF;
    ret = st_frame_sanity_check(&framebuff->dst);
    if (ret < 0) {
      err("%s(%d), ext_frame check frame %u fail %d\n", __func__, ctx->idx,
          framebuff->idx, ret);
      mt_pthread_mutex_unlock(&ctx->lock);
      return ret;
    }
  }

  framebuff->src.addr[0] = frame;
  framebuff->src.data_size = meta->frame_total_size;
  framebuff->src.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->dst.tfmt = meta->tfmt;
  /* set dst timestamp to same as src? */
  framebuff->dst.timestamp = meta->timestamp;
  framebuff->src.rtp_timestamp = framebuff->dst.rtp_timestamp = meta->rtp_timestamp;

  /* if second field */
  framebuff->dst.second_field = framebuff->src.second_field = meta->second_field;

  framebuff->src.pkts_total = framebuff->dst.pkts_total = meta->pkts_total;
  for (enum mtl_session_port s_port = MTL_SESSION_PORT_P; s_port < MTL_SESSION_PORT_MAX;
       s_port++) {
    framebuff->src.pkts_recv[s_port] = framebuff->dst.pkts_recv[s_port] =
        meta->pkts_recv[s_port];
  }

  /* ask app to consume src frame directly for derive mode */
  if (ctx->derive) {
    framebuff->dst = framebuff->src;
    framebuff->stat = ST22P_RX_FRAME_DECODED;
    /* point to next */
    ctx->framebuff_producer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st22p_notify_frame_available(ctx);
    return 0;
  }
  framebuff->stat = ST22P_RX_FRAME_READY;
  /* point to next */
  ctx->framebuff_producer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  rx_st22p_decode_notify_frame_ready(ctx);

  return 0;
}

static int rx_st22p_notify_event(void* priv, enum st_event event, void* args) {
  struct st22p_rx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static int rx_st22p_decode_get_block_wait(struct st22p_rx_ctx* ctx) {
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->decode_block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->decode_block_wake_cond,
                               &ctx->decode_block_wake_mutex,
                               ctx->decode_block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->decode_block_wake_mutex);
  return 0;
}

static int rx_st22p_decode_wake_block(void* priv) {
  struct st22p_rx_ctx* ctx = priv;

  rx_st22p_decode_block_wake(ctx);
  return 0;
}

static int rx_st22p_decode_set_timeout(void* priv, uint64_t timedwait_ns) {
  struct st22p_rx_ctx* ctx = priv;
  ctx->decode_block_timeout_ns = timedwait_ns;
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

  if (!ctx->ready) {
    dbg("%s(%d), not ready %d\n", __func__, idx, ctx->type);
    if (ctx->decode_block_get) {
      rx_st22p_decode_get_block_wait(ctx);
      if (!ctx->ready) return NULL;
    }
    return NULL; /* not ready */
  }

  ctx->stat_decode_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_decode_idx, ST22P_RX_FRAME_READY);
  if (!framebuff && ctx->decode_block_get) { /* wait here for block mode */
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st22p_decode_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        rx_st22p_next_available(ctx, ctx->framebuff_decode_idx, ST22P_RX_FRAME_READY);
  }
  /* not any ready frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    dbg("%s(%d), no ready frame\n", __func__, idx);
    return NULL;
  }

  framebuff->stat = ST22P_RX_FRAME_IN_DECODING;
  /* point to next */
  ctx->framebuff_decode_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  ctx->stat_decode_get_frame_succ++;
  struct st22_decode_frame_meta* frame = &framebuff->decode_frame;
  MT_USDT_ST22P_RX_DECODE_GET(idx, framebuff->idx, frame->src->addr[0],
                              frame->dst->addr[0], frame->src->data_size);
  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return frame;
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

  ctx->stat_decode_put_frame++;
  dbg("%s(%d), frame %u result %d\n", __func__, idx, decode_idx, result);
  if (result < 0) {
    /* free the frame */
    st22_rx_put_framebuff(ctx->transport, framebuff->src.addr[0]);
    framebuff->stat = ST22P_RX_FRAME_FREE;
    mt_atomic32_inc(&ctx->stat_decode_fail);
  } else {
    framebuff->stat = ST22P_RX_FRAME_DECODED;
    rx_st22p_notify_frame_available(ctx);
  }

  MT_USDT_ST22P_RX_DECODE_PUT(idx, framebuff->idx, frame->src->addr[0],
                              frame->dst->addr[0], result);
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

  int decode_fail = mt_atomic32_read(&ctx->stat_decode_fail);
  mt_atomic32_set(&ctx->stat_decode_fail, 0);
  if (decode_fail) {
    notice("RX_ST22P(%s), decode fail %d\n", ctx->ops_name, decode_fail);
  }

  int busy = mt_atomic32_read(&ctx->stat_busy);
  mt_atomic32_set(&ctx->stat_busy, 0);
  if (busy) {
    notice("RX_ST22P(%s), busy drop frame %d\n", ctx->ops_name, busy);
  }

  notice("RX_ST22P(%s), frame get try %d succ %d, put %d\n", ctx->ops_name,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  notice("TX_ST22P(%s), decoder get try %d succ %d, put %d\n", ctx->ops_name,
         ctx->stat_decode_get_frame_try, ctx->stat_decode_get_frame_succ,
         ctx->stat_decode_put_frame);
  ctx->stat_decode_get_frame_try = 0;
  ctx->stat_decode_get_frame_succ = 0;
  ctx->stat_decode_put_frame = 0;

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
    memcpy(ops_rx.ip_addr[i], ops->port.ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops_rx.mcast_sip_addr[i], ops->port.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_rx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST22P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST22_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST22P_RX_FLAG_ENABLE_VSYNC) ops_rx.flags |= ST22_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST22P_RX_FLAG_ENABLE_RTCP) {
    ops_rx.flags |= ST22_RX_FLAG_ENABLE_RTCP;
    ops_rx.rtcp = ops->rtcp;
    if (ops->flags & ST22P_RX_FLAG_SIMULATE_PKT_LOSS)
      ops_rx.flags |= ST22_RX_FLAG_SIMULATE_PKT_LOSS;
  }
  if (ops->flags & ST22P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    ops_rx.flags |= ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  if (ops->flags & ST22P_RX_FLAG_FORCE_NUMA) {
    ops_rx.socket_id = ops->socket_id;
    ops_rx.flags |= ST22_RX_FLAG_FORCE_NUMA;
  }
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.interlaced = ops->interlaced;
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.ssrc = ops->port.ssrc;
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
    frames[i].src.fmt = ctx->codestream_fmt;
    frames[i].src.buffer_size = ops_rx.framebuff_max_size;
    frames[i].src.data_size = ops_rx.framebuff_max_size;
    frames[i].src.interlaced = ops->interlaced;
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
    if (!ctx->derive && !ctx->ext_frame) {
      for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
        if (ctx->framebuffs[i].dst.addr[0]) {
          mt_rte_free(ctx->framebuffs[i].dst.addr[0]);
          ctx->framebuffs[i].dst.addr[0] = NULL;
        }
      }
    }
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int rx_st22p_init_dst_fbs(struct st22p_rx_ctx* ctx, struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
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
    frames[i].dst.fmt = ops->output_fmt;
    frames[i].dst.interlaced = ops->interlaced;
    frames[i].dst.buffer_size = dst_size;
    frames[i].dst.data_size = dst_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    frames[i].dst.priv = &frames[i];

    if (ctx->derive) continue; /* skip the plane init */

    if (ctx->ext_frame) { /* will use ext frame from user */
      uint8_t planes = st_frame_fmt_planes(frames[i].dst.fmt);

      for (uint8_t plane = 0; plane < planes; plane++) {
        frames[i].dst.addr[plane] = NULL;
        frames[i].dst.iova[plane] = 0;
      }
    } else {
      dst = mt_rte_zmalloc_socket(dst_size, soc_id);
      if (!dst) {
        err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
        rx_st22p_uinit_dst_fbs(ctx);
        return -ENOMEM;
      }

      /* init plane */
      st_frame_init_plane_single_src(&frames[i].dst, dst,
                                     mtl_hp_virt2iova(ctx->impl, dst));
      /* check plane */
      if (st_frame_sanity_check(&frames[i].dst) < 0) {
        err("%s(%d), dst frame %d sanity check fail\n", __func__, idx, i);
        rx_st22p_uinit_dst_fbs(ctx);
        return -EINVAL;
      }
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
  req.req.interlaced = ops->interlaced;
  req.req.socket_id = ctx->socket_id;
  req.priv = ctx;
  req.get_frame = rx_st22p_decode_get_frame;
  req.wake_block = rx_st22p_decode_wake_block;
  req.set_block_timeout = rx_st22p_decode_set_timeout;
  req.put_frame = rx_st22p_decode_put_frame;
  req.dump = rx_st22p_decode_dump;

  struct st22_decode_session_impl* decode_impl = st22_get_decoder(impl, &req);
  if (!decode_impl) {
    err("%s(%d), get decoder fail\n", __func__, idx);
    return -EINVAL;
  }
  ctx->decode_impl = decode_impl;

  if (decode_impl->req.req.resp_flag & ST22_DECODER_RESP_FLAG_BLOCK_GET) {
    ctx->decode_block_get = true;
    info("%s(%d), decoder use block get mode\n", __func__, idx);
  }

  dbg("%s(%d), succ\n", __func__, idx);
  return 0;
}

static int rx_st22p_get_block_wait(struct st22p_rx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                               ctx->block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

static int rx_st22p_usdt_dump_frame(struct st22p_rx_ctx* ctx, struct st_frame* frame) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  struct st22p_rx_ops* ops = &ctx->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st22prx_s%d_%d_%d_XXXXXX.yuv", idx, ops->width, ops->height);
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
  MT_USDT_ST22P_RX_FRAME_DUMP(idx, usdt_dump_path, frame->addr[0], n);

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
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

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_consumer_idx, ST22P_RX_FRAME_DECODED);
  if (!framebuff && ctx->block_get) {
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st22p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        rx_st22p_next_available(ctx, ctx->framebuff_consumer_idx, ST22P_RX_FRAME_DECODED);
  }
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
  ctx->stat_get_frame_succ++;
  struct st_frame* frame = &framebuff->dst;
  MT_USDT_ST22P_RX_FRAME_GET(idx, framebuff->idx, frame->addr[0], frame->data_size);
  /* check if dump USDT enabled */
  if (!ctx->derive && MT_USDT_ST22P_RX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ctx->ops.fps) * 5; /* dump every 5s now */
    if ((ctx->usdt_frame_cnt % period) == (period / 2)) {
      rx_st22p_usdt_dump_frame(ctx, frame);
    }
    ctx->usdt_frame_cnt++;
  } else {
    ctx->usdt_frame_cnt = 0;
  }
  return frame;
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
  ctx->stat_put_frame++;
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);
  MT_USDT_ST22P_RX_FRAME_PUT(idx, framebuff->idx, frame->addr[0]);

  return 0;
}

st22p_rx_handle st22p_rx_create(mtl_handle mt, struct st22p_rx_ops* ops) {
  static int st22p_rx_idx;
  struct mtl_main_impl* impl = mt;
  struct st22p_rx_ctx* ctx;
  int ret;
  int idx = st22p_rx_idx;
  size_t dst_size;
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

  if (ops->flags & ST22P_RX_FLAG_EXT_FRAME) {
    if (!ops->query_ext_frame) {
      err("%s, no query_ext_frame query callback for ext frame mode\n", __func__);
      return NULL;
    }
  }

  codestream_fmt = st_codec_codestream_fmt(ops->codec);
  if (codestream_fmt == ST_FRAME_FMT_MAX) {
    err("%s(%d), unknow codec %d\n", __func__, idx, ops->codec);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST22P_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST22P_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  if (codestream_fmt == ops->output_fmt) {
    ctx->derive = true;
    info("%s(%d), derive mode\n", __func__, idx);
  }

  if (ctx->derive) {
    dst_size = 0;
  } else {
    dst_size = st_frame_size(ops->output_fmt, ops->width, ops->height, ops->interlaced);
    if (!dst_size) {
      err("%s(%d), get dst size fail\n", __func__, idx);
      return NULL;
    }
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->ext_frame = (ops->flags & ST22P_RX_FLAG_EXT_FRAME) ? true : false;
  ctx->codestream_fmt = codestream_fmt;
  ctx->impl = impl;
  ctx->type = MT_ST22_HANDLE_PIPELINE_RX;
  ctx->dst_size = dst_size;
  /* use the possible max size */
  ctx->max_codestream_size = ops->max_codestream_size;
  if (!ctx->max_codestream_size) ctx->max_codestream_size = dst_size;
  if (ctx->derive && !ctx->max_codestream_size) {
    warn("%s(%d), codestream_size is not set by user in derive mode, use default 1M\n",
         __func__, idx);
    ctx->max_codestream_size = 0x100000;
  }
  mt_atomic32_set(&ctx->stat_decode_fail, 0);
  mt_atomic32_set(&ctx->stat_busy, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->decode_block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->decode_block_wake_cond);
  ctx->decode_block_timeout_ns = NS_PER_S;

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  if (ops->flags & ST22P_RX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST22P_RX_%d", idx);
  }
  ctx->ops = *ops;

  /* get one suitable jpegxs decode device */
  if (!ctx->derive) {
    ret = rx_st22p_get_decoder(impl, ctx, ops);
    if (ret < 0) {
      err("%s(%d), get decoder fail %d\n", __func__, idx, ret);
      st22p_rx_free(ctx);
      return NULL;
    }
  }

  /* init fbs */
  ret = rx_st22p_init_dst_fbs(ctx, ops);
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
  notice("%s(%d), codestream fmt %s, output fmt: %s, flags 0x%x\n", __func__, idx,
         st_frame_fmt_name(ctx->codestream_fmt), st_frame_fmt_name(ops->output_fmt),
         ops->flags);
  st22p_rx_idx++;

  if (!ctx->block_get) rx_st22p_notify_frame_available(ctx);

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
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  mt_pthread_mutex_destroy(&ctx->decode_block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->decode_block_wake_cond);
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

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  if (ctx->ext_frame) {
    err("%s(%d), not known as EXT_FRAME flag enabled\n", __func__, cidx);
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

  if (ctx->derive)
    return ctx->max_codestream_size;
  else
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

int st22p_rx_update_source(st22p_rx_handle handle, struct st_rx_source_info* src) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st22_rx_update_source(ctx->transport, src);
}

int st22p_rx_wake_block(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) rx_st22p_block_wake(ctx);

  return 0;
}

int st22p_rx_set_block_timeout(st22p_rx_handle handle, uint64_t timedwait_ns) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST22_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}
