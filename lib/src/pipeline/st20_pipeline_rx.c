/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_pipeline_rx.h"

#include "../st_log.h"

static const char* st20p_rx_frame_stat_name[ST20P_RX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_converting", "converted", "in_user",
};

static const char* rx_st20p_stat_name(enum st20p_rx_frame_status stat) {
  return st20p_rx_frame_stat_name[stat];
}

static inline int convert_rfc4175_422be10_to_422le8(void* src, void* dst, uint32_t width,
                                                    uint32_t height) {
  return st20_rfc4175_422be10_to_422le8((struct st20_rfc4175_422_10_pg2_be*)src,
                                        (struct st20_rfc4175_422_8_pg2_le*)dst, width,
                                        height);
}

static inline int convert_rfc4175_422be10_to_v210(void* src, void* dst, uint32_t width,
                                                  uint32_t height) {
  return st20_rfc4175_422be10_to_v210((struct st20_rfc4175_422_10_pg2_be*)src,
                                      (uint8_t*)dst, width, height);
}

static inline int convert_rfc4175_422be10_to_y210(void* src, void* dst, uint32_t width,
                                                  uint32_t height) {
  return st20_rfc4175_422be10_to_y210((struct st20_rfc4175_422_10_pg2_be*)src,
                                      (uint16_t*)dst, width, height);
}

static inline int convert_rfc4175_422be10_to_yuv422p10le(void* src, void* dst,
                                                         uint32_t width,
                                                         uint32_t height) {
  uint16_t* p10_u16 = (uint16_t*)dst;
  return st20_rfc4175_422be10_to_yuv422p10le(
      (struct st20_rfc4175_422_10_pg2_be*)src, p10_u16, (p10_u16 + width * height),
      (p10_u16 + width * height * 3 / 2), width, height);
}

static inline int convert_rfc4175_422be12_to_yuv422p12le(void* src, void* dst,
                                                         uint32_t width,
                                                         uint32_t height) {
  uint16_t* p12_u16 = (uint16_t*)dst;
  return st20_rfc4175_422be12_to_yuv422p12le(
      (struct st20_rfc4175_422_12_pg2_be*)src, p12_u16, (p12_u16 + width * height),
      (p12_u16 + width * height * 3 / 2), width, height);
}

static inline int convert_rfc4175_444be10_to_yuv444p10le(void* src, void* dst,
                                                         uint32_t width,
                                                         uint32_t height) {
  uint16_t* p10_u16 = (uint16_t*)dst;
  return st20_rfc4175_444be10_to_yuv444p10le(
      (struct st20_rfc4175_444_10_pg4_be*)src, p10_u16, (p10_u16 + width * height),
      (p10_u16 + width * height * 2), width, height);
}

static inline int convert_rfc4175_444be10_to_gbrp10le(void* src, void* dst,
                                                      uint32_t width, uint32_t height) {
  uint16_t* p10_u16 = (uint16_t*)dst;
  return st20_rfc4175_444be10_to_gbrp10le((struct st20_rfc4175_444_10_pg4_be*)src,
                                          p10_u16, (p10_u16 + width * height),
                                          (p10_u16 + width * height * 2), width, height);
}

static uint16_t rx_st20p_next_idx(struct st20p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static struct st20p_rx_frame* rx_st20p_next_available(
    struct st20p_rx_ctx* ctx, uint16_t idx_start, enum st20p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st20p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st20p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int rx_st20p_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->src.addr = frame;
  framebuff->src.data_size = meta->frame_total_size;
  framebuff->src.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->src.status = meta->status;

  /* ask app to consume src frame directly */
  if (ctx->derive) {
    framebuff->dst = framebuff->src;
    framebuff->stat = ST20P_RX_FRAME_CONVERTED;
    /* point to next */
    ctx->framebuff_producer_idx = rx_st20p_next_idx(ctx, framebuff->idx);
    st_pthread_mutex_unlock(&ctx->lock);
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
    return 0;
  }

  framebuff->dst.tfmt = meta->tfmt;
  framebuff->dst.status = meta->status;
  /* set dst timestamp to same as src? */
  framebuff->dst.timestamp = meta->timestamp;
  framebuff->stat = ST20P_RX_FRAME_READY;

  /* point to next */
  ctx->framebuff_producer_idx = rx_st20p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);

  /* ask convert plugin to consume */
  if (ctx->convert_impl) st20_convert_notify_frame_ready(ctx->convert_impl);

  /* or ask app to consume with internal converter */
  if (ctx->convert_func_internal) {
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
  }

  return 0;
}

static int rx_st20p_query_ext_frame(void* priv, struct st20_ext_frame* ext_frame,
                                    struct st20_rx_frame_meta* meta) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff;
  int ret;

  if (!ctx->ready) return -EBUSY; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  ret = ctx->ops.query_ext_frame(ctx->ops.priv, ext_frame, meta);
  if (ret < 0) {
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }
  framebuff->src.opaque = ext_frame->opaque;
  st_pthread_mutex_unlock(&ctx->lock);

  return 0;
}

static int rx_st20p_notify_event(void* priv, enum st_event event, void* args) {
  struct st20p_rx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static struct st20_convert_frame_meta* rx_st20p_convert_get_frame(void* priv) {
  struct st20p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st20p_next_available(ctx, ctx->framebuff_convert_idx, ST20P_RX_FRAME_READY);
  /* not any ready frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_RX_FRAME_IN_CONVERTING;
  /* point to next */
  ctx->framebuff_convert_idx = rx_st20p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->convert_frame;
}

static int rx_st20p_convert_put_frame(void* priv, struct st20_convert_frame_meta* frame,
                                      int result) {
  struct st20p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff = frame->priv;
  uint16_t convert_idx = framebuff->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_RX_FRAME_IN_CONVERTING != framebuff->stat) {
    err("%s(%d), frame %u not in converting %d\n", __func__, idx, convert_idx,
        framebuff->stat);
    return -EIO;
  }

  dbg("%s(%d), frame %u result %d\n", __func__, idx, convert_idx, result);
  if (result < 0) {
    /* free the frame */
    st20_rx_put_framebuff(ctx->transport, framebuff->src.addr);
    framebuff->stat = ST20P_RX_FRAME_FREE;
    rte_atomic32_inc(&ctx->stat_convert_fail);
  } else {
    framebuff->stat = ST20P_RX_FRAME_CONVERTED;
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
  }

  return 0;
}

static int rx_st20p_convert_dump(void* priv) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t convert_idx = ctx->framebuff_convert_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  notice("RX_st20p(%s), p(%d:%s) cv(%d:%s) c(%d:%s)\n", ctx->ops_name, producer_idx,
         rx_st20p_stat_name(framebuff[producer_idx].stat), convert_idx,
         rx_st20p_stat_name(framebuff[convert_idx].stat), consumer_idx,
         rx_st20p_stat_name(framebuff[consumer_idx].stat));

  int convert_fail = rte_atomic32_read(&ctx->stat_convert_fail);
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  if (convert_fail) {
    notice("RX_st20p(%s), convert fail %d\n", ctx->ops_name, convert_fail);
  }

  int busy = rte_atomic32_read(&ctx->stat_busy);
  rte_atomic32_set(&ctx->stat_busy, 0);
  if (busy) {
    notice("RX_st20p(%s), busy drop frame %d\n", ctx->ops_name, busy);
  }

  return 0;
}

static int rx_st20p_create_transport(st_handle st, struct st20p_rx_ctx* ctx,
                                     struct st20p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st20_rx_ops ops_rx;
  st20_rx_handle transport;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, ST_PORT_MAX);
  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.sip_addr[i], ops->port.sip_addr[i], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[i], ops->port.port[i], ST_PORT_MAX_LEN);
    ops_rx.udp_port[i] = ops->port.udp_port[i];
  }
  if (ops->flags & ST20P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST20P_RX_FLAG_ENABLE_VSYNC) ops_rx.flags |= ST20_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  if (ops->flags & ST20P_RX_FLAG_DMA_OFFLOAD) ops_rx.flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  if (ops->flags & ST20P_RX_FLAG_DISABLE_MIGRATE)
    ops_rx.flags |= ST20_RX_FLAG_DISABLE_MIGRATE;
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.fmt = ops->transport_fmt;
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.notify_frame_ready = rx_st20p_frame_ready;
  ops_rx.notify_event = rx_st20p_notify_event;
  if (ctx->derive) {
    /* ext frame info directly passed down to st20 lib */
    if (ops->ext_frames) ops_rx.ext_frames = ops->ext_frames;
    if (ops->query_ext_frame) {
      if (!(ops->flags & ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
        err("%s, pls enable incomplete frame flag for query ext mode\n", __func__);
        return -EINVAL;
      }
      ops_rx.query_ext_frame = rx_st20p_query_ext_frame;
    }
  }

  transport = st20_rx_create(st, &ops_rx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st20p_rx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].src.fmt = st_frame_fmt_from_transport(ctx->ops.transport_fmt);
    frames[i].src.buffer_size = st_frame_size(frames[i].src.fmt, ops->width, ops->height);
    frames[i].src.data_size = frames[i].src.buffer_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.priv = &frames[i];

    frames[i].convert_frame.src = &frames[i].src;
    frames[i].convert_frame.dst = &frames[i].dst;
    frames[i].convert_frame.priv = &frames[i];
  }

  return 0;
}

static int rx_st20p_uinit_dst_fbs(struct st20p_rx_ctx* ctx) {
  if (ctx->framebuffs) {
    if (!ctx->derive && !ctx->ops.ext_frames) {
      /* do not free derived/ext frames */
      for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
        if (ctx->framebuffs[i].dst.addr) {
          st_rte_free(ctx->framebuffs[i].dst.addr);
          ctx->framebuffs[i].dst.addr = NULL;
        }
      }
    }
    st_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int rx_st20p_init_dst_fbs(struct st_main_impl* impl, struct st20p_rx_ctx* ctx,
                                 struct st20p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = st_socket_id(impl, ST_PORT_P);
  struct st20p_rx_frame* frames;
  void* dst;
  size_t dst_size = ctx->dst_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = st_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST20P_RX_FRAME_FREE;
    frames[i].idx = i;
    if (!ctx->derive) { /* when derive, no need to alloc dst frames */
      if (ops->ext_frames) {
        /* use ext frame as dst frame */
        dst = ops->ext_frames[i].buf_addr;
      } else {
        dst = st_rte_zmalloc_socket(dst_size, soc_id);
      }
      if (!dst) {
        err("%s(%d), dst frame malloc fail at %u\n", __func__, idx, i);
        rx_st20p_uinit_dst_fbs(ctx);
        return -ENOMEM;
      }
      frames[i].dst.addr = dst;
      frames[i].dst.fmt = ops->output_fmt;
      frames[i].dst.buffer_size = dst_size;
      frames[i].dst.data_size = dst_size;
      frames[i].dst.width = ops->width;
      frames[i].dst.height = ops->height;
      frames[i].dst.priv = &frames[i];
    }
  }

  info("%s(%d), size %ld fmt %d with %u frames\n", __func__, idx, dst_size,
       ops->output_fmt, ctx->framebuff_cnt);
  return 0;
}

static int rx_st20p_get_converter_internal(struct st20p_rx_ctx* ctx,
                                           enum st_frame_fmt input_fmt,
                                           enum st_frame_fmt output_fmt) {
  switch (input_fmt) {
    case ST_FRAME_FMT_YUV422RFC4175PG2BE10:
      switch (output_fmt) {
        case ST_FRAME_FMT_YUV422PACKED8:
          ctx->convert_func_internal = convert_rfc4175_422be10_to_422le8;
          break;
        case ST_FRAME_FMT_V210:
          ctx->convert_func_internal = convert_rfc4175_422be10_to_v210;
          break;
        case ST_FRAME_FMT_Y210:
          ctx->convert_func_internal = convert_rfc4175_422be10_to_y210;
          break;
        case ST_FRAME_FMT_YUV422PLANAR10LE:
          ctx->convert_func_internal = convert_rfc4175_422be10_to_yuv422p10le;
          break;
        default:
          err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
              st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
          return -EIO;
      }
      break;
    case ST_FRAME_FMT_YUV422RFC4175PG2BE12:
      switch (output_fmt) {
        case ST_FRAME_FMT_YUV422PLANAR12LE:
          ctx->convert_func_internal = convert_rfc4175_422be12_to_yuv422p12le;
          break;
        default:
          err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
              st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
          return -EIO;
      }
      break;
    case ST_FRAME_FMT_YUV444RFC4175PG4BE10:
      switch (output_fmt) {
        case ST_FRAME_FMT_YUV444PLANAR10LE:
          ctx->convert_func_internal = convert_rfc4175_444be10_to_yuv444p10le;
          break;
        default:
          err("%s(%d), format not supported, input: %s, output: %s\n", __func__, ctx->idx,
              st_frame_fmt_name(input_fmt), st_frame_fmt_name(output_fmt));
          return -EIO;
      }
      break;
    case ST_FRAME_FMT_RGBRFC4175PG4BE10:
      switch (output_fmt) {
        case ST_FRAME_FMT_GBRPLANAR10LE:
          ctx->convert_func_internal = convert_rfc4175_444be10_to_gbrp10le;
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

static int rx_st20p_get_converter(struct st_main_impl* impl, struct st20p_rx_ctx* ctx,
                                  struct st20p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st20_get_converter_request req;

  memset(&req, 0, sizeof(req));
  req.device = ops->device;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.input_fmt = st_frame_fmt_from_transport(ops->transport_fmt);
  req.req.output_fmt = ops->output_fmt;
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.priv = ctx;
  req.get_frame = rx_st20p_convert_get_frame;
  req.put_frame = rx_st20p_convert_put_frame;
  req.dump = rx_st20p_convert_dump;

  if (req.device == ST_PLUGIN_DEVICE_TEST_INTERNAL) {
    info("%s(%d), use internal converter for test\n", __func__, idx);
    return rx_st20p_get_converter_internal(ctx, req.req.input_fmt, req.req.output_fmt);
  }
  struct st20_convert_session_impl* convert_impl = st20_get_converter(impl, &req);
  if (!convert_impl) {
    warn("%s(%d), get converter plugin fail, use internal converter\n", __func__, idx);
    return rx_st20p_get_converter_internal(ctx, req.req.input_fmt, req.req.output_fmt);
  }
  ctx->convert_impl = convert_impl;

  return 0;
}

struct st_frame* st20p_rx_get_frame(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);

  if (ctx->convert_func_internal) { /* convert internal */
    framebuff =
        rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx, ST20P_RX_FRAME_READY);
    /* not any ready frame */
    if (!framebuff) {
      st_pthread_mutex_unlock(&ctx->lock);
      return NULL;
    }
    ctx->convert_func_internal(framebuff->src.addr, framebuff->dst.addr,
                               framebuff->dst.width, framebuff->dst.height);
  } else {
    framebuff = rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx,
                                        ST20P_RX_FRAME_CONVERTED);
    /* not any converted frame */
    if (!framebuff) {
      st_pthread_mutex_unlock(&ctx->lock);
      return NULL;
    }
  }

  framebuff->stat = ST20P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st20p_next_idx(ctx, framebuff->idx);

  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->dst;
}

int st20p_rx_put_frame(st20p_rx_handle handle, struct st_frame* frame) {
  struct st20p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff = frame->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* free the frame */
  st20_rx_put_framebuff(ctx->transport, framebuff->src.addr);
  framebuff->stat = ST20P_RX_FRAME_FREE;
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);

  return 0;
}

st20p_rx_handle st20p_rx_create(st_handle st, struct st20p_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st20p_rx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  size_t dst_size;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!ops->notify_frame_available) {
    err("%s, pls set notify_frame_available\n", __func__);
    return NULL;
  }

  dst_size = st_frame_size(ops->output_fmt, ops->width, ops->height);
  if (!dst_size) {
    err("%s(%d), get dst size fail\n", __func__, idx);
    return NULL;
  }

  ctx = st_rte_zmalloc_socket(sizeof(*ctx), st_socket_id(impl, ST_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->derive = st_frame_fmt_equal_transport(ops->output_fmt, ops->transport_fmt);
  ctx->impl = impl;
  ctx->type = ST20_SESSION_TYPE_PIPELINE_RX;
  ctx->dst_size = dst_size;
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  st_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  ctx->ops = *ops;

  /* get one suitable convert device */
  if (!ctx->derive) {
    ret = rx_st20p_get_converter(impl, ctx, ops);
    if (ret < 0) {
      err("%s(%d), get converter fail %d\n", __func__, idx, ret);
      st20p_rx_free(ctx);
      return NULL;
    }
  }

  /* init fbs */
  ret = rx_st20p_init_dst_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st20p_rx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = rx_st20p_create_transport(st, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st20p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ctx;
}

int st20p_rx_free(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  struct st_main_impl* impl = ctx->impl;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->convert_impl) {
    st20_put_converter(impl, ctx->convert_impl);
    ctx->convert_impl = NULL;
  }

  if (ctx->transport) {
    st20_rx_free(ctx->transport);
    ctx->transport = NULL;
  }
  rx_st20p_uinit_dst_fbs(ctx);

  st_pthread_mutex_destroy(&ctx->lock);
  st_rte_free(ctx);

  return 0;
}

void* st20p_rx_get_fb_addr(st20p_rx_handle handle, uint16_t idx) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }
  if (ctx->derive) return ctx->framebuffs[idx].src.addr;
  return ctx->framebuffs[idx].dst.addr;
}

size_t st20p_rx_frame_size(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->dst_size;
}

int st20p_rx_pcapng_dump(st20p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st20_rx_pcapng_dump(ctx->transport, max_dump_packets, sync, meta);
}

int st20p_rx_get_queue_meta(st20p_rx_handle handle, struct st_queue_meta* meta) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_get_queue_meta(ctx->transport, meta);
}

int st20p_rx_get_sch_idx(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST20_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_get_sch_idx(ctx->transport);
}
