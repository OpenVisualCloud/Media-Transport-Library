/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st40_pipeline_tx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st40p_tx_frame_stat_name[ST40P_TX_FRAME_STATUS_MAX] = {
    "free",
    "in_user",
    "ready",
    "in_transmitting",
};

static const char* tx_st40p_stat_name(enum st40p_tx_frame_status stat) {
  return st40p_tx_frame_stat_name[stat];
}

static uint16_t tx_st40p_next_idx(struct st40p_tx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static void tx_st40p_block_wake(struct st40p_tx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void tx_st40p_notify_frame_available(struct st40p_tx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    tx_st40p_block_wake(ctx);
  }
}

static struct st40p_tx_frame* tx_st40p_next_available(
    struct st40p_tx_ctx* ctx, uint16_t idx_start, enum st40p_tx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st40p_tx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = tx_st40p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int tx_st40p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st40_tx_frame_meta* meta) {
  struct st40p_tx_ctx* ctx = priv;
  struct st40p_tx_frame* framebuff;
  MTL_MAY_UNUSED(meta);

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st40p_next_available(ctx, ctx->framebuff_consumer_idx, ST40P_TX_FRAME_READY);
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST40P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;
  /* point to next */
  ctx->framebuff_consumer_idx = tx_st40p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  return 0;
}

static int tx_st40p_frame_done(void* priv, uint16_t frame_idx,
                               struct st40_tx_frame_meta* meta) {
  struct st40p_tx_ctx* ctx = priv;
  struct st40p_tx_frame* framebuff;
  struct st40_frame_info* frame_info;
  int ret;

  framebuff = &ctx->framebuffs[frame_idx];

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST40P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST40P_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  frame_info = &framebuff->frame_info;
  frame_info->tfmt = meta->tfmt;
  frame_info->timestamp = meta->timestamp;
  frame_info->epoch = meta->epoch;
  frame_info->rtp_timestamp = meta->rtp_timestamp;

  if (ctx->ops.notify_frame_done) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, frame_info);
  }

  /* notify app can get frame */
  tx_st40p_notify_frame_available(ctx);

  return ret;
}
static int tx_st40p_asign_anc_frames(struct st40p_tx_ctx* ctx) {
  struct st40p_tx_frame* frames = ctx->framebuffs;
  struct st40_frame_info* frame_info;
  int idx = ctx->idx;
  uint16_t i;

  for (i = 0; i < ctx->framebuff_cnt; i++) {
    frame_info = &frames[i].frame_info;

    frame_info->anc_frame = st40_tx_get_framebuffer(ctx->transport, i);
    if (!frame_info->anc_frame) {
      err("%s(%d), Failed to get framebuffer %u \n", __func__, idx, i);
      return -EIO;
    }
    dbg("%s(%d), fb %p\n", __func__, idx, frame_info->anc_frame);
  }
  return 0;
}

static int tx_st40p_create_transport(struct mtl_main_impl* impl, struct st40p_tx_ctx* ctx,
                                     struct st40p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st40_tx_ops ops_tx = {0};

  ops_tx.name = ops->name;
  ops_tx.priv = ctx;
  ops_tx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  ops_tx.payload_type = ops->port.payload_type;
  ops_tx.ssrc = ops->port.ssrc;

  for (int i = 0; i < ops_tx.num_port; i++) {
    memcpy(ops_tx.dip_addr[i], ops->port.dip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_tx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_tx.udp_src_port[i] = ops->port.udp_src_port[i];
    ops_tx.udp_port[i] = ops->port.udp_port[i];
  }

  if (ops->flags & ST40P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_P][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_P][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST40_TX_FLAG_USER_P_MAC;
  }

  if (ops->flags & ST40P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_R][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_R][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST40_TX_FLAG_USER_R_MAC;
  }

  if (ops->flags & ST40P_TX_FLAG_DEDICATE_QUEUE)
    ops_tx.flags |= ST40_TX_FLAG_DEDICATE_QUEUE;

  if (ops->interlaced) {
    err("%s(%d), interlaced mode not supported\n", __func__, ctx->idx);
    return -EINVAL;
  }

  ops_tx.interlaced = false;
  ops_tx.fps = ops->fps;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  ops_tx.type = ST40_TYPE_FRAME_LEVEL;
  ops_tx.get_next_frame = tx_st40p_next_frame;
  ops_tx.notify_frame_done = tx_st40p_frame_done;

  ctx->transport = st40_tx_create(impl, &ops_tx);
  if (!ctx->transport) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    return -EIO;
  }

  if (tx_st40p_asign_anc_frames(ctx)) {
    err("%s(%d), Failed to asign ancillary frames\n", __func__, idx);
    return -EIO;
  }

  return 0;
}

static int tx_st40p_uinit_fbs(struct st40p_tx_ctx* ctx) {
  if (!ctx->framebuffs) return 0;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    if (ST40P_TX_FRAME_FREE != ctx->framebuffs[i].stat) {
      warn("%s(%d), frame %u is still in %s\n", __func__, ctx->idx, i,
           tx_st40p_stat_name(ctx->framebuffs[i].stat));
    }
  }
  mt_rte_free(ctx->framebuffs);
  ctx->framebuffs = NULL;

  return 0;
}

static int tx_st40p_init_fbs(struct st40p_tx_ctx* ctx, struct st40p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st40p_tx_frame *frames, *framebuff;
  struct st40_frame_info* frame_info;

  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc failed\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    framebuff = &frames[i];
    frame_info = &framebuff->frame_info;
    framebuff->stat = ST40P_TX_FRAME_FREE;
    framebuff->idx = i;

    frame_info->udw_buff_addr = mt_rte_zmalloc_socket(ops->max_udw_buff_size, soc_id);
    if (!frame_info->udw_buff_addr) {
      err("%s(%d), udw_buff malloc failed\n", __func__, idx);
      mt_rte_free(frames);
      return -ENOMEM;
    }
    frame_info->udw_buffer_size = ops->max_udw_buff_size;

    /* addr will be resolved later in tx_st40p_create_transport */
    frame_info->priv = framebuff;
    dbg("%s(%d), init fb %u\n", __func__, idx, i);
  }

  return 0;
}

static int tx_st40p_stat(void* priv) {
  struct st40p_tx_ctx* ctx = priv;
  struct st40p_tx_frame* framebuff = ctx->framebuffs;
  uint16_t producer_idx;
  uint16_t consumer_idx;

  if (!ctx->ready) return -EBUSY; /* not ready */

  producer_idx = ctx->framebuff_producer_idx;
  consumer_idx = ctx->framebuff_consumer_idx;
  notice("TX_st40p(%d,%s), p(%d:%s) c(%d:%s)\n", ctx->idx, ctx->ops_name, producer_idx,
         tx_st40p_stat_name(framebuff[producer_idx].stat), consumer_idx,
         tx_st40p_stat_name(framebuff[consumer_idx].stat));

  notice("TX_st40p(%d), frame get try %d succ %d, put %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  return 0;
}

static int tx_st40p_get_block_wait(struct st40p_tx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);

  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                               ctx->block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);

  return 0;
}

static void tx_st40p_framebuffs_flush(struct st40p_tx_ctx* ctx) {
  struct st40p_tx_frame* framebuff;

  /* wait all frame are in free or in transmitting(flushed by transport) */
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    framebuff = &ctx->framebuffs[i];
    int retry = 0;

    while (retry < 100) {
      if (framebuff->stat == ST40P_TX_FRAME_FREE) break;
      if (framebuff->stat == ST40P_TX_FRAME_IN_TRANSMITTING) {
        /* make sure transport to finish the transmit */
        /* WA to use sleep here, todo: add a transport API to query the stat */
        mt_sleep_ms(50);
        break;
      }
      dbg("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
          tx_st40p_stat_name(framebuff->stat), retry);

      mt_sleep_ms(10);
      retry++;
    }

    if (retry >= 100) {
      info("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
           tx_st40p_stat_name(framebuff->stat), retry);
    }
  }
}

struct st40_frame_info* st40p_tx_get_frame(st40p_tx_handle handle) {
  struct st40p_tx_ctx* ctx = handle;
  struct st40p_tx_frame* framebuff;
  struct st40_frame_info* frame_info;
  int idx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st40p_next_available(ctx, ctx->framebuff_producer_idx, ST40P_TX_FRAME_FREE);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    tx_st40p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        tx_st40p_next_available(ctx, ctx->framebuff_producer_idx, ST40P_TX_FRAME_FREE);
  }

  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST40P_TX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_producer_idx = tx_st40p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  frame_info = &framebuff->frame_info;
  ctx->stat_get_frame_succ++;
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, framebuff->idx,
      frame_info->anc_frame);
  return frame_info;
}

int st40p_tx_put_frame(st40p_tx_handle handle, struct st40_frame_info* frame_info) {
  struct st40p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_tx_frame* framebuff = frame_info->priv;
  uint16_t producer_idx = framebuff->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST40P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (frame_info->anc_frame->data != frame_info->udw_buff_addr) {
    err("%s(%d), frame %u udw_buff_addr %p not match %p\n", __func__, idx, producer_idx,
        frame_info->anc_frame->data, frame_info->udw_buff_addr);
    return -EIO;
  }

  framebuff->stat = ST40P_TX_FRAME_READY;
  ctx->stat_put_frame++;
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, producer_idx, frame_info->anc_frame);
  return 0;
}

int st40p_tx_free(st40p_tx_handle handle) {
  struct st40p_tx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->framebuffs && mt_started(impl)) {
    tx_st40p_framebuffs_flush(ctx);
  }

  if (ctx->ready) {
    mt_stat_unregister(impl, tx_st40p_stat, ctx);
  }

  if (ctx->transport) {
    st40_tx_free(ctx->transport);
    ctx->transport = NULL;
  }
  tx_st40p_uinit_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

st40p_tx_handle st40p_tx_create(mtl_handle mt, struct st40p_tx_ops* ops) {
  static int st40p_tx_idx;
  struct mtl_main_impl* impl = mt;
  struct st40p_tx_ctx* ctx;
  int idx = st40p_tx_idx;
  enum mtl_port port;
  int ret;

  /* validate the input parameters */
  if (!mt || !ops) {
    err("%s(%d), NULL input parameters \n", __func__, idx);
    return NULL;
  }

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (MT_HANDLE_MAIN != impl->type) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST40P_TX_FLAG_FORCE_NUMA) {
    err("%s(%d), force numa not supported\n", __func__, idx);
    return NULL;
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc failed on socket %d\n", __func__, socket);
    return NULL;
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = MT_ST40_HANDLE_PIPELINE_TX;
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  if (ops->flags & ST40P_TX_FLAG_BLOCK_GET) {
    ctx->block_get = true;
  }

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST40P_TX_%d", idx);
  }
  ctx->ops = *ops;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  /* init fbs */
  ret = tx_st40p_init_fbs(ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs failed %d\n", __func__, idx, ret);
    st40p_tx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = tx_st40p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    st40p_tx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), flags 0x%x\n", __func__, idx, ops->flags);
  st40p_tx_idx++;

  /* notify app can get frame */
  if (!ctx->block_get) tx_st40p_notify_frame_available(ctx);

  mt_stat_register(impl, tx_st40p_stat, ctx, ctx->ops_name);

  return ctx;
}

int st40p_tx_update_destination(st40p_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st40_tx_update_destination(ctx->transport, dst);
}

int st40p_tx_wake_block(st40p_tx_handle handle) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) tx_st40p_block_wake(ctx);

  return 0;
}

int st40p_tx_set_block_timeout(st40p_tx_handle handle, uint64_t timedwait_ns) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}

size_t st40p_tx_max_udw_buff_size(st40p_tx_handle handle) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->ops.max_udw_buff_size;
}

void* st40p_tx_get_udw_buff_addr(st40p_tx_handle handle, uint16_t idx) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %u]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].frame_info.udw_buff_addr;
}

void* st40p_tx_get_fb_addr(st40p_tx_handle handle, uint16_t idx) {
  struct st40p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_TX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %u]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].frame_info.anc_frame;
}
