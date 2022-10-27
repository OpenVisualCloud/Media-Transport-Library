/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_pipeline_tx.h"

#include "../st_log.h"

static const char* st20p_tx_frame_stat_name[ST20P_TX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_converting", "converted", "in_user", "in_transmitting",
};

static const char* tx_st20p_stat_name(enum st20p_tx_frame_status stat) {
  return st20p_tx_frame_stat_name[stat];
}

static inline int convert_yuv422p10le_to_rfc4175_422be10(void* src, void* dst,
                                                         uint32_t width,
                                                         uint32_t height) {
  uint16_t* p10_u16 = (uint16_t*)src;
  return st20_yuv422p10le_to_rfc4175_422be10(
      p10_u16, (p10_u16 + width * height), (p10_u16 + width * height * 3 / 2),
      (struct st20_rfc4175_422_10_pg2_be*)dst, width, height);
}

static inline int convert_v210_to_rfc4175_422be10(void* src, void* dst, uint32_t width,
                                                  uint32_t height) {
  return st20_v210_to_rfc4175_422be10(
      (uint8_t*)src, (struct st20_rfc4175_422_10_pg2_be*)dst, width, height);
}

static uint16_t tx_st20p_next_idx(struct st20p_tx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
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

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_consumer_idx, ST20P_TX_FRAME_CONVERTED);
  /* not any converted frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_TRANSMITTING;
  *next_frame_idx = framebuff->idx;
  if (ctx->ops.flags & (ST20P_TX_FLAG_USER_PACING | ST20P_TX_FLAG_USER_TIMESTAMP)) {
    if (ctx->derive) {
      meta->tfmt = framebuff->dst.tfmt;
      meta->timestamp = framebuff->dst.timestamp;
    } else {
      meta->tfmt = framebuff->src.tfmt;
      meta->timestamp = framebuff->src.timestamp;
    }
  }
  /* point to next */
  ctx->framebuff_consumer_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);
  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  return 0;
}

static int tx_st20p_frame_done(void* priv, uint16_t frame_idx,
                               struct st20_tx_frame_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;
  int ret;
  struct st20p_tx_frame* framebuff = &ctx->framebuffs[frame_idx];

  st_pthread_mutex_lock(&ctx->lock);
  if (ST20P_TX_FRAME_IN_TRANSMITTING == framebuff->stat) {
    ret = 0;
    framebuff->stat = ST20P_TX_FRAME_FREE;
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
    ctx->ops.notify_frame_done(ctx->ops.priv,
                               ctx->derive ? &framebuff->dst : &framebuff->src);
  }

  if (ctx->ops.notify_frame_available) { /* notify app can get frame */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ret;
}

static int tx_st20p_frame_vsync(void* priv, struct st10_vsync_meta* meta) {
  struct st20p_tx_ctx* ctx = priv;

  if (ctx->ops.notify_vsync) {
    ctx->ops.notify_vsync(ctx->ops.priv, meta);
  }

  return 0;
}

static struct st20_convert_frame_meta* tx_st20p_convert_get_frame(void* priv) {
  struct st20p_tx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_convert_idx, ST20P_TX_FRAME_READY);
  /* not any free frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_CONVERTING;
  /* point to next */
  ctx->framebuff_convert_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

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

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_CONVERTING != framebuff->stat) {
    err("%s(%d), frame %u not in converting %d\n", __func__, idx, convert_idx,
        framebuff->stat);
    return -EIO;
  }

  dbg("%s(%d), frame %u result %d data_size %ld\n", __func__, idx, convert_idx, result,
      data_size);
  if ((result < 0) || (data_size <= 0)) {
    info("%s(%d), frame %u result %d data_size %ld\n", __func__, idx, convert_idx, result,
         data_size);
    framebuff->stat = ST20P_TX_FRAME_FREE;
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
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

static int tx_st20p_create_transport(st_handle st, struct st20p_tx_ctx* ctx,
                                     struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st20_tx_ops ops_tx;
  st20_tx_handle transport;

  memset(&ops_tx, 0, sizeof(ops_tx));
  ops_tx.name = ops->name;
  ops_tx.priv = ctx;
  ops_tx.num_port = RTE_MIN(ops->port.num_port, ST_PORT_MAX);
  for (int i = 0; i < ops_tx.num_port; i++) {
    memcpy(ops_tx.dip_addr[i], ops->port.dip_addr[i], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[i], ops->port.port[i], ST_PORT_MAX_LEN);
    ops_tx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST20P_TX_FLAG_USER_P_MAC) {
    memcpy(&ops_tx.tx_dst_mac[ST_PORT_P][0], &ops->tx_dst_mac[ST_PORT_P][0], 6);
    ops_tx.flags |= ST20_TX_FLAG_USER_P_MAC;
  }
  if (ops->flags & ST20P_TX_FLAG_USER_R_MAC) {
    memcpy(&ops_tx.tx_dst_mac[ST_PORT_R][0], &ops->tx_dst_mac[ST_PORT_R][0], 6);
    ops_tx.flags |= ST20_TX_FLAG_USER_R_MAC;
  }
  ops_tx.pacing = ST21_PACING_NARROW;
  ops_tx.width = ops->width;
  ops_tx.height = ops->height;
  ops_tx.fps = ops->fps;
  ops_tx.fmt = ops->transport_fmt;
  ops_tx.payload_type = ops->port.payload_type;
  ops_tx.type = ST20_TYPE_FRAME_LEVEL;
  ops_tx.framebuff_cnt = ops->framebuff_cnt;
  ops_tx.get_next_frame = tx_st20p_next_frame;
  ops_tx.notify_frame_done = tx_st20p_frame_done;
  ops_tx.notify_vsync = tx_st20p_frame_vsync;
  if (ctx->derive && ops->flags & ST20P_TX_FLAG_EXT_FRAME)
    ops_tx.flags |= ST20_TX_FLAG_EXT_FRAME;
  if (ops->flags & ST20P_TX_FLAG_USER_PACING) ops_tx.flags |= ST20_TX_FLAG_USER_PACING;
  if (ops->flags & ST20P_TX_FLAG_USER_TIMESTAMP)
    ops_tx.flags |= ST20_TX_FLAG_USER_TIMESTAMP;

  transport = st20_tx_create(st, &ops_tx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st20p_tx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    if (ctx->derive && ops->flags & ST20P_TX_FLAG_EXT_FRAME) {
      frames[i].dst.addr = NULL;
    } else {
      frames[i].dst.addr = st20_tx_get_framebuffer(transport, i);
    }
    frames[i].dst.fmt = st_frame_fmt_from_transport(ctx->ops.transport_fmt);
    frames[i].dst.buffer_size = st_frame_size(frames[i].dst.fmt, ops->width, ops->height);
    frames[i].dst.data_size = frames[i].dst.buffer_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
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
        if (ctx->framebuffs[i].src.addr) {
          st_rte_free(ctx->framebuffs[i].src.addr);
          ctx->framebuffs[i].src.addr = NULL;
        }
      }
    }
    st_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int tx_st20p_init_src_fbs(struct st_main_impl* impl, struct st20p_tx_ctx* ctx,
                                 struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = st_socket_id(impl, ST_PORT_P);
  struct st20p_tx_frame* frames;
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
    frames[i].stat = ST20P_TX_FRAME_FREE;
    frames[i].idx = i;
    if (!ctx->derive) { /* when derive, no need to alloc src frames */
      if (ops->flags & ST20P_TX_FLAG_EXT_FRAME) {
        frames[i].dst.addr = NULL;
      } else {
        src = st_rte_zmalloc_socket(src_size, soc_id);
        if (!src) {
          err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
          tx_st20p_uinit_src_fbs(ctx);
          return -ENOMEM;
        }
        frames[i].src.addr = src;
      }
      frames[i].src.fmt = ops->input_fmt;
      frames[i].src.buffer_size = src_size;
      frames[i].src.data_size = src_size;
      frames[i].src.width = ops->width;
      frames[i].src.height = ops->height;
      frames[i].src.priv = &frames[i];
    }
  }

  info("%s(%d), size %ld fmt %d with %u frames\n", __func__, idx, src_size,
       ops->transport_fmt, ctx->framebuff_cnt);
  return 0;
}

static int tx_st20p_get_converter_internal(struct st20p_tx_ctx* ctx,
                                           enum st_frame_fmt input_fmt,
                                           enum st_frame_fmt output_fmt) {
  switch (input_fmt) {
    case ST_FRAME_FMT_YUV422PLANAR10LE:
      switch (output_fmt) {
        case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
          ctx->convert_func_internal = convert_yuv422p10le_to_rfc4175_422be10;
          break;
        default:
          err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
              st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
          return -EIO;
      }
      break;
    case ST_FRAME_FMT_V210:
      switch (output_fmt) {
        case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
          ctx->convert_func_internal = convert_v210_to_rfc4175_422be10;
          break;
        default:
          err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
              st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
          return -EIO;
      }
      break;
    default:
      err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
          st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
      return -EIO;
  }

  info("%s(%d), succ, input: %s, output: %s\n", __func__, ctx->idx,
       st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));

  return 0;
}

static int tx_st20p_get_converter(struct st_main_impl* impl, struct st20p_tx_ctx* ctx,
                                  struct st20p_tx_ops* ops) {
  int idx = ctx->idx;
  struct st20_get_converter_request req;

  memset(&req, 0, sizeof(req));
  req.device = ops->device;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.input_fmt = ops->input_fmt;
  req.req.output_fmt = st_frame_fmt_from_transport(ops->transport_fmt);
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.priv = ctx;
  req.get_frame = tx_st20p_convert_get_frame;
  req.put_frame = tx_st20p_convert_put_frame;
  req.dump = tx_st20p_convert_dump;

  if (req.device == ST_PLUGIN_DEVICE_TEST_INTERNAL) {
    info("%s(%d), use internal converter for test\n", __func__, idx);
    return tx_st20p_get_converter_internal(ctx, req.req.input_fmt, req.req.output_fmt);
  }
  struct st20_convert_session_impl* convert_impl = st20_get_converter(impl, &req);
  if (!convert_impl) {
    warn("%s(%d), get converter plugin fail, use internal converter\n", __func__, idx);
    return tx_st20p_get_converter_internal(ctx, req.req.input_fmt, req.req.output_fmt);
  }
  ctx->convert_impl = convert_impl;

  return 0;
}

struct st_frame* st20p_tx_get_frame(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      tx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_TX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_TX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_producer_idx = tx_st20p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  if (ctx->derive) /* derive from dst frame */
    return &framebuff->dst;
  return &framebuff->src;
}

int st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame* frame) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (ctx->convert_func_internal) { /* convert internal */
    ctx->convert_func_internal(framebuff->src.addr, framebuff->dst.addr,
                               framebuff->dst.width, framebuff->dst.height);
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
                           struct st20_ext_frame* ext_frame) {
  struct st20p_tx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_tx_frame* framebuff = frame->priv;
  uint16_t producer_idx = framebuff->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (!(ctx->ops.flags & ST20P_TX_FLAG_EXT_FRAME)) {
    err("%s(%d), EXT_FRAME flag not enabled %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_TX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, producer_idx,
        framebuff->stat);
    return -EIO;
  }

  if (ctx->convert_func_internal) { /* convert internal */
    framebuff->src.addr = ext_frame->buf_addr;
    ctx->convert_func_internal(framebuff->src.addr, framebuff->dst.addr,
                               framebuff->dst.width, framebuff->dst.height);
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
    if (ctx->ops.notify_frame_done)
      ctx->ops.notify_frame_done(ctx->ops.priv, &framebuff->src);
  } else if (ctx->derive) {
    framebuff->dst.addr = ext_frame->buf_addr;
    framebuff->dst.flags |= ST_FRAME_FLAG_EXT_BUF;
    int ret = st20_tx_set_ext_frame(ctx->transport, producer_idx, ext_frame);
    if (ret < 0) {
      err("%s, set ext framebuffer fail %d fb_idx %d\n", __func__, ret, producer_idx);
      return -EIO;
    }
    framebuff->stat = ST20P_TX_FRAME_CONVERTED;
  } else {
    framebuff->src.addr = ext_frame->buf_addr;
    framebuff->src.flags |= ST_FRAME_FLAG_EXT_BUF;
    framebuff->stat = ST20P_TX_FRAME_READY;
    st20_convert_notify_frame_ready(ctx->convert_impl);
  }

  dbg("%s(%d), frame %u succ\n", __func__, idx, producer_idx);
  return 0;
}

st20p_tx_handle st20p_tx_create(st_handle st, struct st20p_tx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st20p_tx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  size_t src_size;

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
    err("%s(%d), get src size fail\n", __func__, idx);
    return NULL;
  }

  ctx = st_rte_zmalloc_socket(sizeof(*ctx), st_socket_id(impl, ST_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->derive = st_frame_fmt_equal_transport(ops->input_fmt, ops->transport_fmt);
  ctx->impl = impl;
  ctx->type = ST20_SESSION_TYPE_PIPELINE_TX;
  ctx->src_size = src_size;
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  st_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
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
  ret = tx_st20p_create_transport(st, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st20p_tx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ctx;
}

int st20p_tx_free(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  struct st_main_impl* impl = ctx->impl;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->convert_impl) {
    st20_put_converter(impl, ctx->convert_impl);
    ctx->convert_impl = NULL;
  }

  if (ctx->transport) {
    st20_tx_free(ctx->transport);
    ctx->transport = NULL;
  }
  tx_st20p_uinit_src_fbs(ctx);

  st_pthread_mutex_destroy(&ctx->lock);
  st_rte_free(ctx);

  return 0;
}

void* st20p_tx_get_fb_addr(st20p_tx_handle handle, uint16_t idx) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }
  if (ctx->derive) /* derive dst to src frame */
    return ctx->framebuffs[idx].dst.addr;
  return ctx->framebuffs[idx].src.addr;
}

size_t st20p_tx_frame_size(st20p_tx_handle handle) {
  struct st20p_tx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_TX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->src_size;
}
