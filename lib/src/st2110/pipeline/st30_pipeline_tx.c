/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "st30_pipeline_tx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st30p_tx_frame_stat_name[ST30P_TX_FRAME_STATUS_MAX] = {
    "free",
    "in_user",
    "ready",
    "in_transmitting",
};

static const char* st30p_tx_frame_stat_name_short[ST30P_TX_FRAME_STATUS_MAX] = {"F", "U",
                                                                                "R", "T"};

static const char* tx_st30p_stat_name(enum st30p_tx_frame_status stat) {
  return st30p_tx_frame_stat_name[stat];
}

static void tx_st30p_block_wake(struct st30p_tx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void tx_st30p_notify_frame_available(struct st30p_tx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    tx_st30p_block_wake(ctx);
  }
}

static struct st30p_tx_frame* tx_st30p_next_available(
    struct st30p_tx_ctx* ctx, enum st30p_tx_frame_status desired) {
  struct st30p_tx_frame* framebuff;

  for (int idx = 0; idx < ctx->framebuff_cnt; idx++) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      return framebuff;
    }
  }

  /* no any desired frame */
  return NULL;
}

static struct st30p_tx_frame* tx_st30p_newest_available(
    struct st30p_tx_ctx* ctx, enum st30p_tx_frame_status desired) {
  struct st30p_tx_frame* framebuff_newest = NULL;

  for (uint16_t idx = 0; idx < ctx->framebuff_cnt; idx++) {
    struct st30p_tx_frame* framebuff = &ctx->framebuffs[idx];
    if ((desired == framebuff->stat) &&
        (!framebuff_newest ||
         !mt_seq32_greater(framebuff->seq_number, framebuff_newest->seq_number))) {
      framebuff_newest = framebuff;
    }
  }

  return framebuff_newest;
}

static int tx_st30p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st30_tx_frame_meta* meta) {
  struct st30p_tx_ctx* ctx = priv;
  struct st30p_tx_frame* framebuff;
  struct st30_frame* frame;
  MTL_MAY_UNUSED(meta);

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st30p_newest_available(ctx, ST30P_TX_FRAME_READY);
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST30P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;

  if (ctx->ops.flags & (ST30P_TX_FLAG_USER_PACING)) {
    frame = &framebuff->frame;
    meta->tfmt = frame->tfmt;
    meta->timestamp = frame->timestamp;
  }

  mt_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  MT_USDT_ST30P_TX_FRAME_NEXT(ctx->idx, framebuff->idx);
  return 0;
}

static int st30p_tx_late_frame_drop(void* handle, uint64_t epoch_skipped) {
  struct st30p_tx_ctx* ctx = handle;
  struct st30p_tx_frame* framebuff;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return 0;
  }

  if (!ctx->ready) return -EBUSY;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st30p_newest_available(ctx, ST30P_TX_FRAME_READY);
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST30P_TX_FRAME_FREE;
  ctx->stat_drop_frame++;
  dbg("%s(%d), drop frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_late) {
    ctx->ops.notify_frame_late(ctx->ops.priv, epoch_skipped);
  } else if (ctx->ops.notify_frame_done) {
    ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->frame);
  }

  tx_st30p_notify_frame_available(ctx);
  MT_USDT_ST30P_TX_FRAME_DONE(ctx->idx, framebuff->idx, framebuff->frame.rtp_timestamp);

  return 0;
}

static int tx_st30p_frame_done(void* priv, uint16_t frame_idx,
                               struct st30_tx_frame_meta* meta) {
  struct st30p_tx_ctx* ctx = priv;
  int ret;
  struct st30p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  struct st30_frame* frame = &framebuff->frame;
  frame->tfmt = meta->tfmt;
  frame->timestamp = meta->timestamp;
  frame->epoch = meta->epoch;
  frame->rtp_timestamp = meta->rtp_timestamp;

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST30P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST30P_TX_FRAME_FREE;
    dbg("%s(%d), done_idx %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_done) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, frame);
  }

  /* notify app can get frame */
  tx_st30p_notify_frame_available(ctx);

  MT_USDT_ST30P_TX_FRAME_DROP(ctx->idx, frame_idx, frame->rtp_timestamp);
  return ret;
}

static int tx_st30p_create_transport(struct mtl_main_impl* impl, struct st30p_tx_ctx* ctx,
                                     struct st30p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st30_tx_ops ops_tx;
  st30_tx_handle transport;

  memset(&ops_tx, 0, sizeof(ops_tx));
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
  if (ops->flags & ST30P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_P][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_P][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST30_TX_FLAG_USER_P_MAC;
  }
  if (ops->flags & ST30P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[MTL_SESSION_PORT_R][0],
           &ops->tx_dst_mac[MTL_SESSION_PORT_R][0], MTL_MAC_ADDR_LEN);
    ops_tx.flags |= ST30_TX_FLAG_USER_R_MAC;
  }
  if (ops->flags & ST30P_TX_FLAG_DEDICATE_QUEUE)
    ops_tx.flags |= ST30_TX_FLAG_DEDICATE_QUEUE;
  if (ops->flags & ST30P_TX_FLAG_FORCE_NUMA) {
    ops_tx.socket_id = ops->socket_id;
    ops_tx.flags |= ST30_TX_FLAG_FORCE_NUMA;
  }
  if (ops->flags & ST30P_TX_FLAG_USER_PACING) ops_tx.flags |= ST30_TX_FLAG_USER_PACING;
  if (ops->flags & ST30P_TX_FLAG_DROP_WHEN_LATE) {
    ops_tx.notify_frame_late = st30p_tx_late_frame_drop;
  } else if (ops->notify_frame_late) {
    ops_tx.notify_frame_late = ops->notify_frame_late;
  }
  ops_tx.pacing_way = ops->pacing_way;
  ops_tx.rtp_timestamp_delta_us = ops->rtp_timestamp_delta_us;

  ops_tx.fmt = ops->fmt;
  ops_tx.channel = ops->channel;
  ops_tx.sampling = ops->sampling;
  ops_tx.ptime = ops->ptime;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  ops_tx.framebuff_size = ops->framebuff_size;
  ops_tx.type = ST30_TYPE_FRAME_LEVEL;
  ops_tx.get_next_frame = tx_st30p_next_frame;
  ops_tx.notify_frame_done = tx_st30p_frame_done;
  ops_tx.rl_accuracy_ns = ops->rl_accuracy_ns;
  ops_tx.rl_offset_ns = ops->rl_offset_ns;
  ops_tx.fifo_size = ops->fifo_size;

  transport = st30_tx_create(impl, &ops_tx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;
  ctx->frames_per_sec = transport->impl->frames_per_sec;

  struct st30p_tx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st30_frame* frame = &frames[i].frame;

    frame->addr = st30_tx_get_framebuffer(transport, i);
    dbg("%s(%d), fb %p\n", __func__, idx, frame->addr);
  }

  return 0;
}

static int tx_st30p_uinit_fbs(struct st30p_tx_ctx* ctx) {
  if (ctx->framebuffs) {
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].stat != ST30P_TX_FRAME_FREE) {
        warn("%s(%d), frame %u are still in %s\n", __func__, ctx->idx, i,
             tx_st30p_stat_name(ctx->framebuffs[i].stat));
      }
    }
    mt_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int tx_st30p_init_fbs(struct st30p_tx_ctx* ctx, struct st30p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st30p_tx_frame* frames;

  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st30p_tx_frame* framebuff = &frames[i];
    struct st30_frame* frame = &framebuff->frame;

    framebuff->stat = ST30P_TX_FRAME_FREE;
    framebuff->idx = i;

    /* addr will be resolved later in tx_st30p_create_transport */
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

static int tx_st30p_stat(void* priv) {
  struct st30p_tx_ctx* ctx = priv;
  struct st30p_tx_frame* framebuff = ctx->framebuffs;
  uint16_t status_counts[ST30P_TX_FRAME_STATUS_MAX] = {0};

  if (!ctx->ready) return -EBUSY; /* not ready */

  for (uint16_t j = 0; j < ctx->framebuff_cnt; j++) {
    enum st30p_tx_frame_status stat = framebuff[j].stat;
    if (stat < ST30P_TX_FRAME_STATUS_MAX) {
      status_counts[stat]++;
    }
  }

  char status_str[256];
  int offset = 0;
  for (uint16_t i = 0; i < ST30P_TX_FRAME_STATUS_MAX; i++) {
    if (status_counts[i] > 0) {
      offset += snprintf(status_str + offset, sizeof(status_str) - offset, "%s:%u ",
                         st30p_tx_frame_stat_name_short[i], status_counts[i]);
    }
  }
  notice("TX_st30p(%d,%s), framebuffer queue: %s\n", ctx->idx, ctx->ops_name, status_str);

  notice("TX_st30p(%d), frame get try %d succ %d, put %d, drop %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame,
         ctx->stat_drop_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;
  ctx->stat_drop_frame = 0;

  return 0;
}

static int tx_st30p_get_block_wait(struct st30p_tx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                               ctx->block_timeout_ns);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

static int tx_st30p_usdt_dump_close(struct st30p_tx_ctx* ctx) {
  int idx = ctx->idx;

  if (ctx->usdt_dump_fd >= 0) {
    info("%s(%d), close fd %d, dumped frames %d\n", __func__, idx, ctx->usdt_dump_fd,
         ctx->usdt_dumped_frames);
    close(ctx->usdt_dump_fd);
    ctx->usdt_dump_fd = -1;
  }
  return 0;
}

static int tx_st30p_usdt_dump_frame(struct st30p_tx_ctx* ctx, struct st30_frame* frame) {
  int idx = ctx->idx;
  int ret;

  if (ctx->usdt_dump_fd < 0) {
    struct st30p_tx_ops* ops = &ctx->ops;
    snprintf(ctx->usdt_dump_path, sizeof(ctx->usdt_dump_path),
             "imtl_usdt_st30ptx_s%d_%d_%d_c%u_XXXXXX.pcm", idx,
             st30_get_sample_rate(ops->sampling), st30_get_sample_size(ops->fmt) * 8,
             ops->channel);
    ret = mt_mkstemps(ctx->usdt_dump_path, strlen(".pcm"));
    if (ret < 0) {
      err("%s(%d), mkstemps %s fail %d\n", __func__, idx, ctx->usdt_dump_path, ret);
      return ret;
    }
    ctx->usdt_dump_fd = ret;
    info("%s(%d), mkstemps succ on %s fd %d\n", __func__, idx, ctx->usdt_dump_path,
         ctx->usdt_dump_fd);
  }

  /* write frame to dump file */
  ssize_t n = write(ctx->usdt_dump_fd, frame->addr, frame->data_size);
  if (n != frame->data_size) {
    warn("%s(%d), write fail %" PRIu64 "\n", __func__, idx, n);
  } else {
    ctx->usdt_dumped_frames++;
    /* logging every 1 sec */
    if ((ctx->usdt_dumped_frames % (ctx->frames_per_sec * 1)) == 0) {
      MT_USDT_ST30P_TX_FRAME_DUMP(idx, ctx->usdt_dump_path, ctx->usdt_dumped_frames);
    }
  }

  return 0;
}

static void tx_st30p_framebuffs_flush(struct st30p_tx_ctx* ctx) {
  /* wait all frame are in free or in transmitting(flushed by transport) */
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st30p_tx_frame* framebuff = &ctx->framebuffs[i];
    int retry = 0;

    while (1) {
      if (framebuff->stat == ST30P_TX_FRAME_FREE) break;
      if (framebuff->stat == ST30P_TX_FRAME_IN_TRANSMITTING) {
        /* make sure transport to finish the transmit */
        /* WA to use sleep here, todo: add a transport API to query the stat */
        mt_sleep_ms(50);
        break;
      }

      dbg("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
          tx_st30p_stat_name(framebuff->stat), retry);
      retry++;
      if (retry > 100) {
        info("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
             tx_st30p_stat_name(framebuff->stat), retry);
        break;
      }
      mt_sleep_ms(10);
    }
  }
  /* Workaround: When tx_st30p_frame_done is called and subsequently framebuff->stat is
   * set to ST30P_TX_FRAME_FREE, data from the framebuffer can still be in transport,
   * already packetized and copied into rte_mbuf, waiting to be sent.
   * TODO: add synchronization mechanism to ensure all data is sent before freeing the
   * session.
   */
  mt_sleep_ms(50);
}

struct st30_frame* st30p_tx_get_frame(st30p_tx_handle handle) {
  struct st30p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st30p_tx_frame* framebuff;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st30p_next_available(ctx, ST30P_TX_FRAME_FREE);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    tx_st30p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff = tx_st30p_next_available(ctx, ST30P_TX_FRAME_FREE);
  }
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST30P_TX_FRAME_IN_USER;
  framebuff->seq_number = ctx->framebuff_seq_number++;
  mt_pthread_mutex_unlock(&ctx->lock);

  struct st30_frame* frame = &framebuff->frame;
  ctx->stat_get_frame_succ++;
  MT_USDT_ST30P_TX_FRAME_GET(idx, framebuff->idx, frame->addr);
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, framebuff->idx, frame->addr);
  /* check if dump USDT enabled */
  if (MT_USDT_ST30P_TX_FRAME_DUMP_ENABLED()) {
    tx_st30p_usdt_dump_frame(ctx, frame);
  } else {
    tx_st30p_usdt_dump_close(ctx);
  }
  return frame;
}

int st30p_tx_put_frame(st30p_tx_handle handle, struct st30_frame* frame) {
  struct st30p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st30p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST30P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EIO;
  }

  framebuff->stat = ST30P_TX_FRAME_READY;
  ctx->stat_put_frame++;
  MT_USDT_ST30P_TX_FRAME_PUT(idx, framebuff->idx, frame->addr);
  dbg("%s(%d), frame %u(%p) succ\n", __func__, idx, producer_idx, frame->addr);
  mt_pthread_mutex_unlock(&ctx->lock);
  return 0;
}

int st30p_tx_free(st30p_tx_handle handle) {
  struct st30p_tx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->framebuffs && mt_started(impl)) {
    tx_st30p_framebuffs_flush(ctx);
  }

  if (ctx->ready) {
    mt_stat_unregister(impl, tx_st30p_stat, ctx);
  }

  if (ctx->transport) {
    st30_tx_free(ctx->transport);
    ctx->transport = NULL;
  }
  tx_st30p_uinit_fbs(ctx);

  tx_st30p_usdt_dump_close(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

st30p_tx_handle st30p_tx_create(mtl_handle mt, struct st30p_tx_ops* ops) {
  static int st30p_tx_idx;
  struct mtl_main_impl* impl = mt;
  struct st30p_tx_ctx* ctx;
  int ret;
  int idx = st30p_tx_idx;

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

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST30P_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST30P_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = MT_ST30_HANDLE_PIPELINE_TX;
  ctx->usdt_dump_fd = -1;
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  if (ops->flags & ST30P_TX_FLAG_BLOCK_GET) {
    ctx->block_get = true;
  }

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST30P_TX_%d", idx);
  }
  ctx->ops = *ops;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  /* init fbs */
  ret = tx_st30p_init_fbs(ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st30p_tx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = tx_st30p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st30p_tx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), flags 0x%x\n", __func__, idx, ops->flags);
  st30p_tx_idx++;

  /* notify app can get frame */
  if (!ctx->block_get) tx_st30p_notify_frame_available(ctx);

  mt_stat_register(impl, tx_st30p_stat, ctx, ctx->ops_name);

  return ctx;
}

int st30p_tx_update_destination(st30p_tx_handle handle, struct st_tx_dest_info* dst) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st30_tx_update_destination(ctx->transport, dst);
}

int st30p_tx_wake_block(st30p_tx_handle handle) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) tx_st30p_block_wake(ctx);

  return 0;
}

int st30p_tx_set_block_timeout(st30p_tx_handle handle, uint64_t timedwait_ns) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}

size_t st30p_tx_frame_size(st30p_tx_handle handle) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->ops.framebuff_size;
}

void* st30p_tx_get_fb_addr(st30p_tx_handle handle, uint16_t idx) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %u]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].frame.addr;
}

int st30p_tx_get_session_stats(st30p_tx_handle handle, struct st30_tx_user_stats* stats) {
  struct st30p_tx_ctx* ctx;
  int cidx;
  struct st30p_tx_frame* framebuff;
  uint16_t status_counts[ST30P_TX_FRAME_STATUS_MAX] = {0};

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  ctx = handle;
  cidx = ctx->idx;
  framebuff = ctx->framebuffs;

  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EINVAL;
  }

  for (uint16_t j = 0; j < ctx->framebuff_cnt; j++) {
    enum st30p_tx_frame_status stat = framebuff[j].stat;
    if (stat < ST30P_TX_FRAME_STATUS_MAX) {
      status_counts[stat]++;
    }
  }

  char status_str[256];
  int offset = 0;
  for (uint16_t i = 0; i < ST30P_TX_FRAME_STATUS_MAX; i++) {
    if (status_counts[i] > 0) {
      offset += snprintf(status_str + offset, sizeof(status_str) - offset, "%s:%u ",
                         st30p_tx_frame_stat_name_short[i], status_counts[i]);
    }
  }
  notice("TX_st30p(%d,%s), framebuffer queue: %s\n", ctx->idx, ctx->ops_name, status_str);

  return st30_tx_get_session_stats(ctx->transport, stats);
}

int st30p_tx_reset_session_stats(st30p_tx_handle handle) {
  struct st30p_tx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST30_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st30_tx_reset_session_stats(ctx->transport);
}
