/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_pipeline_tx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static const char* st20p_tx_frame_stat_name[ST20P_TX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_converting", "converted", "in_user", "in_transmitting",
};

static const char* st20p_tx_frame_stat_name_short[ST20P_TX_FRAME_STATUS_MAX] = {
    "F", "R", "IC", "C", "U", "T",
};

static const char* tx_st20p_stat_name(enum st20p_tx_frame_status stat) {
  return st20p_tx_frame_stat_name[stat];
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
    struct st20p_tx_ctx* ctx, enum st20p_tx_frame_status desired) {
  struct st20p_tx_frame* framebuff;

  /* check ready frame from start */
  for (int idx = 0; idx < ctx->framebuff_cnt; idx++) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      return framebuff;
    }
  }

  return NULL;
}

static struct st20p_tx_frame* tx_st20p_newest_available(
    struct st20p_tx_ctx* ctx, enum st20p_tx_frame_status desired) {
  struct st20p_tx_frame* framebuff = NULL;
  struct st20p_tx_frame* framebuff_newest = NULL;

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

static int tx_st20p_next_frame(void* priv, uint16_t* next_frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;
  struct st20p_tx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st20p_newest_available(ctx, ST20P_TX_FRAME_CONVERTED);
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
  mt_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ, frame_idx: %u\n", __func__, ctx->idx, framebuff->idx,
      framebuff->idx);
  MT_USDT_ST20P_TX_FRAME_NEXT(ctx->idx, framebuff->idx);
  return 0;
}

int st20p_tx_late_frame_drop(void* handle, uint64_t epoch_skipped) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;
  struct st20p_tx_frame* framebuff;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (!ctx->ready) return -EBUSY; /* not ready */
  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st20p_newest_available(ctx, ST20P_TX_FRAME_CONVERTED);
  /* not any converted frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST20P_TX_FRAME_FREE;
  ctx->stat_drop_frame++;
  dbg("%s(%d), drop frame %u succ\n", __func__, cidx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_late) {
    ctx->ops.notify_frame_late(ctx->ops.priv, epoch_skipped);
  } else if (ctx->ops.notify_frame_done &&
             !framebuff->frame_done_cb_called) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, tx_st20p_user_frame(ctx, framebuff));
    framebuff->frame_done_cb_called = true;
  }

  /* notify app can get frame */
  tx_st20p_notify_frame_available(ctx);
  MT_USDT_ST20P_TX_FRAME_DROP(cidx, framebuff->idx, framebuff->dst.rtp_timestamp);
  return 0;
}

static int tx_st20p_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;
  int ret;
  struct st20p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  struct st_frame* frame = tx_st20p_user_frame(ctx, framebuff);
  frame->tfmt = meta->tfmt;
  frame->timestamp = meta->timestamp;
  frame->epoch = meta->epoch;
  frame->rtp_timestamp = meta->rtp_timestamp;
  mt_pthread_mutex_lock(&ctx->lock);
  if (ST20P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST20P_TX_FRAME_FREE;
    dbg("%s(%d), frame_idx: %u\n", __func__, ctx->idx, frame_idx);
  } else {
    ret = -EIO;
    err("%s(%d), err status %d for frame %u\n", __func__, ctx->idx, framebuff->stat,
        frame_idx);
  }
  mt_pthread_mutex_unlock(&ctx->lock);

  if (ctx->ops.notify_frame_done &&
      !framebuff->frame_done_cb_called) { /* notify app which frame done */
    ctx->ops.notify_frame_done(ctx->ops.priv, frame);
    framebuff->frame_done_cb_called = true;
  }

  /* notify app can get frame */
  tx_st20p_notify_frame_available(ctx);

  MT_USDT_ST20P_TX_FRAME_DONE(ctx->idx, frame_idx, frame->rtp_timestamp);

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
  framebuff = tx_st20p_newest_available(ctx, ST20P_TX_FRAME_READY);
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_CONVERTING;
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ, frame_idx: %u\n", __func__, idx, framebuff->idx,
      framebuff->idx);
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

  mt_pthread_mutex_lock(&ctx->lock);
  if (ST20P_TX_FRAME_IN_CONVERTING != framebuff->stat) {
    mt_pthread_mutex_unlock(&ctx->lock);
    err("%s(%d), frame %u not in converting %d\n", __func__, idx, convert_idx,
        framebuff->stat);
    return -EIO;
  }

  if ((result < 0) || (data_size <= 0)) {
    dbg("%s(%d), frame %u result %d data_size %" PRIu64 ", frame_idx: %u\n", __func__,
        idx, convert_idx, result, data_size, convert_idx);

    framebuff->stat = ST20P_TX_FRAME_FREE;
    mt_pthread_mutex_unlock(&ctx->lock);

    /* notify app can get frame */
    tx_st20p_notify_frame_available(ctx);
    mt_atomic32_inc(&ctx->stat_convert_fail);
  } else {
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
    mt_pthread_mutex_unlock(&ctx->lock);
  }

  if (ctx->ops.notify_frame_done && !framebuff->frame_done_cb_called) {
    ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->src);
    framebuff->frame_done_cb_called = true;
  }

  return 0;
}

static int tx_st20p_convert_dump(void* priv) {
  struct st20p_tx_ctx* ctx = priv;

  if (!ctx->ready) return -EBUSY; /* not ready */

  int convert_fail = mt_atomic32_read(&ctx->stat_convert_fail);
  mt_atomic32_set(&ctx->stat_convert_fail, 0);
  if (convert_fail) {
    notice("TX_st20p(%s), convert fail %d\n", ctx->ops_name, convert_fail);
  }

  int busy = mt_atomic32_read(&ctx->stat_busy);
  mt_atomic32_set(&ctx->stat_busy, 0);
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
  if (ops->flags & ST20P_TX_FLAG_DROP_WHEN_LATE) {
    ops_tx.notify_frame_late = st20p_tx_late_frame_drop;
  } else if (ops->notify_frame_late) {
    ops_tx.notify_frame_late = ops->notify_frame_late;
  }
  if (ops->flags & ST20P_TX_FLAG_USER_TIMESTAMP)
    ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  if (ops->flags & ST20P_TX_FLAG_ENABLE_VSYNC) ops_tx.flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST20P_TX_FLAG_ENABLE_STATIC_PAD_P)
    ops_tx.flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;
  if (ops->flags & ST20P_TX_FLAG_ENABLE_RTCP) {
    ops_tx.flags |= ST20_TX_FLAG_ENABLE_RTCP;
    ops_tx.rtcp = ops->rtcp;
  }
  if (ops->flags & ST20P_TX_FLAG_EXACT_USER_PACING)
    ops_tx.flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  if (ops->flags & ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH)
    ops_tx.flags |= ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  if (ops->flags & ST20P_TX_FLAG_DISABLE_BULK) ops_tx.flags |= ST20_TX_FLAG_DISABLE_BULK;
  if (ops->flags & ST20P_TX_FLAG_FORCE_NUMA) {
    ops_tx.socket_id = ops->socket_id;
    ops_tx.flags |= ST20_TX_FLAG_FORCE_NUMA;
  }

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
      if (ctx->framebuffs[i].stat != ST20P_TX_FRAME_FREE) {
        warn("%s(%d), frame %u are still in %s\n", __func__, ctx->idx, i,
             tx_st20p_stat_name(ctx->framebuffs[i].stat));
      }
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
  int soc_id = ctx->socket_id;
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
    converter = mt_rte_zmalloc_socket(sizeof(*converter), ctx->socket_id);
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

static int tx_st20p_stat(void* priv) {
  struct st20p_tx_ctx* ctx = priv;
  struct st20p_tx_frame* framebuff = ctx->framebuffs;
  uint16_t status_counts[ST20P_TX_FRAME_STATUS_MAX] = {0};

  if (!ctx->ready) return -EBUSY; /* not ready */

  for (uint16_t j = 0; j < ctx->framebuff_cnt; j++) {
    enum st20p_tx_frame_status stat = framebuff[j].stat;
    if (stat < ST20P_TX_FRAME_STATUS_MAX) {
      status_counts[stat]++;
    }
  }

  char status_str[256];
  int offset = 0;
  for (uint16_t i = 0; i < ST20P_TX_FRAME_STATUS_MAX; i++) {
    if (status_counts[i] > 0) {
      offset += snprintf(status_str + offset, sizeof(status_str) - offset, "%s:%u ",
                         st20p_tx_frame_stat_name_short[i], status_counts[i]);
    }
  }
  notice("TX_st20p(%d,%s), framebuffer queue: %s\n", ctx->idx, ctx->ops_name, status_str);

  notice("TX_st20p(%d), frame get try %d succ %d, put %d, drop %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame,
         ctx->stat_drop_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;
  ctx->stat_drop_frame = 0;

  return 0;
}

static int tx_st20p_usdt_dump_frame(struct st20p_tx_ctx* ctx, struct st_frame* frame) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  struct st20p_tx_ops* ops = &ctx->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st20ptx_s%d_%d_%d_XXXXXX.yuv", idx, ops->width, ops->height);
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
  MT_USDT_ST20P_TX_FRAME_DUMP(idx, usdt_dump_path, frame->addr[0], n);

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static void tx_st20p_framebuffs_flush(struct st20p_tx_ctx* ctx) {
  /* wait all frame are in free or in transmitting(flushed by transport) */
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    struct st20p_tx_frame* framebuff = &ctx->framebuffs[i];
    int retry = 0;

    while (1) {
      if (framebuff->stat == ST20P_TX_FRAME_FREE) break;
      if (framebuff->stat == ST20P_TX_FRAME_IN_TRANSMITTING) {
        /* make sure transport to finish the transmit */
        /* WA to use sleep here, todo: add a transport API to query the stat */
        mt_sleep_ms(50);
        break;
      }

      dbg("%s(%d), frame %u are still in %s, retry %d, frame_idx: %u\n", __func__,
          ctx->idx, i, tx_st20p_stat_name(framebuff->stat), retry, i);
      retry++;
      if (retry > 100) {
        info("%s(%d), frame %u are still in %s, retry %d\n", __func__, ctx->idx, i,
             tx_st20p_stat_name(framebuff->stat), retry);
        break;
      }
      mt_sleep_ms(10);
    }
  }
}

static int st20p_tx_get_block_wait(struct st20p_tx_ctx* ctx) {
  dbg("%s(%d), start\n", __func__, ctx->idx);
  /* wait on the block cond */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  mt_pthread_cond_timedwait_ns(&ctx->block_wake_cond, &ctx->block_wake_mutex,
                               ctx->block_timeout_ns);
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

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff = tx_st20p_next_available(ctx, ST20P_TX_FRAME_FREE);
  if (!framebuff && ctx->block_get) { /* wait here */
    mt_pthread_mutex_unlock(&ctx->lock);
    st20p_tx_get_block_wait(ctx);
    /* get again */
    mt_pthread_mutex_lock(&ctx->lock);
    framebuff = tx_st20p_next_available(ctx, ST20P_TX_FRAME_FREE);
  }
  /* not any free frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_USER;
  framebuff->frame_done_cb_called = false;
  framebuff->seq_number = ctx->framebuff_sequence_number++;
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  struct st_frame* frame = tx_st20p_user_frame(ctx, framebuff);
  if (ctx->ops.interlaced) { /* init second_field but user still can customize */
    frame->second_field = ctx->second_field;
    ctx->second_field = ctx->second_field ? false : true;
  }
  frame->user_meta = NULL;
  frame->user_meta_size = 0;
  ctx->stat_get_frame_succ++;
  MT_USDT_ST20P_TX_FRAME_GET(idx, framebuff->idx, frame->addr[0]);
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
  ctx->stat_put_frame++;

  MT_USDT_ST20P_TX_FRAME_PUT(idx, framebuff->idx, frame->addr[0], framebuff->stat);
  /* check if dump USDT enabled */
  if (MT_USDT_ST20P_TX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ctx->ops.fps) * 5; /* dump every 5s now */
    if ((ctx->usdt_frame_cnt % period) == (period / 2)) {
      tx_st20p_usdt_dump_frame(ctx, frame);
    }
    ctx->usdt_frame_cnt++;
  } else {
    ctx->usdt_frame_cnt = 0;
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
      if (ctx->ops.notify_frame_done && !framebuff->frame_done_cb_called) {
        ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->src);
        framebuff->frame_done_cb_called = true;
      }
    } else {
      framebuff->stat = ST20P_TX_FRAME_READY;
      st20_convert_notify_frame_ready(ctx->convert_impl);
    }
  }
  ctx->stat_put_frame++;

  MT_USDT_ST20P_TX_FRAME_PUT(idx, framebuff->idx, frame->addr[0], framebuff->stat);
  /* check if dump USDT enabled */
  if (MT_USDT_ST20P_TX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ctx->ops.fps) * 5; /* dump every 5s now */
    if ((ctx->usdt_frame_cnt % period) == (period / 2)) {
      tx_st20p_usdt_dump_frame(ctx, frame);
    }
    ctx->usdt_frame_cnt++;
  } else {
    ctx->usdt_frame_cnt = 0;
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

  src_size = st_frame_size(ops->input_fmt, ops->width, ops->height, ops->interlaced);
  if (!src_size) {
    err("%s(%d), get src size fail\n", __func__, idx);
    return NULL;
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST20P_TX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST20P_TX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->derive = st_frame_fmt_equal_transport(ops->input_fmt, ops->transport_fmt);
  ctx->impl = impl;
  ctx->type = MT_ST20_HANDLE_PIPELINE_TX;
  ctx->src_size = src_size;
  mt_atomic32_set(&ctx->stat_convert_fail, 0);
  mt_atomic32_set(&ctx->stat_busy, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
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
         st20_fmt_name(ops->transport_fmt), st_frame_fmt_name(ops->input_fmt),
         ops->flags);
  st20p_tx_idx++;

  /* notify app can get frame */
  if (!ctx->block_get) tx_st20p_notify_frame_available(ctx);

  mt_stat_register(impl, tx_st20p_stat, ctx, ctx->ops_name);

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

  if (ctx->framebuffs && mt_started(impl)) {
    tx_st20p_framebuffs_flush(ctx);
  }

  if (ctx->ready) {
    mt_stat_unregister(impl, tx_st20p_stat, ctx);
  }

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

  return tx_st20p_user_frame(ctx, &ctx->framebuffs[idx])->addr[0];
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

int st20p_tx_get_pacing_params(st20p_tx_handle handle, double* tr_offset_ns,
                               double* trs_ns, uint32_t* vrx_pkts) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }
  ctx = handle;
  cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EINVAL;
  }

  return st20_tx_get_pacing_params(ctx->transport, tr_offset_ns, trs_ns, vrx_pkts);
}

int st20p_tx_get_session_stats(st20p_tx_handle handle, struct st20_tx_user_stats* stats) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_get_session_stats(ctx->transport, stats);
}

int st20p_tx_reset_session_stats(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_tx_reset_session_stats(ctx->transport);
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

int st20p_tx_set_block_timeout(st20p_tx_handle handle, uint64_t timedwait_ns) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}
