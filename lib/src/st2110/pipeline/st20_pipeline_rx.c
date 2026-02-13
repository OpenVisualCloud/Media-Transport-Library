/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

#include "st20_pipeline_rx.h"

#include "../../mt_log.h"
#include "../../mt_stat.h"

static int rx_st20p_uinit_dst_fbs(struct st20p_rx_ctx* ctx);

static const char* st20p_rx_frame_stat_name[ST20P_RX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_converting", "converted", "in_user",
};

static const char* rx_st20p_stat_name(enum st20p_rx_frame_status stat) {
  return st20p_rx_frame_stat_name[stat];
}

static uint16_t rx_st20p_next_idx(struct st20p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static void rx_st20p_block_wake(struct st20p_rx_ctx* ctx) {
  /* notify block */
  mt_pthread_mutex_lock(&ctx->block_wake_mutex);
  ctx->block_wake_pending = true;
  mt_pthread_cond_signal(&ctx->block_wake_cond);
  mt_pthread_mutex_unlock(&ctx->block_wake_mutex);
}

static void rx_st20p_notify_frame_available(struct st20p_rx_ctx* ctx) {
  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  if (ctx->block_get) {
    /* notify block */
    rx_st20p_block_wake(ctx);
  }
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

static int rx_st20p_packet_convert(void* priv, void* frame,
                                   struct st20_rx_uframe_pg_meta* meta) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff;
  int ret = 0;
  struct st20_rfc4175_422_10_pg2_be* src = meta->payload;
  MTL_MAY_UNUSED(frame);

  mt_pthread_mutex_lock(&ctx->lock);
  if (meta->row_number == 0 && meta->row_offset == 0) {
    /* first packet of frame */
    framebuff =
        rx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_RX_FRAME_FREE);
    if (framebuff) {
      framebuff->stat = ST20P_RX_FRAME_IN_CONVERTING;
      framebuff->dst.timestamp = meta->timestamp;
    }
  } else {
    framebuff = rx_st20p_next_available(ctx, ctx->framebuff_producer_idx,
                                        ST20P_RX_FRAME_IN_CONVERTING);
    if (framebuff && framebuff->dst.timestamp != meta->timestamp) {
      dbg("%s(%d), not this frame, find next one\n", __func__, ctx->idx);
      framebuff =
          rx_st20p_next_available(ctx, framebuff->idx, ST20P_RX_FRAME_IN_CONVERTING);
      if (framebuff && framebuff->dst.timestamp != meta->timestamp) {
        /* should never happen */
        err_once("%s(%d), wrong frame timestamp\n", __func__, ctx->idx);
        mt_pthread_mutex_unlock(&ctx->lock);
        return -EIO;
      }
    }
  }
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }
  mt_pthread_mutex_unlock(&ctx->lock);
  if (ctx->ops.output_fmt == ST_FRAME_FMT_YUV422PLANAR10LE) {
    uint8_t* y = (uint8_t*)framebuff->dst.addr[0] +
                 framebuff->dst.linesize[0] * meta->row_number + meta->row_offset * 2;
    uint8_t* b = (uint8_t*)framebuff->dst.addr[1] +
                 framebuff->dst.linesize[1] * meta->row_number + meta->row_offset;
    uint8_t* r = (uint8_t*)framebuff->dst.addr[2] +
                 framebuff->dst.linesize[2] * meta->row_number + meta->row_offset;
    ret = st20_rfc4175_422be10_to_yuv422p10le(src, (uint16_t*)y, (uint16_t*)b,
                                              (uint16_t*)r, meta->pg_cnt, 2);
  } else if (ctx->ops.output_fmt == ST_FRAME_FMT_Y210) {
    uint8_t* dst = (uint8_t*)framebuff->dst.addr[0] +
                   framebuff->dst.linesize[0] * meta->row_number + meta->row_offset * 4;
    ret = st20_rfc4175_422be10_to_y210(src, (uint16_t*)dst, meta->pg_cnt, 2);
  } else if (ctx->ops.output_fmt == ST_FRAME_FMT_UYVY) {
    uint8_t* dst = (uint8_t*)framebuff->dst.addr[0] +
                   framebuff->dst.linesize[0] * meta->row_number + meta->row_offset * 2;
    ret = st20_rfc4175_422be10_to_422le8(src, (struct st20_rfc4175_422_8_pg2_le*)dst,
                                         meta->pg_cnt, 2);
  } else if (ctx->ops.output_fmt == ST_FRAME_FMT_YUV422PLANAR8) {
    uint8_t* y = (uint8_t*)framebuff->dst.addr[0] +
                 framebuff->dst.linesize[0] * meta->row_number + meta->row_offset * 2;
    uint8_t* b = (uint8_t*)framebuff->dst.addr[1] +
                 framebuff->dst.linesize[1] * meta->row_number + meta->row_offset;
    uint8_t* r = (uint8_t*)framebuff->dst.addr[2] +
                 framebuff->dst.linesize[2] * meta->row_number + meta->row_offset;
    ret = st20_rfc4175_422be10_to_yuv422p8(src, y, b, r, meta->pg_cnt, 2);
  } else if (ctx->ops.output_fmt == ST_FRAME_FMT_YUV420PLANAR8) {
    uint8_t* y = (uint8_t*)framebuff->dst.addr[0] +
                 framebuff->dst.linesize[0] * meta->row_number + meta->row_offset * 2;
    uint8_t* b = (uint8_t*)framebuff->dst.addr[1] +
                 framebuff->dst.linesize[1] * meta->row_number + meta->row_offset;
    uint8_t* r = (uint8_t*)framebuff->dst.addr[2] +
                 framebuff->dst.linesize[2] * meta->row_number + meta->row_offset;
    ret = st20_rfc4175_422be10_to_yuv420p8(src, y, b, r, meta->pg_cnt, 2);
  }

  return ret;
}

static int rx_st20p_frame_ready(void* priv, void* frame,
                                struct st20_rx_frame_meta* meta) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  if (ctx->ops.flags & ST20P_RX_FLAG_PKT_CONVERT) {
    framebuff = rx_st20p_next_available(ctx, ctx->framebuff_producer_idx,
                                        ST20P_RX_FRAME_IN_CONVERTING);
    if (framebuff && framebuff->dst.timestamp != meta->timestamp) {
      dbg("%s(%d), not this frame, find next one\n", __func__, ctx->idx);
      framebuff =
          rx_st20p_next_available(ctx, framebuff->idx, ST20P_RX_FRAME_IN_CONVERTING);
      if (framebuff && framebuff->dst.timestamp != meta->timestamp) {
        /* should never happen */
        mt_pthread_mutex_unlock(&ctx->lock);
        err_once("%s(%d), wrong frame timestamp\n", __func__, ctx->idx);
        return 0; /* surpress the error */
      }
    }
  } else {
    framebuff =
        rx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_RX_FRAME_FREE);
  }

  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  MT_USDT_ST20P_RX_FRAME_AVAILABLE(ctx->idx, framebuff->idx, frame, meta->rtp_timestamp,
                                   meta->frame_recv_size);

  /* query the ext frame for no convert mode */
  if (ctx->dynamic_ext_frame && !ctx->derive) {
    struct st_ext_frame ext_frame;
    memset(&ext_frame, 0x0, sizeof(ext_frame));
    int ret = ctx->ops.query_ext_frame(ctx->ops.priv, &ext_frame, meta);
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
  framebuff->src.second_field = framebuff->dst.second_field = meta->second_field;
  framebuff->src.tfmt = framebuff->dst.tfmt = meta->tfmt;
  framebuff->src.timestamp = framebuff->dst.timestamp = meta->timestamp;
  framebuff->src.rtp_timestamp = framebuff->dst.rtp_timestamp = meta->rtp_timestamp;
  framebuff->src.status = framebuff->dst.status = meta->status;
  framebuff->src.receive_timestamp = framebuff->dst.receive_timestamp =
      meta->timestamp_first_pkt;

  framebuff->src.pkts_total = framebuff->dst.pkts_total = meta->pkts_total;
  for (enum mtl_session_port s_port = 0; s_port < MTL_SESSION_PORT_MAX; s_port++) {
    framebuff->src.pkts_recv[s_port] = framebuff->dst.pkts_recv[s_port] =
        meta->pkts_recv[s_port];
  }

  /* copy timing parser meta */
  for (enum mtl_session_port s_port = 0; s_port < MTL_SESSION_PORT_MAX; s_port++) {
    framebuff->src.tp[s_port] = framebuff->dst.tp[s_port] = NULL;
  }

  for (enum mtl_session_port s_port = 0; s_port < ctx->ops.port.num_port; s_port++) {
    if (!meta->tp[s_port]) continue;
    mtl_memcpy(&framebuff->tp[s_port], meta->tp[s_port], sizeof(framebuff->tp[s_port]));
    framebuff->src.tp[s_port] = framebuff->dst.tp[s_port] = &framebuff->tp[s_port];
  }

  /* check user meta */
  framebuff->user_meta_data_size = 0;
  if (meta->user_meta) {
    if (meta->user_meta_size <= framebuff->user_meta_buffer_size) {
      rte_memcpy(framebuff->user_meta, meta->user_meta, meta->user_meta_size);
      framebuff->user_meta_data_size = meta->user_meta_size;
    } else {
      err("%s(%d), wrong user_meta_size\n", __func__, ctx->idx);
    }
  }

  /* ask app to consume src frame directly */
  if (ctx->derive || (ctx->ops.flags & ST20P_RX_FLAG_PKT_CONVERT)) {
    if (ctx->derive) framebuff->dst = framebuff->src;
    framebuff->stat = ST20P_RX_FRAME_CONVERTED;
    /* point to next */
    ctx->framebuff_producer_idx = rx_st20p_next_idx(ctx, framebuff->idx);
    mt_pthread_mutex_unlock(&ctx->lock);
    rx_st20p_notify_frame_available(ctx);
    return 0;
  }
  framebuff->stat = ST20P_RX_FRAME_READY;

  /* point to next */
  ctx->framebuff_producer_idx = rx_st20p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);

  /* ask convert plugin to consume */
  if (ctx->convert_impl) st20_convert_notify_frame_ready(ctx->convert_impl);

  /* or ask app to consume with internal converter */
  if (ctx->internal_converter) {
    rx_st20p_notify_frame_available(ctx);
  }

  return 0;
}

static int rx_st20p_query_ext_frame(void* priv, struct st20_ext_frame* ext_frame,
                                    struct st20_rx_frame_meta* meta) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff;
  int ret;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st20p_next_available(ctx, ctx->framebuff_producer_idx, ST20P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  struct st_ext_frame ext_st;
  memset(&ext_st, 0, sizeof(ext_st));
  ret = ctx->ops.query_ext_frame(ctx->ops.priv, &ext_st, meta);
  if (ret < 0) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }
  /* only 1 plane for no converter mode */
  ext_frame->buf_addr = ext_st.addr[0];
  ext_frame->buf_iova = ext_st.iova[0];
  ext_frame->buf_len = ext_st.size;
  ext_frame->opaque = ext_st.opaque;
  framebuff->src.opaque = ext_st.opaque;
  mt_pthread_mutex_unlock(&ctx->lock);

  return 0;
}

static int rx_st20p_notify_event(void* priv, enum st_event event, void* args) {
  struct st20p_rx_ctx* ctx = priv;

  if (ctx->ops.notify_event) {
    ctx->ops.notify_event(ctx->ops.priv, event, args);
  }

  return 0;
}

static int rx_st20p_notify_detected(void* priv, const struct st20_detect_meta* meta,
                                    struct st20_detect_reply* reply) {
  struct st20p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  void* dst = NULL;
  struct st20p_rx_frame* frames = ctx->framebuffs;
  bool no_dst_malloc = false;
  int soc_id = ctx->socket_id;

  info("%s(%d), init dst buffer now, w %d h %d\n", __func__, idx, meta->width,
       meta->height);
  ctx->dst_size =
      st_frame_size(ctx->ops.output_fmt, meta->width, meta->height, meta->interlaced);
  if (ctx->derive || ctx->ops.ext_frames || ctx->ops.flags & ST20P_RX_FLAG_EXT_FRAME) {
    no_dst_malloc = true;
  }

  /* init frame width, height now */
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].dst.interlaced = meta->interlaced;
    frames[i].dst.width = meta->width;
    frames[i].dst.height = meta->height;
    frames[i].src.interlaced = meta->interlaced;
    frames[i].src.width = meta->width;
    frames[i].src.height = meta->height;

    frames[i].src.buffer_size =
        st_frame_size(frames[i].src.fmt, frames[i].src.width, frames[i].src.height,
                      frames[i].src.interlaced);
    frames[i].src.data_size = frames[i].src.buffer_size;
    /* rfc4175 uses packed format */
    frames[i].src.linesize[0] =
        RTE_MAX(ctx->ops.transport_linesize,
                st_frame_least_linesize(frames[i].src.fmt, frames[i].src.width, 0));

    if (no_dst_malloc) continue;
    dst = mt_rte_zmalloc_socket(ctx->dst_size, soc_id);
    if (!dst) {
      err("%s(%d), dst frame malloc fail at %u, size %" PRIu64 "\n", __func__, idx, i,
          ctx->dst_size);
      rx_st20p_uinit_dst_fbs(ctx);
      return -ENOMEM;
    }
    frames[i].dst.buffer_size = ctx->dst_size;
    frames[i].dst.data_size = ctx->dst_size;
    /* init plane */
    st_frame_init_plane_single_src(&frames[i].dst, dst, mtl_hp_virt2iova(ctx->impl, dst));
  }

  if (ctx->ops.notify_detected) {
    ctx->ops.notify_detected(ctx->ops.priv, meta, reply);
  }

  return 0;
}

static struct st20_convert_frame_meta* rx_st20p_convert_get_frame(void* priv) {
  struct st20p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st20p_next_available(ctx, ctx->framebuff_convert_idx, ST20P_RX_FRAME_READY);
  /* not any ready frame */
  if (!framebuff) {
    mt_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST20P_RX_FRAME_IN_CONVERTING;
  /* point to next */
  ctx->framebuff_convert_idx = rx_st20p_next_idx(ctx, framebuff->idx);
  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->convert_frame;
}

static int rx_st20p_convert_put_frame(void* priv, struct st20_convert_frame_meta* frame,
                                      int result) {
  struct st20p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff = frame->priv;
  uint16_t convert_idx = framebuff->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
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
    st20_rx_put_framebuff(ctx->transport, framebuff->src.addr[0]);
    framebuff->stat = ST20P_RX_FRAME_FREE;
    rte_atomic32_inc(&ctx->stat_convert_fail);
  } else {
    framebuff->stat = ST20P_RX_FRAME_CONVERTED;
    rx_st20p_notify_frame_available(ctx);
  }

  return 0;
}

static int rx_st20p_convert_dump(void* priv) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t convert_idx = ctx->framebuff_convert_idx;
  notice("RX_st20p(%s), cv(%d:%s)\n", ctx->ops_name, convert_idx,
         rx_st20p_stat_name(framebuff[convert_idx].stat));

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

static int rx_st20p_create_transport(struct mtl_main_impl* impl, struct st20p_rx_ctx* ctx,
                                     struct st20p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st20_rx_ops ops_rx;
  st20_rx_handle transport;
  struct st20_ext_frame* trans_ext_frames = NULL;

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
  if (ops->flags & ST20P_RX_FLAG_DATA_PATH_ONLY)
    ops_rx.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  if (ops->flags & ST20P_RX_FLAG_ENABLE_VSYNC) ops_rx.flags |= ST20_RX_FLAG_ENABLE_VSYNC;
  if (ops->flags & ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)
    ops_rx.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  if (ops->flags & ST20P_RX_FLAG_DMA_OFFLOAD) ops_rx.flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  if (ops->flags & ST20P_RX_FLAG_AUTO_DETECT) ops_rx.flags |= ST20_RX_FLAG_AUTO_DETECT;
  if (ops->flags & ST20P_RX_FLAG_HDR_SPLIT) ops_rx.flags |= ST20_RX_FLAG_HDR_SPLIT;
  if (ops->flags & ST20P_RX_FLAG_DISABLE_MIGRATE)
    ops_rx.flags |= ST20_RX_FLAG_DISABLE_MIGRATE;
  if (ops->flags & ST20P_RX_FLAG_TIMING_PARSER_STAT)
    ops_rx.flags |= ST20_RX_FLAG_TIMING_PARSER_STAT;
  if (ops->flags & ST20P_RX_FLAG_TIMING_PARSER_META)
    ops_rx.flags |= ST20_RX_FLAG_TIMING_PARSER_META;
  if (ops->flags & ST20P_RX_FLAG_USE_MULTI_THREADS)
    ops_rx.flags |= ST20_RX_FLAG_USE_MULTI_THREADS;
  if (ops->flags & ST20P_RX_FLAG_PKT_CONVERT) {
    uint64_t pkt_cvt_output_cap = ST_FMT_CAP_YUV422PLANAR10LE | ST_FMT_CAP_Y210 |
                                  ST_FMT_CAP_UYVY | ST_FMT_CAP_YUV422PLANAR16LE;
    if (ops->transport_fmt != ST20_FMT_YUV_422_10BIT) {
      err("%s(%d), only 422 10bit support packet convert\n", __func__, idx);
      return -EIO;
    }
    if (!(MTL_BIT64(ops->output_fmt) & pkt_cvt_output_cap)) {
      err("%s(%d), %s not supported by packet convert\n", __func__, idx,
          st_frame_fmt_name(ops->output_fmt));
      return -EIO;
    }
    ops_rx.uframe_pg_callback = rx_st20p_packet_convert;
    ops_rx.uframe_size = st20_frame_size(ops->transport_fmt, ops->width, ops->height);
  }
  if (ops->flags & ST20P_RX_FLAG_ENABLE_RTCP) {
    ops_rx.flags |= ST20_RX_FLAG_ENABLE_RTCP;
    ops_rx.rtcp = ops->rtcp;
    if (ops->flags & ST20P_RX_FLAG_SIMULATE_PKT_LOSS)
      ops_rx.flags |= ST20_RX_FLAG_SIMULATE_PKT_LOSS;
  }
  if (ops->flags & ST20P_RX_FLAG_FORCE_NUMA) {
    ops_rx.socket_id = ops->socket_id;
    ops_rx.flags |= ST20_RX_FLAG_FORCE_NUMA;
  }

  if (ops->flags & ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS) {
    ops_rx.gpu_direct_framebuffer_in_vram_device_address = true;
    ops_rx.gpu_context = ops->gpu_context;
  }

  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.fmt = ops->transport_fmt;
  ops_rx.interlaced = ops->interlaced;
  ops_rx.linesize = ops->transport_linesize;
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.ssrc = ops->port.ssrc;
  ops_rx.type = ST20_TYPE_FRAME_LEVEL;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.rx_burst_size = ops->rx_burst_size;
  ops_rx.notify_frame_ready = rx_st20p_frame_ready;
  ops_rx.notify_event = rx_st20p_notify_event;
  ops_rx.notify_detected = rx_st20p_notify_detected;

  if (ctx->derive) {
    /* ext frame info directly passed down to st20 lib */
    if (ops->ext_frames) {
      uint16_t framebuff_cnt = ctx->framebuff_cnt;
      /* hdr split use continuous frame */
      if (ops->flags & ST20P_RX_FLAG_HDR_SPLIT) framebuff_cnt = 1;
      trans_ext_frames = mt_rte_zmalloc_socket(sizeof(*trans_ext_frames) * framebuff_cnt,
                                               ctx->socket_id);
      if (!trans_ext_frames) {
        err("%s, trans_ext_frames malloc fail\n", __func__);
        return -ENOMEM;
      }
      for (uint16_t i = 0; i < framebuff_cnt; i++) {
        trans_ext_frames[i].buf_addr = ops->ext_frames[i].addr[0];
        trans_ext_frames[i].buf_iova = ops->ext_frames[i].iova[0];
        trans_ext_frames[i].buf_len = ops->ext_frames[i].size;
      }
      ops_rx.ext_frames = trans_ext_frames;
    }
    if (ops->query_ext_frame) {
      if (!(ops->flags & ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME)) {
        err("%s, pls enable incomplete frame flag for derive query ext mode\n", __func__);
        if (trans_ext_frames) mt_rte_free(trans_ext_frames);
        return -EINVAL;
      }
      ops_rx.query_ext_frame = rx_st20p_query_ext_frame;
    }
  }

  transport = st20_rx_create(impl, &ops_rx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    if (trans_ext_frames) mt_rte_free(trans_ext_frames);
    return -EIO;
  }
  ctx->transport = transport;

  struct st20p_rx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].src.fmt = st_frame_fmt_from_transport(ctx->ops.transport_fmt);
    frames[i].src.interlaced = ops->interlaced;
    frames[i].src.buffer_size =
        st_frame_size(frames[i].src.fmt, ops->width, ops->height, ops->interlaced);
    frames[i].src.data_size = frames[i].src.buffer_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.linesize[0] = /* rfc4175 uses packed format */
        RTE_MAX(ops->transport_linesize,
                st_frame_least_linesize(frames[i].src.fmt, frames[i].src.width, 0));
    frames[i].src.priv = &frames[i];

    frames[i].convert_frame.src = &frames[i].src;
    frames[i].convert_frame.dst = &frames[i].dst;
    frames[i].convert_frame.priv = &frames[i];
  }

  if (trans_ext_frames) mt_rte_free(trans_ext_frames);

  return 0;
}

static int rx_st20p_uinit_dst_fbs(struct st20p_rx_ctx* ctx) {
  if (ctx->framebuffs) {
    if (!ctx->derive && !ctx->ops.ext_frames &&
        !(ctx->ops.flags & ST20P_RX_FLAG_EXT_FRAME)) {
      /* do not free derived/ext frames */
      for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
        if (ctx->framebuffs[i].dst.addr[0]) {
          mt_rte_free(ctx->framebuffs[i].dst.addr[0]);
          ctx->framebuffs[i].dst.addr[0] = NULL;
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

static int rx_st20p_init_dst_fbs(struct mtl_main_impl* impl, struct st20p_rx_ctx* ctx,
                                 struct st20p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = ctx->socket_id;
  struct st20p_rx_frame* frames;
  void* dst = NULL;
  size_t dst_size = ctx->dst_size;

  bool no_dst_malloc = false;
  if (ops->flags & ST20P_RX_FLAG_EXT_FRAME || ops->flags & ST20P_RX_FLAG_AUTO_DETECT) {
    no_dst_malloc = true;
  }

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = mt_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST20P_RX_FRAME_FREE;
    frames[i].idx = i;
    frames[i].dst.fmt = ops->output_fmt;
    frames[i].dst.interlaced = ops->interlaced;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    if (!ctx->derive) { /* when derive, no need to alloc dst frames */
      uint8_t planes = st_frame_fmt_planes(frames[i].dst.fmt);
      if (ops->ext_frames) {
        /* use dedicated ext frame as dst frame */
        for (uint8_t plane = 0; plane < planes; plane++) {
          frames[i].dst.addr[plane] = ops->ext_frames[i].addr[plane];
          frames[i].dst.iova[plane] = ops->ext_frames[i].iova[plane];
          frames[i].dst.linesize[plane] = ops->ext_frames[i].linesize[plane];
        }
        frames[i].dst.buffer_size = frames[i].dst.data_size = ops->ext_frames[i].size;
        frames[i].dst.opaque = ops->ext_frames[i].opaque;
      } else if (no_dst_malloc) {
        for (uint8_t plane = 0; plane < planes; plane++) {
          frames[i].dst.addr[plane] = NULL;
          frames[i].dst.iova[plane] = 0;
        }
      } else {
        dst = mt_rte_zmalloc_socket(dst_size, soc_id);
        if (!dst) {
          err("%s(%d), dst frame malloc fail at %u, size %" PRIu64 "\n", __func__, idx, i,
              dst_size);
          rx_st20p_uinit_dst_fbs(ctx);
          return -ENOMEM;
        }
        frames[i].dst.buffer_size = dst_size;
        frames[i].dst.data_size = dst_size;
        /* init plane */
        st_frame_init_plane_single_src(&frames[i].dst, dst,
                                       mtl_hp_virt2iova(ctx->impl, dst));
      }

      if (!no_dst_malloc && st_frame_sanity_check(&frames[i].dst) < 0) {
        err("%s(%d), dst frame %d sanity check fail\n", __func__, idx, i);
        rx_st20p_uinit_dst_fbs(ctx);
        return -EINVAL;
      }
      frames[i].dst.priv = &frames[i];
    }
    /* init user meta */
    frames[i].user_meta_buffer_size =
        impl->pkt_udp_suggest_max_size - sizeof(struct st20_rfc4175_rtp_hdr);
    frames[i].user_meta = mt_rte_zmalloc_socket(frames[i].user_meta_buffer_size, soc_id);
    if (!frames[i].user_meta) {
      err("%s(%d), user_meta malloc %" PRIu64 " fail at %d\n", __func__, idx,
          frames[i].user_meta_buffer_size, i);
      rx_st20p_uinit_dst_fbs(ctx);
      return -ENOMEM;
    }
  }
  info("%s(%d), size %" PRIu64 " fmt %d with %u frames\n", __func__, idx, dst_size,
       ops->output_fmt, ctx->framebuff_cnt);
  return 0;
}

static int rx_st20p_get_converter(struct mtl_main_impl* impl, struct st20p_rx_ctx* ctx,
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
  req.req.interlaced = ops->interlaced;
  req.priv = ctx;
  req.get_frame = rx_st20p_convert_get_frame;
  req.put_frame = rx_st20p_convert_put_frame;
  req.dump = rx_st20p_convert_dump;

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

static int rx_st20p_stat(void* priv) {
  struct st20p_rx_ctx* ctx = priv;
  struct st20p_rx_frame* framebuff = ctx->framebuffs;
  uint16_t producer_idx;
  uint16_t consumer_idx;
  enum st20p_rx_frame_status producer_stat;
  enum st20p_rx_frame_status consumer_stat;

  if (!ctx->ready) return -EBUSY; /* not ready */

  mt_pthread_mutex_lock(&ctx->lock);
  producer_idx = ctx->framebuff_producer_idx;
  consumer_idx = ctx->framebuff_consumer_idx;
  producer_stat = framebuff[producer_idx].stat;
  consumer_stat = framebuff[consumer_idx].stat;
  mt_pthread_mutex_unlock(&ctx->lock);

  notice("RX_st20p(%d,%s), p(%d:%s) c(%d:%s)\n", ctx->idx, ctx->ops_name, producer_idx,
         rx_st20p_stat_name(producer_stat), consumer_idx,
         rx_st20p_stat_name(consumer_stat));

  notice("RX_st20p(%d), frame get try %d succ %d, put %d\n", ctx->idx,
         ctx->stat_get_frame_try, ctx->stat_get_frame_succ, ctx->stat_put_frame);
  ctx->stat_get_frame_try = 0;
  ctx->stat_get_frame_succ = 0;
  ctx->stat_put_frame = 0;

  return 0;
}

static int rx_st20p_usdt_dump_frame(struct st20p_rx_ctx* ctx, struct st_frame* frame) {
  int idx = ctx->idx;
  struct mtl_main_impl* impl = ctx->impl;
  int fd;
  char usdt_dump_path[64];
  struct st20p_rx_ops* ops = &ctx->ops;
  uint64_t tsc_s = mt_get_tsc(impl);

  snprintf(usdt_dump_path, sizeof(usdt_dump_path),
           "imtl_usdt_st20prx_s%d_%d_%d_XXXXXX.yuv", idx, ops->width, ops->height);
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
  MT_USDT_ST20P_RX_FRAME_DUMP(idx, usdt_dump_path, frame->addr[0], n);

  info("%s(%d), write %" PRIu64 " to %s(fd:%d), time %fms\n", __func__, idx, n,
       usdt_dump_path, fd, (float)(mt_get_tsc(impl) - tsc_s) / NS_PER_MS);
  close(fd);
  return 0;
}

static int st20p_rx_get_block_wait(struct st20p_rx_ctx* ctx) {
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

struct st_frame* st20p_rx_get_frame(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff;
  struct st_frame* frame;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  ctx->stat_get_frame_try++;

  mt_pthread_mutex_lock(&ctx->lock);

  if (ctx->internal_converter) { /* convert internal */
    framebuff =
        rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx, ST20P_RX_FRAME_READY);
    if (!framebuff && ctx->block_get) { /* wait here */
      mt_pthread_mutex_unlock(&ctx->lock);
      st20p_rx_get_block_wait(ctx);
      /* get again */
      mt_pthread_mutex_lock(&ctx->lock);
      framebuff =
          rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx, ST20P_RX_FRAME_READY);
    }
    /* not any ready frame */
    if (!framebuff) {
      mt_pthread_mutex_unlock(&ctx->lock);
      return NULL;
    }
    ctx->internal_converter->convert_func(&framebuff->src, &framebuff->dst);
  } else {
    framebuff = rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx,
                                        ST20P_RX_FRAME_CONVERTED);
    if (!framebuff && ctx->block_get) { /* wait here */
      mt_pthread_mutex_unlock(&ctx->lock);
      st20p_rx_get_block_wait(ctx);
      /* get again */
      mt_pthread_mutex_lock(&ctx->lock);
      framebuff = rx_st20p_next_available(ctx, ctx->framebuff_consumer_idx,
                                          ST20P_RX_FRAME_CONVERTED);
    }
    /* not any converted frame */
    if (!framebuff) {
      mt_pthread_mutex_unlock(&ctx->lock);
      return NULL;
    }
  }

  framebuff->stat = ST20P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st20p_next_idx(ctx, framebuff->idx);

  mt_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  frame = &framebuff->dst;
  if (framebuff->user_meta_data_size) {
    frame->user_meta = framebuff->user_meta;
    frame->user_meta_size = framebuff->user_meta_data_size;
  } else {
    frame->user_meta = NULL;
    frame->user_meta_size = 0;
  }
  ctx->stat_get_frame_succ++;
  MT_USDT_ST20P_RX_FRAME_GET(idx, framebuff->idx, frame->addr[0]);
  /* check if dump USDT enabled */
  if (MT_USDT_ST20P_RX_FRAME_DUMP_ENABLED()) {
    int period = st_frame_rate(ctx->ops.fps) * 5; /* dump every 5s now */
    if ((ctx->usdt_frame_cnt % period) == (period / 2)) {
      rx_st20p_usdt_dump_frame(ctx, frame);
    }
    ctx->usdt_frame_cnt++;
  } else {
    ctx->usdt_frame_cnt = 0;
  }
  return frame;
}

int st20p_rx_put_frame(st20p_rx_handle handle, struct st_frame* frame) {
  struct st20p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st20p_rx_frame* framebuff = frame->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST20P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in user %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* free the frame */
  st20_rx_put_framebuff(ctx->transport, framebuff->src.addr[0]);
  framebuff->stat = ST20P_RX_FRAME_FREE;
  ctx->stat_put_frame++;

  MT_USDT_ST20P_RX_FRAME_PUT(idx, framebuff->idx, frame->addr[0]);
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);

  return 0;
}

st20p_rx_handle st20p_rx_create(mtl_handle mt, struct st20p_rx_ops* ops) {
  static int st20p_rx_idx;
  struct mtl_main_impl* impl = mt;
  struct st20p_rx_ctx* ctx;
  int ret;
  int idx = st20p_rx_idx;
  size_t dst_size = 0;
  bool auto_detect;

  /* validate the input parameters */
  if (!mt || !ops) {
    err("%s(%d), NULL input parameters \n", __func__, idx);
    return NULL;
  }

  notice("%s, start for %s\n", __func__, mt_string_safe(ops->name));
  auto_detect = ops->flags & ST20P_RX_FLAG_AUTO_DETECT ? true : false;

  if (impl->type != MT_HANDLE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (ops->flags & ST20P_RX_FLAG_EXT_FRAME) {
    if (!ops->query_ext_frame) {
      err("%s, no query_ext_frame query callback for dynamic ext frame mode\n", __func__);
      return NULL;
    }
  }

  if (auto_detect) {
    info("%s(%d), auto_detect enabled\n", __func__, idx);
  } else {
    dst_size = st_frame_size(ops->output_fmt, ops->width, ops->height, ops->interlaced);
    if (!dst_size) {
      err("%s(%d), get dst size fail\n", __func__, idx);
      return NULL;
    }
  }

  enum mtl_port port = mt_port_by_name(impl, ops->port.port[MTL_SESSION_PORT_P]);
  if (port >= MTL_PORT_MAX) return NULL;
  int socket = mt_socket_id(impl, port);

  if (ops->flags & ST20P_RX_FLAG_FORCE_NUMA) {
    socket = ops->socket_id;
    info("%s, ST20P_RX_FLAG_FORCE_NUMA to socket %d\n", __func__, socket);
  }

  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), socket);
  if (!ctx) {
    err("%s, ctx malloc fail on socket %d\n", __func__, socket);
    return NULL;
  }

  ctx->idx = idx;
  ctx->socket_id = socket;
  ctx->ready = false;
  ctx->derive = st_frame_fmt_equal_transport(ops->output_fmt, ops->transport_fmt);
  ctx->dynamic_ext_frame = (ops->flags & ST20P_RX_FLAG_EXT_FRAME) ? true : false;
  ctx->impl = impl;
  ctx->type = MT_ST20_HANDLE_PIPELINE_RX;
  ctx->dst_size = dst_size;
  rte_atomic32_set(&ctx->stat_convert_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  mt_pthread_mutex_init(&ctx->lock, NULL);

  mt_pthread_mutex_init(&ctx->block_wake_mutex, NULL);
  mt_pthread_cond_wait_init(&ctx->block_wake_cond);
  ctx->block_timeout_ns = NS_PER_S;
  ctx->block_wake_pending = false;
  if (ops->flags & ST20P_RX_FLAG_BLOCK_GET) ctx->block_get = true;

  /* copy ops */
  if (ops->name) {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "%s", ops->name);
  } else {
    snprintf(ctx->ops_name, sizeof(ctx->ops_name), "ST20P_RX_%d", idx);
  }
  ctx->ops = *ops;

  /* get one suitable convert device */
  if (!ctx->derive && !(ctx->ops.flags & ST20P_RX_FLAG_PKT_CONVERT)) {
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
  ret = rx_st20p_create_transport(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st20p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;
  notice("%s(%d), transport fmt %s, output fmt %s, flags 0x%x\n", __func__, idx,
         st20_fmt_name(ops->transport_fmt), st_frame_fmt_name(ops->output_fmt),
         ops->flags);
  st20p_rx_idx++;

  if (!ctx->block_get) rx_st20p_notify_frame_available(ctx);

  mt_stat_register(impl, rx_st20p_stat, ctx, ctx->ops_name);

  return ctx;
}

int st20p_rx_free(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  struct mtl_main_impl* impl = ctx->impl;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  notice("%s(%d), start\n", __func__, ctx->idx);

  if (ctx->ready) {
    mt_stat_unregister(impl, rx_st20p_stat, ctx);
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
    st20_rx_free(ctx->transport);
    ctx->transport = NULL;
  }
  rx_st20p_uinit_dst_fbs(ctx);

  mt_pthread_mutex_destroy(&ctx->lock);
  mt_pthread_mutex_destroy(&ctx->block_wake_mutex);
  mt_pthread_cond_destroy(&ctx->block_wake_cond);
  notice("%s(%d), succ\n", __func__, ctx->idx);
  mt_rte_free(ctx);

  return 0;
}

void* st20p_rx_get_fb_addr(st20p_rx_handle handle, uint16_t idx) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
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

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->dst_size;
}

int st20p_rx_pcapng_dump(st20p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st20_rx_pcapng_dump(ctx->transport, max_dump_packets, sync, meta);
}

int st20p_rx_get_queue_meta(st20p_rx_handle handle, struct st_queue_meta* meta) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_get_queue_meta(ctx->transport, meta);
}

int st20p_rx_get_sch_idx(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_get_sch_idx(ctx->transport);
}

int st20p_rx_get_session_stats(st20p_rx_handle handle, struct st20_rx_user_stats* stats) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle || !stats) {
    err("%s, invalid handle %p or stats %p\n", __func__, handle, stats);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_get_session_stats(ctx->transport, stats);
}

int st20p_rx_reset_session_stats(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx;

  if (!handle) {
    err("%s, invalid handle %p\n", __func__, handle);
    return -EINVAL;
  }

  cidx = ctx->idx;
  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_reset_session_stats(ctx->transport);
}

int st20p_rx_update_source(st20p_rx_handle handle, struct st_rx_source_info* src) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_update_source(ctx->transport, src);
}

int st20p_rx_timing_parser_critical(st20p_rx_handle handle,
                                    struct st20_rx_tp_pass* pass) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return st20_rx_timing_parser_critical(ctx->transport, pass);
}

int st20p_rx_wake_block(st20p_rx_handle handle) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  if (ctx->block_get) rx_st20p_block_wake(ctx);

  return 0;
}

int st20p_rx_set_block_timeout(st20p_rx_handle handle, uint64_t timedwait_ns) {
  struct st20p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != MT_ST20_HANDLE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  ctx->block_timeout_ns = timedwait_ns;
  return 0;
}
