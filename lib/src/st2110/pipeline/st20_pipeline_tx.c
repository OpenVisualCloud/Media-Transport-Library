/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_pipeline_tx.h"

#include "../../mt_log.h"

static const char* st20p_tx_frame_stat_name[ST20P_TX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_converting", "converted", "in_user", "in_transmitting",
};

static const char* tx_st20p_stat_name(enum st20p_tx_frame_status stat) {
  return st20p_tx_frame_stat_name[stat];
}

static uint16_t tx_st20p_next_idx(struct st20p_tx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static inline struct st_frame* tx_st20p_user_frame(struct st20p_tx_ctx* ctx,
                                                   struct st20p_tx_frame* framebuff) {
  return ctx->derive ? &framebuff->dst : &framebuff->src;
}

static void tx_st20p_block_wake(struct st20p_tx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void tx_st20p_notify_frame_available(struct st20p_tx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    tx_st20p_block_wake(ctx);
  }
}

static struct st20p_tx_frame* tx_st20p_next_available(
    struct st20p_tx_ctx* ctx, uint16_t idx_start, enum st20p_tx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st20p_tx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = tx_st20p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int tx_st20p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;
  struct st20p_tx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_consumer_idx, ST20P_TX_FRAME_CONVERTED);
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;

  struct st_frame* frame = tx_st20p_user_frame(ctx, framebuff);
  meta->second_field = frame->second_field;
  if (ctx->ops.flags & (ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_USER_TIMESTAMP)) {
    meta->tfmt = frame->tfmt;
    meta->timestamp = frame->timestamp;
  }
  if (framebuff->user_meta_data_size) {
    meta->user_meta = framebuff->user_meta;
    meta->user_meta_size = framebuff->user_meta_data_size;
  }
  /* point to next */
  ctx->framebuff_consumer_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  return 0;
}

static int tx_st20p_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;
  int ret;
  struct st20p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST20P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST20P_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  struct st_frame* frame = tx_st20p_user_frame(ctx, framebuff);
  frame->tfmt = meta->tfmt;
  frame->timestamp = meta->timestamp;
  frame->epoch = meta->epoch;

  if (ctx->ops.notify_frame_done) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, frame);
  }

  /* notify app can get frame */
  tx_st20p_notify_frame_available(ctx);

  return ret;
}

static int tx_st20p_notify_event(void* priv, enum st_event event, void* args) {
  struct st20p_tx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static struct st20_convert_frame_meta* tx_st20p_convert_get_frame(void* priv) {
  struct st20p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_convert_idx, ST20P_TX_FRAME_READY);
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_CONVERTING;
  /* point to next */
  ctx->framebuff_convert_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->convert_frame;
}

static int tx_st20p_convert_put_frame(void* priv, struct st20_convert_frame_meta* frame,
                                      int result) {
  struct st20p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff = frame->priv;
  uint16_t convert_idx = framebuff->idx;
  size_t data_size = frame->dst->data_size;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_CONVERTING != framebuff->stat) {
    err("%s(%d), frame %u not in converting %d\n", __func__, idx, convert_idx,
        framebuff->stat);
    return -EIO;
  }

  if ((result < 0) || (data_size <= 0)) {
    dbg("%s(%d), frame %u result %d data_size %" PRIu64 "\n", __func__, idx, convert_idx,
        result, data_size);
    framebuff->stat = ST20P_TX_FRAME_FREE;
    /* notify app can get frame */
    tx_st20p_notify_frame_available(ctx);
    rte_atomic32_inc(&ctx->stat_convert_fail);
  } else {
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
  }

  return 0;
}

static int tx_st20p_convert_dump(void* priv) {
  struct st20p_tx_ctx* ctx = priv;
  struct st20p_tx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t convert_idx = ctx->framebuff_convert_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  notice("TX_st20p(%s), p(%d:%s) cv(%d:%s) c(%d:%s)\n", ctx->ops_name, producer_idx,
         tx_st20p_stat_name(framebuff[producer_idx].stat), convert_idx,
         tx_st20p_stat_name(framebuff[convert_idx].stat), consumer_idx,
         tx_st20p_stat_name(framebuff[consumer_idx].stat));

  int convert_fail = rte_atomic32_read(&ctx->stat_convert_fail);
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  if (convert_fail) {
    notice("TX_st20p(%s), convert fail %d\n", ctx->ops_name, convert_fail);
  }

  int busy = rte_atomic32_read(&ctx->stat_busy);
  rte_atomic32_set(&ctx->stat_busy, 0);
  if (busy) {
    notice("TX_st20p(%s), busy drop frame %d\n", ctx->ops_name, busy);
  }

  return 0;
}

static int tx_st20p_create_transport(struct mtl_main_impl* impl, struct st20p_tx_ctx* ctx,
                                     struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st20_tx_ops ops_tx;
  st20_tx_handle transport;

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
  if (ops->flags & ST20P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_P][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_P][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST20_TX_FLAG_USER_P_MAC;
  }
  if (ops->flags & ST20P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_R][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_R][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST20_TX_FLAG_USER_R_MAC;
  }
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.start_vrx = ops->start_vrx;
  ops_tx.pad_interval = ops->pad_interval;
  ops_tx.rtp_timestamp_delta_us = ops->rtp_timestamp_delta_us;
  ops_tx.tx_hang_detect_ms = ops->tx_hang_detect_ms;
  ops_tx.width = ops->width;
  ops_tx.height = ops->height;
  ops_tx.fps = ops->fps;
  ops_tx.pacing = ops->transport_pacing;
  ops_tx.packing = ops->transport_packing;
  ops_tx.fmt = ops->transport_fmt;
  ops_tx.interlaced = ops->interlaced;
  ops_tx.linesize = ops->transport_linesize;
  ops_tx.payload_type = ops->port.payload_type;
  ops_tx.ssrc = ops->port.ssrc;
  ops_tx.type = ST20_TYPE_FRAME_LEVEL;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  ops_tx.get_next_frame = tx_st20p_next_frame;
  ops_tx.notify_frame_done = tx_st20p_frame_done;
  ops_tx.notify_event = tx_st20p_notify_event;
  if (ctx->derive && ops->flags & ST20P_TX_FLAG_EXT_FRAME)
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
  if (ops->flags & ST20P_TX_FLAG_USER_PACING) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
  if (ops->flags & ST20P_TX_FLAG_USER_TIMESTAMP)
    ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  if (ops->flags & ST20P_TX_FLAG_ENABLE_VSYNC) ops_tx.flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST20P_TX_FLAG_DISABLE_STATIC_PAD_P)
    ops_tx.flags |= ST20_TX_FLAG_DISABLE_STATIC_PAD_P;
  if (ops->flags & ST20P_TX_FLAG_ENABLE_RTCP) {
    ops_tx.flags |= ST20_TX_FLAG_ENABLE_RTCP;
    ops_tx.rtcp = ops->rtcp;
  }
  if (ops->flags & ST20P_TX_FLAG_RTP_TIMESTAMP_FIRST_PKT)
    ops_tx.flags |= ST20_TX_FLAG_RTP_TIMESTAMP_FIRST_PKT;
  if (ops->flags & ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH)
    ops_tx.flags |= ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  if (ops->flags & ST20P_TX_FLAG_DISABLE_BULK) ops_tx.flags |= ST20_TX_FLAG_DISABLE_BULK;

  transport = st20_tx_create(impl, &ops_tx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st20p_tx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->derive && ops->flags & ST20P_TX_FLAG_EXT_FRAME) {
      frames[i].dst.addr[0] = NULL;
    } else {
      frames[i].dst.addr[0] = st20_tx_get_framebuffer(transport, i);
    }
    frames[i].dst.fmt = st_frame_fmt_from_transport(ctx->ops.transport_fmt);
    frames[i].dst.interlaced = ops->interlaced;
    frames[i].dst.buffer_size =
        st_frame_size(frames[i].dst.fmt, ops->width, ops->height, ops->interlaced);
    frames[i].dst.data_size = frames[i].dst.buffer_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    frames[i].dst.linesize[0] = /* rfc4175 uses packed format */
        RTE_MAX(ops->transport_linesize,
                st_frame_least_linesize(frames[i].dst.fmt, frames[i].dst.width, 0));
    frames[i].dst.priv = &frames[i];

    frames[i].convert_frame.src = &frames[i].src;
    frames[i].convert_frame.dst = &frames[i].dst;
    frames[i].convert_frame.priv = &frames[i];
  }

  return 0;
}

static int tx_st20p_uinit_src_fbs(struct st20p_tx_ctx* ctx) {
  if (ctx->framebuffs) {
    if (!ctx->derive && !(ctx->ops.flags & ST20P_TX_FLAG_EXT_FRAME)) {
      /* do not free derived/ext frames */
      for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
        if (ctx->framebuffs[i].src.addr[0]) {
          mt_rte_free(ctx->framebuffs[i].src.addr[0]);
          ctx->framebuffs[i].src.addr[0] = NULL;
        }
      }
    }
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].user_meta) {
        mt_rte_free(ctx->framebuffs[i].user_meta);
        ctx->framebuffs[i].user_meta = NULL;
      }
    }
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int tx_st20p_init_src_fbs(struct mtl_main_impl* impl, struct st20p_tx_ctx* ctx,
                                 struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = mt_socket_id(impl, MTL_PORT_P);
  struct st20p_tx_frame* frames;
  void* src = NULL;
  size_t src_size = ctx->src_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST20P_TX_FRAME_FREE;
    frames[i].idx = i;
    frames[i].src.fmt = ops->input_fmt;
    frames[i].src.interlaced = ops->interlaced;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    if (!ctx->derive) { /* when derive, no need to alloc src frames */
      uint8_t planes = st_frame_fmt_planes(frames[i].src.fmt);
      if (ops->flags & ST20P_TX_FLAG_EXT_FRAME) {
        for (uint8_t plane = 0; plane < planes; plane++) {
          frames[i].src.addr[plane] = NULL;
          frames[i].src.iova[plane] = 0;
        }
      } else {
        src = mt_rte_zmalloc_socket(src_size, soc_id);
        if (!src) {
          err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
          tx_st20p_uinit_src_fbs(ctx);
          return -ENOMEM;
        }
        frames[i].src.buffer_size = src_size;
        frames[i].src.data_size = src_size;
        /* init plane */
        st_frame_init_plane_single_src(&frames[i].src, src,
                                       mtl_hp_virt2iova(ctx->impl, src));
        /* check plane */
        if (st_frame_sanity_check(&frames[i].src) < 0) {
          err("%s(%d), src frame %d sanity check fail\n", __func__, idx, i);
          tx_st20p_uinit_src_fbs(ctx);
          return -EINVAL;
        }
      }
      frames[i].src.priv = &frames[i];
    }
    /* init user meta */
    frames[i].user_meta_buffer_size =
        impl->pkt_udp_suggest_max_size - sizeof(struct st20_rfc4175_rtp_hdr);
    frames[i].user_meta = mt_rte_zmalloc_socket(frames[i].user_meta_buffer_size, soc_id);
    if (!frames[i].user_meta) {
      err("%s(%d), user_meta malloc %" PRIu64 " fail at %d\n", __func__, idx,
          frames[i].user_meta_buffer_size, i);
      tx_st20p_uinit_src_fbs(ctx);
      return -ENOMEM;
    }
  }
  info("%s(%d), size %" PRIu64 " fmt %d with %u frames\n", __func__, idx, src_size,
       ops->transport_fmt, ctx->framebuff_cnt);
  return 0;
}

static int tx_st20p_get_converter(struct mtl_main_impl* impl, struct st20p_tx_ctx* ctx,
                                  struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st20_get_converter_request req;

  memset(&req, 0, sizeof(req));
  req.device = ops->device;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.interlaced = ops->interlaced;
  req.req.input_fmt = ops->input_fmt;
  req.req.output_fmt = st_frame_fmt_from_transport(ops->transport_fmt);
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.priv = ctx;
  req.get_frame = tx_st20p_convert_get_frame;
  req.put_frame = tx_st20p_convert_put_frame;
  req.dump = tx_st20p_convert_dump;

  struct st20_convert_session_impl* convert_impl = st20_get_converter(impl, &req);
  if (req.device == ST_PLUGIN_DEVICE_TEST_INTERNAL || !convert_impl) {
    struct st_frame_converter* converter = NULL;
    converter = mt_rte_zmalloc_socket(sizeof(*converter), mt_socket_id(impl, MTL_PORT_P));
    if (!converter) {
      err("%s, converter malloc fail\n", __func__);
      return -ENOMEM;
    }
    memset(converter, 0, sizeof(*converter));
    if (st_frame_get_converter(req.req.input_fmt, req.req.output_fmt, converter) < 0) {
      err("%s, get converter fail\n", __func__);
      mt_rte_free(converter);
      return -EIO;
    }
    ctx->internal_converter = converter;
    info("%s(%d), use internal converter\n", __func__, idx);
    return 0;
  }
  ctx->convert_impl = convert_impl;

  return 0;
}

static int st20p_tx_get_block_wait(struct st20p_tx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex, NS_PER_S);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

struct st_frame* st20p_tx_get_frame(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_TX_FRAME_FREE);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    st20p_tx_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        tx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_TX_FRAME_FREE);
  }
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_producer_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  struct st_frame* frame = tx_st20p_user_frame(ctx, framebuff);
  if (ctx->ops.interlaced) { /* init second_field but user still can customize */
    frame->second_field = ctx->second_field;
    ctx->second_field = ctx->second_field ? false : true;
  }
  frame->user_meta = NULL;
  frame->user_meta_size = 0;
  return frame;
}

int st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame* frame) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (ctx->ops.flags & ST20P_TX_FLAG_EXT_FRAME) {
    err("%s(%d), EXT_FRAME flag enabled, use st20p_tx_put_ext_frame instead\n", __func__,
        idx);
    return -EIO;
  }

  framebuff->user_meta_data_size = 0;
  if (frame->user_meta) {
    if (frame->user_meta_size > framebuff->user_meta_buffer_size) {
      err("%s(%d), frame %u user meta size %" PRId64 " too large\n", __func__, idx,
          producer_idx, frame->user_meta_size);
      framebuff->stat = ST20P_TX_FRAME_FREE;
      return -EIO;
    }

    /* copy user meta to framebuff user_meta */
    rte_memcpy(framebuff->user_meta, frame->user_meta, frame->user_meta_size);
    framebuff->user_meta_data_size = frame->user_meta_size;
  }

  if (ctx->ops.interlaced) { /* update second_field */
    framebuff->dst.second_field = framebuff->src.second_field = frame->second_field;
  }

  if (ctx->internal_converter) { /* convert internal */
    ctx->internal_converter->convert_func(&framebuff->src, &framebuff->dst);
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
  } else if (ctx->derive) {
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
  } else {
    framebuff->stat = ST20P_TX_FRAME_READY;
    st20_convert_notify_frame_ready(ctx->convert_impl);
  }

  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);
  return 0;
}

int st20p_tx_put_ext_frame(st20p_tx_handle handle, struct st_frame* frame,
                           struct st_ext_frame* ext_frame) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;
  int ret = 0;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (!(ctx->ops.flags & ST20P_TX_FLAG_EXT_FRAME)) {
    err("%s(%d), EXT_FRAME flag not enabled\n", __func__, idx);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (ctx->ops.interlaced) { /* update second_field */
    framebuff->dst.second_field = framebuff->src.second_field = frame->second_field;
  }

  uint8_t planes = st_frame_fmt_planes(framebuff->src.fmt);
  if (ctx->derive) {
    struct st20_ext_frame trans_ext_frame;
    trans_ext_frame.buf_addr = ext_frame->addr[0];
    trans_ext_frame.buf_iova = ext_frame->iova[0];
    trans_ext_frame.buf_len = ext_frame->size;
    ret = st20_tx_set_ext_frame(ctx->transport, producer_idx, &trans_ext_frame);
    if (ret < 0) {
      err("%s, set ext framebuffer fail %d fb_idx %d\n", __func__, ret, producer_idx);
      return -EIO;
    }
    framebuff->dst.addr[0] = ext_frame->addr[0];
    framebuff->dst.iova[0] = ext_frame->iova[0];
    framebuff->dst.opaque = ext_frame->opaque;
    framebuff->dst.flags |= ST_FRAME_FLAG_EXT_BUF;
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
  } else {
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
      return -EIO;
    }
    if (ctx->internal_converter) { /* convert internal */
      ctx->internal_converter->convert_func(&framebuff->src, &framebuff->dst);
      framebuff->stat = ST20P_TX_FRAME_CONVERTED;
      if (ctx->ops.notify_frame_done)
        ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->src);
    } else {
      framebuff->stat = ST20P_TX_FRAME_READY;
      st20_convert_notify_frame_ready(ctx->convert_impl);
    }
  }

  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);
  return 0;
}

st20p_tx_handle st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops) {
  static int st20p_tx_idx;
  struct mtl_main_impl* impl = mt;
  struct st20p_tx_ctx* ctx;
  int ret;
  int idx = st20p_tx_idx;
  size_t src_size;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  src_size = st_frame_size(ops->input_fmt, ops->width, ops->height, ops->interlaced);
  if (!src_size) {
    err("%s(%d), get src size fail\n", __func__, idx);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), mt_socket_id(impl, MTL_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->derive = st_frame_fmt_equal_transport(ops->input_fmt, ops->transport_fmt);
  ctx->impl = impl;
  ctx->type = MT_ST20_HANDLE_PIPELINE_TX;
  ctx->src_size = src_size;
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  if (ops->flags & ST20P_TX_FLAG_BLOCK_GET) {
    ctx->block_get = true;
  }

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST20P_TX_%d", idx);
  }
  ctx->ops = *ops;

  /* get one suitable convert device */
  if (!ctx->derive) {
    ret = tx_st20p_get_converter(impl, ctx, ops);
    if (ret < 0) {
      err("%s(%d), get converter fail %d\n", __func__, idx, ret);
      st20p_tx_free(ctx);
      return NULL;
    }
  }

  /* init fbs */
  ret = tx_st20p_init_src_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st20p_tx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = tx_st20p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st20p_tx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), transport fmt %s, input fmt: %s, flags 0x%x\n", __func__, idx,
         st20_frame_fmt_name(ops->transport_fmt), st_frame_fmt_name(ops->input_fmt),
         ops->flags);
  st20p_tx_idx++;

  /* notify app can get frame */
  if (!ctx->block_get) tx_st20p_notify_frame_available(ctx);

  return ctx;
}

int st20p_tx_free(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->convert_impl) {
    st20_put_converter(impl, ctx->convert_impl);
    ctx->convert_impl = NULL;
  }

  if (ctx->internal_converter) {
    mt_rte_free(ctx->internal_converter);
    ctx->internal_converter = NULL;
  }

  if (ctx->transport) {
    st20_tx_free(ctx->transport);
    ctx->transport = NULL;
  }
  tx_st20p_uinit_src_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

void* st20p_tx_get_fb_addr(st20p_tx_handle handle, uint16_t idx) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  if (ctx->ops.flags & ST20P_TX_FLAG_EXT_FRAME) {
    err("%s(%d), not known as EXT_FRAME flag enabled\n", __func__, cidx);
    return NULL;
  }

  if (ctx->derive) /* derive dst to src frame */
    return ctx->framebuffs[idx].dst.addr[0];
  return ctx->framebuffs[idx].src.addr[0];
}

size_t st20p_tx_frame_size(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->src_size;
}

int st20p_tx_get_sch_idx(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_get_sch_idx(ctx->transport);
}

int st20p_tx_get_port_stats(st20p_tx_handle handle, enum mtl_session_port port,
                            struct st20_tx_port_status* stats) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_get_port_stats(ctx->transport, port, stats);
}

int st20p_tx_reset_port_stats(st20p_tx_handle handle, enum mtl_session_port port) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_reset_port_stats(ctx->transport, port);
}

int st20p_tx_update_destination(st20p_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_update_destination(ctx->transport, dst);
}

int st20p_tx_wake_block(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) tx_st20p_block_wake(ctx);

  return 0;
}
