/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#include "st40_pipeline_rx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st40p_rx_frame_stat_name[ST40P_RX_FRAME_STATUS_MAX] = {
    "free", "receiving", "pending", "ready", "in_user",
};

static const char* rx_st40p_stat_name(enum st40p_rx_frame_status stat) {
  return st40p_rx_frame_stat_name[stat];
}

static uint16_t rx_st40p_next_idx(struct st40p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static void rx_st40p_block_wake(struct st40p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  ctx->block_wake_pending = true;
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void rx_st40p_notify_frame_available(struct st40p_rx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    rx_st40p_block_wake(ctx);
  }
}

static struct st40p_rx_frame* rx_st40p_next_available(
    struct st40p_rx_ctx* ctx, uint16_t idx_start, enum st40p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st40p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st40p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

/* T6: FRAME_LEVEL transport delivery callback.  Transport hands us a fully
 * assembled frame (UDW buffer + meta).  We claim a FREE pipeline framebuff,
 * point it at the transport-owned UDW buffer (zero-copy) and copy meta into
 * the pipeline-owned meta[] array. */
static int rx_st40p_frame_ready(void* priv, void* addr, struct st40_rx_frame_meta* meta) {
  struct st40p_rx_ctx* ctx = priv;
  struct st40p_rx_frame* framebuff;
  struct st40_frame_info* frame_info;

  if (!ctx->ready) return -EBUSY;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st40p_next_available(ctx, ctx->framebuff_producer_idx, ST40P_RX_FRAME_FREE);
  if (!framebuff) {
    ctx->stat_busy++;
    __atomic_fetch_add(&ctx->stat_frames_dropped, 1, __ATOMIC_RELAXED);
    mt_pthread_mutex_unlock(&ctx->lock);
    /* returning <0 makes the transport reclaim the slot to FREE */
    return -EBUSY;
  }

  frame_info = &framebuff->frame_info;
  frame_info->udw_buff_addr = (uint8_t*)addr;
  frame_info->udw_buffer_fill = meta->udw_buffer_fill;
  frame_info->meta_num = meta->meta_num;
  if (meta->meta_num && meta->meta) {
    uint32_t copy = meta->meta_num > ST40_MAX_META ? ST40_MAX_META : meta->meta_num;
    memcpy(frame_info->meta, meta->meta, sizeof(struct st40_meta) * copy);
  }
  frame_info->pkts_total = meta->pkts_total;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    frame_info->pkts_recv[i] = meta->pkts_recv[i];
    frame_info->port_seq_lost[i] = meta->port_seq_lost[i];
    frame_info->port_seq_discont[i] = meta->port_seq_discont[i];
  }
  frame_info->seq_lost = meta->seq_lost;
  frame_info->seq_discont = meta->seq_discont;
  frame_info->rtp_marker = meta->rtp_marker;
  frame_info->interlaced = meta->interlaced;
  frame_info->second_field = meta->second_field;
  frame_info->tfmt = meta->tfmt;
  frame_info->rtp_timestamp = meta->rtp_timestamp;
  frame_info->timestamp = meta->timestamp;
  frame_info->receive_timestamp = meta->timestamp_first_pkt;
  frame_info->epoch = 0;
  frame_info->status = meta->status;

  framebuff->stat = ST40P_RX_FRAME_READY;
  ctx->framebuff_producer_idx = rx_st40p_next_idx(ctx, framebuff->idx);
  __atomic_fetch_add(&ctx->stat_frames_received, 1, __ATOMIC_RELAXED);
  if (frame_info->seq_discont)
    __atomic_fetch_add(&ctx->stat_frames_corrupted, 1, __ATOMIC_RELAXED);
  mt_pthread_mutex_unlock(&ctx->lock);

  rx_st40p_notify_frame_available(ctx);
  MT_USDT_ST40P_RX_FRAME_AVAILABLE(ctx->idx, framebuff->idx, frame_info->meta_num);
  return 0;
}

static int rx_st40p_create_transport(struct mtl_main_impl* impl, struct st40p_rx_ctx* ctx,
                                     struct st40p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st40_rx_ops ops_rx;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, MTL_SESSION_PORT_MAX);
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.ssrc = ops->port.ssrc;
  ops_rx.interlaced = ops->interlaced;

  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.ip_addr[i], ops->port.ip_addr[i], MTL_IP_ADDR_LEN);
    memcpy(ops_rx.mcast_sip_addr[i], ops->port.mcast_sip_addr[i], MTL_IP_ADDR_LEN);
    snprintf(ops_rx.port[i], MTL_PORT_MAX_LEN, "%s", ops->port.port[i]);
    ops_rx.udp_port[i] = ops->port.udp_port[i];

    enum mtl_port phy = mt_port_by_name(impl, ops->port.port[i]);
    ctx->port_map[i] = phy;
    ctx->port_id[i] = mt_port_id(impl, phy);
  }

  /* T6: switch to FRAME_LEVEL transport — assembly + seq stats live in
   * st_rx_ancillary_session.  Pipeline only routes the assembled frame. */
  ops_rx.type = ST40_TYPE_FRAME_LEVEL;
  ops_rx.framebuff_cnt = ctx->framebuff_cnt;
  ops_rx.framebuff_size = ops->max_udw_buff_size;
  ops_rx.notify_frame_ready = rx_st40p_frame_ready;

  if (ops->flags & ST40P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST40_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST40P_RX_FLAG_ENABLE_RTCP) ops_rx.flags |= ST40_RX_FLAG_ENABLE_RTCP;
  if (ops->flags & ST40P_RX_FLAG_DISABLE_AUTO_DETECT)
    ops_rx.flags |= ST40_RX_FLAG_DISABLE_AUTO_DETECT;

  ctx->transport = st40_rx_create(impl, &ops_rx);
  if (!ctx->transport) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    return -EIO;
  }

  return 0;
}

static int rx_st40p_uinit_fbs(struct st40p_rx_ctx* ctx) {
  if (!ctx->framebuffs) return 0;

  /* T6: UDW buffers are owned by the transport pool; nothing to free here. */
  mt_rte_free(ctx->framebuffs);
  ctx->framebuffs = NULL;

  return 0;
}

static int rx_st40p_init_fbs(struct st40p_rx_ctx* ctx, struct st40p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st40p_rx_frame *frames, *framebuff;
  struct st40_frame_info* frame_info;

  if (!ops->max_udw_buff_size) {
    err("%s(%d), invalid max_udw_buff_size %u\n", __func__, idx, ops->max_udw_buff_size);
    return -EINVAL;
  }

  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc failed\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    framebuff = &frames[i];
    frame_info = &framebuff->frame_info;
    framebuff->stat = ST40P_RX_FRAME_FREE;
    framebuff->idx = i;

    /* T6: udw_buff_addr is bound at frame_ready time to the transport's
     * pool slot; pipeline does not allocate per-frame UDW buffers. */
    frame_info->udw_buff_addr = NULL;
    frame_info->udw_buffer_size = ops->max_udw_buff_size;
    frame_info->udw_buffer_fill = 0;
    frame_info->meta_num = 0;
    frame_info->meta = framebuff->meta;
    frame_info->pkts_total = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_P] = 0;
    frame_info->pkts_recv[MTL_SESSION_PORT_R] = 0;
    frame_info->port_seq_lost[MTL_SESSION_PORT_P] = 0;
    frame_info->port_seq_lost[MTL_SESSION_PORT_R] = 0;
    frame_info->port_seq_discont[MTL_SESSION_PORT_P] = false;
    frame_info->port_seq_discont[MTL_SESSION_PORT_R] = false;
    frame_info->seq_discont = false;
    frame_info->seq_lost = 0;
    frame_info->rtp_marker = false;
    frame_info->receive_timestamp = 0;
    frame_info->second_field = false;
    frame_info->interlaced = false;
    frame_info->priv = framebuff;

    dbg("%s(%d), init fb %u\n", __func__, idx, i);
  }

  info("%s(%d), max_udw_buff_size %u with %u frames\n", __func__, idx,
       ops->max_udw_buff_size, ctx->framebuff_cnt);

  return 0;
}

static int rx_st40p_stat(void* priv) {
  struct st40p_rx_ctx* ctx = priv;
  struct st40p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx;
  uint16_t consumer_idx;
  enum st40p_rx_frame_status producer_stat;
  enum st40p_rx_frame_status consumer_stat;

  mt_pthread_mutex_lock(&ctx->lock);
  producer_idx = ctx->framebuff_producer_idx;
  consumer_idx = ctx->framebuff_consumer_idx;
  producer_stat = framebuff[producer_idx].stat;
  consumer_stat = framebuff[consumer_idx].stat;
  mt_pthread_mutex_unlock(&ctx->lock);

  notice("RX_st40p(%d,%s), p(%d:%s) c(%d:%s)\n", ctx->idx, ctx->ops_name, producer_idx,
         rx_st40p_stat_name(producer_stat), consumer_idx,
         rx_st40p_stat_name(consumer_stat));

  notice("RX_st40p(%d), frame get try %d succ %d, put %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);

  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  if (ctx->stat_busy) {
    notice("RX_st40p(%d), busy %d\n", ctx->idx, ctx->stat_busy);
    ctx->stat_busy = 0;
  }

  return 0;
}

static int rx_st40p_get_block_wait(struct st40p_rx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  while (!ctx->block_wake_pending) {
    int ret = mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                                           ctx->block_timeout_ns);
    if (ret) break;
  }
  ctx->block_wake_pending = false;
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
  dbg("%s(%d), end\n", __func__, ctx->idx);
  return 0;
}

static int rx_st40p_usdt_dump_frame(struct st40p_rx_ctx* ctx,
                                    struct st40_frame_info* frame_info) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path), "imtl_usdt_st40prx_s%d_%d_XXXXXX.bin",
           idx, ctx->usdt_dump_frame_cnt);
  fd = mt_mkstemps(usdt_dump_path, strlen(".bin"));
  if (fd < 0) {
    err("%s(%d), mkstemps %s fail %d\n", __func__, idx, usdt_dump_path, fd);
    return fd;
  }

  /* write UDW data to dump file */
  ssize_t n = write(fd, frame_info->udw_buff_addr, frame_info->udw_buffer_fill);
  if (n != frame_info->udw_buffer_fill) {
    warn("%s(%d), write fail %" PRId64 "\n", __func__, idx, (int64_t)n);
  }
  MT_USDT_ST40P_RX_FRAME_DUMP(idx, usdt_dump_path, frame_info->meta_num, n);

  info("%s(%d), write %" PRId64 " to %s(fd:%d), time %fms\n", __func__, idx, (int64_t)n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  ctx->usdt_dump_frame_cnt++;
  close(fd);
  return 0;
}

struct st40_frame_info* st40p_rx_get_frame(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_rx_frame* framebuff;
  struct st40_frame_info* frame_info;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);

  framebuff =
      rx_st40p_next_available(ctx, ctx->framebuff_consumer_idx, ST40P_RX_FRAME_READY);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st40p_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff =
        rx_st40p_next_available(ctx, ctx->framebuff_consumer_idx, ST40P_RX_FRAME_READY);
  }

  /* not any ready frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST40P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st40p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  frame_info = &framebuff->frame_info;
  ctx->stat_get_frame_succ++;
  MT_USDT_ST40P_RX_FRAME_GET(idx, framebuff->idx, frame_info->meta_num);
  dbg("%s(%d), frame %u succ, meta_num %u\n", __func__, idx, framebuff->idx,
      frame_info->meta_num);

  /* check if dump USDT enabled */
  if (MT_USDT_ST40P_RX_FRAME_DUMP_ENABLED()) {
    rx_st40p_usdt_dump_frame(ctx, frame_info);
  }

  return frame_info;
}

int st40p_rx_put_frame(st40p_rx_handle handle, struct st40_frame_info* frame_info) {
  struct st40p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_rx_frame* framebuff = frame_info->priv;
  uint16_t consumer_idx = framebuff->idx;
  uint16_t meta_num_before_reset = frame_info->meta_num;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST40P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* T6: release the transport-owned UDW slot back to the transport pool. */
  if (frame_info->udw_buff_addr) {
    st40_rx_put_framebuff(ctx->transport, frame_info->udw_buff_addr);
    frame_info->udw_buff_addr = NULL;
  }

  /* reset frame for reuse */
  frame_info->meta_num = 0;
  frame_info->udw_buffer_fill = 0;
  frame_info->pkts_total = 0;
  frame_info->pkts_recv[MTL_SESSION_PORT_P] = 0;
  frame_info->pkts_recv[MTL_SESSION_PORT_R] = 0;
  frame_info->port_seq_lost[MTL_SESSION_PORT_P] = 0;
  frame_info->port_seq_lost[MTL_SESSION_PORT_R] = 0;
  frame_info->port_seq_discont[MTL_SESSION_PORT_P] = false;
  frame_info->port_seq_discont[MTL_SESSION_PORT_R] = false;
  frame_info->seq_discont = false;
  frame_info->seq_lost = 0;
  frame_info->rtp_marker = false;
  frame_info->receive_timestamp = 0;
  frame_info->second_field = false;
  frame_info->interlaced = false;
  framebuff->stat = ST40P_RX_FRAME_FREE;
  ctx->stat_put_frame++;

  MT_USDT_ST40P_RX_FRAME_PUT(idx, consumer_idx, meta_num_before_reset);
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);
  return 0;
}

int st40p_rx_put_frame_abort(st40p_rx_handle handle, struct st40_frame_info* frame_info) {
  struct st40p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st40p_rx_frame* framebuff = frame_info->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST40P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* T6: release the transport-owned UDW slot back to the transport pool. */
  if (frame_info->udw_buff_addr) {
    st40_rx_put_framebuff(ctx->transport, frame_info->udw_buff_addr);
    frame_info->udw_buff_addr = NULL;
  }

  /* reset frame for reuse without processing */
  frame_info->meta_num = 0;
  frame_info->udw_buffer_fill = 0;
  framebuff->stat = ST40P_RX_FRAME_FREE;
  dbg("%s(%d), frame %u aborted\n", __func__, idx, consumer_idx);
  return 0;
}

int st40p_rx_free(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  struct mtl_main_impl* impl;

  if (!handle) {
    err("%s, invalid handle\n", __func__);
    return -EINVAL;
  }

  impl = ctx->impl;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->ready) {
    mt_stat_unregister(impl, rx_st40p_stat, ctx);
  }

  if (ctx->transport) {
    st40_rx_free(ctx->transport);
    ctx->transport = NULL;
  }

  rx_st40p_uinit_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

st40p_rx_handle st40p_rx_create(mtl_handle mt, struct st40p_rx_ops* ops) {
  static int st40p_rx_idx;
  struct mtl_main_impl* impl = mt;
  struct st40p_rx_ctx* ctx;
  int ret;
  int idx = st40p_rx_idx;

  /* validate the input parameters */
  if (!mt || !ops) {
    err("%s, NULL input parameters\n", __func__);
    return NULL;
  }

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));

  if (MT_HANDLE_MAIN != impl->type) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST40P_RX_FLAG_FORCE_NUMA) {
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
  ctx->type = MT_ST40_HANDLE_PIPELINE_RX;
  for (int i = 0; i < MTL_SESSION_PORT_MAX; i++) {
    ctx->port_map[i] = MTL_PORT_MAX;
    ctx->port_id[i] = UINT16_MAX;
  }

  mt_pthread_mutex_init(&ctx->lock, NULL);
  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  ctx->block_wake_pending = false;
  if (ops->flags & ST40P_RX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST40P_RX_%d", idx);
  }
  ctx->ops = *ops;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  /* init fbs */
  ret = rx_st40p_init_fbs(ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs failed %d\n", __func__, idx, ret);
    st40p_rx_free(ctx);
    return NULL;
  }

  /* create transport handle */
  ret = rx_st40p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), Failed to create transport\n", __func__, idx);
    st40p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), flags 0x%x\n", __func__, idx, ops->flags);
  st40p_rx_idx++;

  if (!ctx->block_get) rx_st40p_notify_frame_available(ctx);

  mt_stat_register(impl, rx_st40p_stat, ctx, ctx->ops_name);

  return ctx;
}

size_t st40p_rx_max_udw_buff_size(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->ops.max_udw_buff_size;
}

int st40p_rx_get_queue_meta(st40p_rx_handle handle, struct st_queue_meta* meta) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_get_queue_meta(ctx->transport, meta);
}

int st40p_rx_get_session_stats(st40p_rx_handle handle, struct st40_rx_user_stats* stats) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST40_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  int ret = st40_rx_get_session_stats(ctx->transport, stats);
  if (ret < 0) return ret;
  /* Overlay pipeline-tracked frame-level counters; transport never sets these. */
  stats->common.stat_frames_received =
      __atomic_load_n(&ctx->stat_frames_received, __ATOMIC_RELAXED);
  stats->common.stat_frames_dropped =
      __atomic_load_n(&ctx->stat_frames_dropped, __ATOMIC_RELAXED);
  stats->common.stat_frames_corrupted =
      __atomic_load_n(&ctx->stat_frames_corrupted, __ATOMIC_RELAXED);
  return 0;
}

int st40p_rx_reset_session_stats(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST40_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  __atomic_store_n(&ctx->stat_frames_received, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&ctx->stat_frames_dropped, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&ctx->stat_frames_corrupted, 0, __ATOMIC_RELAXED);
  return st40_rx_reset_session_stats(ctx->transport);
}

int st40p_rx_update_source(st40p_rx_handle handle, struct st_rx_source_info* src) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st40_rx_update_source(ctx->transport, src);
}

int st40p_rx_wake_block(st40p_rx_handle handle) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  if (ctx->block_get) rx_st40p_block_wake(ctx);

  return 0;
}

int st40p_rx_set_block_timeout(st40p_rx_handle handle, uint64_t timedwait_ns) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}

void* st40p_rx_get_udw_buff_addr(st40p_rx_handle handle, uint16_t idx) {
  struct st40p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (MT_ST40_HANDLE_PIPELINE_RX != ctx->type) {
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
