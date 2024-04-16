/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 3024 Intel Corporation
 */

#include "st30_pipeline_rx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st30p_rx_frame_stat_name[ST30P_RX_FRAME_STATUS_MAX] = {
    "free",
    "ready",
    "in_user",
};

static const char* rx_st30p_stat_name(enum st30p_rx_frame_status stat) {
  return st30p_rx_frame_stat_name[stat];
}

static uint16_t rx_st30p_next_idx(struct st30p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static void rx_st30p_block_wake(struct st30p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void rx_st30p_notify_frame_available(struct st30p_rx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    rx_st30p_block_wake(ctx);
  }
}

static struct st30p_rx_frame* rx_st30p_next_available(
    struct st30p_rx_ctx* ctx, uint16_t idx_start, enum st30p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st30p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st30p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int rx_st30p_frame_ready(void* priv, void* addr, struct st30_rx_frame_meta* meta) {
  struct st30p_rx_ctx* ctx = priv;
  struct st30p_rx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st30p_next_available(ctx, ctx->framebuff_producer_idx, ST30P_RX_FRAME_FREE);

  /* not any free frame */
  if (!framebuff) {
    ctx->stat_busy++;
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  struct st30_frame* frame = &framebuff->frame;
  frame->addr = addr;
  frame->data_size = meta->frame_recv_size;
  frame->tfmt = meta->tfmt;
  frame->timestamp = meta->timestamp;
  frame->rtp_timestamp = meta->rtp_timestamp;
  framebuff->stat = ST30P_RX_FRAME_READY;
  /* point to next */
  ctx->framebuff_producer_idx = rx_st30p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u(%p) succ\n", __func__, ctx->idx, framebuff->idx, frame->addr);
  /* notify app to a ready frame */
  rx_st30p_notify_frame_available(ctx);

  MT_USDT_ST30P_RX_FRAME_AVAILABLE(ctx->idx, framebuff->idx, frame->addr,
                                   meta->rtp_timestamp, meta->frame_recv_size);
  return 0;
}

static int rx_st30p_create_transport(struct mtl_main_impl* impl, struct st30p_rx_ctx* ctx,
                                     struct st30p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st30_rx_ops ops_rx;
  st30_rx_handle transport;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.ssrc = ops->port.ssrc;
  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.ip_addr[i], ops->port.ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops_rx.mcast_sip_addr[i], ops->port.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_rx.udp_port[i] = ops->port.udp_port[i];
  }

  ops_rx.fmt = ops->fmt;
  ops_rx.channel = ops->channel;
  ops_rx.sampling = ops->sampling;
  ops_rx.ptime = ops->ptime;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.framebuff_size = ops->framebuff_size;
  ops_rx.type = ST30_TYPE_FRAME_LEVEL;
  ops_rx.notify_frame_ready = rx_st30p_frame_ready;

  transport = st30_rx_create(impl, &ops_rx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  return 0;
}

static int rx_st30p_uinit_fbs(struct st30p_rx_ctx* ctx) {
  if (ctx->framebuffs) {
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int rx_st30p_init_fbs(struct mtl_main_impl* impl, struct st30p_rx_ctx* ctx,
                             struct st30p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = mt_socket_id(impl, MTL_PORT_P);
  struct st30p_rx_frame* frames;

  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st30p_rx_frame* framebuff = &frames[i];
    struct st30_frame* frame = &framebuff->frame;

    framebuff->stat = ST30P_RX_FRAME_FREE;
    framebuff->idx = i;

    /* addr will be resolved later in rx_st30p_frame_ready */
    frame->priv = framebuff;
    frame->fmt = ops->fmt;
    frame->channel = ops->channel;
    frame->sampling = ops->sampling;
    frame->ptime = ops->ptime;
    /* same to framebuffer size */
    frame->buffer_size = frame->data_size = ops->framebuff_size;
    dbg("%s(%d), init fb %u\n", __func__, idx, i);
  }

  return 0;
}

static int rx_st30p_stat(void* priv) {
  struct st30p_rx_ctx* ctx = priv;
  struct st30p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  notice("RX_st30p(%d,%s), p(%d:%s) c(%d:%s)\n", ctx->idx, ctx->ops_name, producer_idx,
         rx_st30p_stat_name(framebuff[producer_idx].stat), consumer_idx,
         rx_st30p_stat_name(framebuff[consumer_idx].stat));

  notice("RX_st30p(%d), frame get try %d succ %d, put %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  if (ctx->stat_busy) {
    warn("RX_st30p(%d), stat_busy %d in rx frame ready\n", ctx->idx, ctx->stat_busy);
    ctx->stat_busy = 0;
  }

  return 0;
}

static int st30p_rx_get_block_wait(struct st30p_rx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex, NS_PER_S);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

struct st30_frame* st30p_rx_get_frame(st30p_rx_handle handle) {
  struct st30p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st30p_rx_frame* framebuff;
  struct st30_frame* frame;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);

  framebuff =
      rx_st30p_next_available(ctx, ctx->framebuff_consumer_idx, ST30P_RX_FRAME_READY);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    st30p_rx_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        rx_st30p_next_available(ctx, ctx->framebuff_consumer_idx, ST30P_RX_FRAME_READY);
  }
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST30P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st30p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  frame = &framebuff->frame;
  ctx->stat_get_frame_succ++;
  MT_USDT_ST30P_RX_FRAME_GET(idx, framebuff->idx, frame->addr);
  /* check if dump USDT enabled */
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, framebuff->idx, frame->addr);
  return frame;
}

int st30p_rx_put_frame(st30p_rx_handle handle, struct st30_frame* frame) {
  struct st30p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st30p_rx_frame* framebuff = frame->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST30P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* free the frame */
  st30_rx_put_framebuff(ctx->transport, frame->addr);
  framebuff->stat = ST30P_RX_FRAME_FREE;
  ctx->stat_put_frame++;

  MT_USDT_ST30P_RX_FRAME_PUT(idx, framebuff->idx, frame->addr);
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, consumer_idx, frame->addr);
  return 0;
}

int st30p_rx_free(st30p_rx_handle handle) {
  struct st30p_rx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->ready) {
    mt_stat_unregister(impl, rx_st30p_stat, ctx);
  }

  if (ctx->transport) {
    st30_rx_free(ctx->transport);
    ctx->transport = NULL;
  }
  rx_st30p_uinit_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

st30p_rx_handle st30p_rx_create(mtl_handle mt, struct st30p_rx_ops* ops) {
  static int st30p_rx_idx;
  struct mtl_main_impl* impl = mt;
  struct st30p_rx_ctx* ctx;
  int ret;
  int idx = st30p_rx_idx;

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), mt_socket_id(impl, MTL_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = MT_ST30_HANDLE_PIPELINE_RX;

  mt_pthread_mutex_init(&ctx->lock, NULL);
  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  if (ops->flags & ST30P_RX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST30P_RX_%d", idx);
  }
  ctx->ops = *ops;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  /* init fbs */
  ret = rx_st30p_init_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st30p_rx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = rx_st30p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st30p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), flags 0x%x\n", __func__, idx, ops->flags);
  st30p_rx_idx++;

  if (!ctx->block_get) rx_st30p_notify_frame_available(ctx);

  mt_stat_register(impl, rx_st30p_stat, ctx, ctx->ops_name);

  return ctx;
}

size_t st30p_rx_frame_size(st30p_rx_handle handle) {
  struct st30p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->ops.framebuff_size;
}

int st30p_rx_get_queue_meta(st30p_rx_handle handle, struct st_queue_meta* meta) {
  struct st30p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st30_rx_get_queue_meta(ctx->transport, meta);
}

int st30p_rx_update_source(st30p_rx_handle handle, struct st_rx_source_info* src) {
  struct st30p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st30_rx_update_source(ctx->transport, src);
}

int st30p_rx_wake_block(st30p_rx_handle handle) {
  struct st30p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) rx_st30p_block_wake(ctx);

  return 0;
}
