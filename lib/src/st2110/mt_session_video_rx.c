/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_video_rx.c
 *
 * Video RX session implementation for the unified session API.
 * Wraps st20_rx_create/free and translates between mtl_video_config_t
 * and st20_rx_ops.
 */

#include "../mt_log.h"
#include "../mt_mem.h"
#include "../mt_session.h"

/*************************************************************************
 * Callback Context
 *************************************************************************/

struct video_rx_ctx {
  struct mtl_session_impl* session;
  st20_rx_handle handle; /* low-level RX handle */
  size_t frame_size;     /* cached frame size */
  /* Ring to queue received frames for buffer_get() */
  struct rte_ring* ready_ring;
  /* User ext_frame callback (if any) */
  int (*user_query_ext_frame)(void* priv, struct st_ext_frame* ext_frame,
                              struct mtl_buffer* frame_meta);
  void* user_priv;
};

/*************************************************************************
 * ST20 RX Callbacks → Unified Event Queue / Ready Ring
 *************************************************************************/

/**
 * notify_frame_ready callback - library delivered a received frame.
 * We push the frame address onto the ready ring for buffer_get() to return.
 */
static int video_rx_notify_frame_ready(void* priv, void* frame,
                                       struct st20_rx_frame_meta* meta) {
  struct video_rx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  /* Enqueue frame pointer to ready ring */
  if (ctx->ready_ring) {
    if (rte_ring_enqueue(ctx->ready_ring, frame) != 0) {
      /* Ring full - return the frame to the library */
      dbg("%s(%s), ready ring full, dropping frame\n", __func__, s->name);
      st20_rx_put_framebuff(ctx->handle, frame);

      rte_spinlock_lock(&s->stats_lock);
      s->stats.buffers_dropped++;
      rte_spinlock_unlock(&s->stats_lock);
      return 0;
    }
  }

  /* Post buffer ready event */
  mtl_event_t event = {0};
  event.type = MTL_EVENT_BUFFER_READY;
  if (meta) {
    event.timestamp = meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
  }
  mtl_session_event_post(s, &event);

  return 0;
}

/**
 * notify_detected callback - video format auto-detected.
 */
static int video_rx_notify_detected(void* priv, const struct st20_detect_meta* meta,
                                    struct st20_detect_reply* reply) {
  struct video_rx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  if (!meta) return -EINVAL;

  mtl_event_t event = {0};
  event.type = MTL_EVENT_FORMAT_DETECTED;
  event.format_detected.width = meta->width;
  event.format_detected.height = meta->height;
  event.format_detected.fps = meta->fps;
  event.format_detected.packing = meta->packing;
  event.format_detected.interlaced = meta->interlaced;
  mtl_session_event_post(s, &event);

  /* Accept the detected format - reply fields (slice_lines, uframe_size) keep defaults */
  (void)reply;

  return 0;
}

/**
 * notify_event callback - general events (vsync, etc.)
 */
static int video_rx_notify_event(void* priv, enum st_event ev, void* args) {
  struct video_rx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  if (ev == ST_EVENT_VSYNC && args) {
    struct st10_vsync_meta* vsync = args;
    mtl_event_t event = {0};
    event.type = MTL_EVENT_VSYNC;
    event.vsync.epoch = vsync->epoch;
    event.vsync.ptp_time = vsync->ptp;
    mtl_session_event_post(s, &event);
  }

  return 0;
}

/**
 * Wrapper for query_ext_frame: translates st20_ext_frame to st_ext_frame.
 */
static int video_rx_query_ext_frame_wrapper(void* priv,
                                            struct st20_ext_frame* st20_ext,
                                            struct st20_rx_frame_meta* meta) {
  struct video_rx_ctx* ctx = priv;

  if (!ctx->user_query_ext_frame) return -ENOTSUP;

  /* Translate st20_ext_frame ↔ st_ext_frame (plane-based) */
  struct st_ext_frame ext = {0};
  mtl_buffer_t buf = {0};

  buf.video.width = meta->width;
  buf.video.height = meta->height;
  buf.size = meta->frame_total_size;

  int ret = ctx->user_query_ext_frame(ctx->user_priv, &ext, &buf);
  if (ret < 0) return ret;

  /* Copy back: take plane[0] as the single buffer for low-level */
  st20_ext->buf_addr = ext.addr[0];
  st20_ext->buf_iova = ext.iova[0];
  st20_ext->buf_len = ext.size;
  st20_ext->opaque = ext.opaque;
  return 0;
}

/*************************************************************************
 * VTable Implementation
 *************************************************************************/

static int video_rx_start(struct mtl_session_impl* s) {
  (void)s;
  return 0;
}

static int video_rx_stop(struct mtl_session_impl* s) {
  (void)s;
  return 0;
}

static void video_rx_destroy(struct mtl_session_impl* s) {
  struct video_rx_ctx* ctx = NULL;

  if (s->inner.video_rx) {
    ctx = s->inner.video_rx->ops.priv;
  }

  /* Drain ready ring and return frames to library before freeing */
  if (ctx && ctx->ready_ring && ctx->handle) {
    void* frame = NULL;
    while (rte_ring_dequeue(ctx->ready_ring, &frame) == 0 && frame) {
      st20_rx_put_framebuff(ctx->handle, frame);
    }
  }

  /* Free the low-level session */
  if (ctx && ctx->handle) {
    st20_rx_free(ctx->handle);
    ctx->handle = NULL;
  }

  s->inner.video_rx = NULL;

  if (ctx) {
    if (ctx->ready_ring) {
      rte_ring_free(ctx->ready_ring);
      ctx->ready_ring = NULL;
    }
    mt_rte_free(ctx);
  }
}

static int video_rx_buffer_get(struct mtl_session_impl* s, mtl_buffer_t** buf,
                               uint32_t timeout_ms) {
  struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
  struct video_rx_ctx* ctx = rx_impl->ops.priv;
  void* frame = NULL;
  uint64_t deadline_ns = 0;

  if (timeout_ms > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
                  (uint64_t)timeout_ms * 1000000ULL;
  }

  do {
    if (mtl_session_check_stopped(s)) {
      return -EAGAIN;
    }

    /* Try to dequeue a received frame */
    if (ctx->ready_ring && rte_ring_dequeue(ctx->ready_ring, &frame) == 0 && frame) {
      /* Find the frame_trans for this frame address */
      struct st_frame_trans* ft = NULL;
      uint16_t frame_idx = 0;
      for (uint16_t i = 0; i < rx_impl->st20_frames_cnt; i++) {
        if (rx_impl->st20_frames[i].addr == frame) {
          ft = &rx_impl->st20_frames[i];
          frame_idx = i;
          break;
        }
      }

      if (!ft) {
        err("%s(%s), frame addr %p not found in frames\n", __func__, s->name, frame);
        return -EIO;
      }

      /* Fill buffer wrapper */
      struct mtl_buffer_impl* b = &s->buffers[frame_idx % s->buffer_count];
      b->frame_trans = ft;
      b->idx = frame_idx;

      mtl_buffer_t* pub = &b->pub;
      memset(pub, 0, sizeof(*pub));
      pub->data = ft->addr;
      pub->iova = ft->iova;
      pub->size = ctx->frame_size;
      pub->priv = b;

      /* Fill from RX metadata */
      struct st20_rx_frame_meta* meta = &ft->rv_meta;
      pub->data_size = meta->frame_recv_size > 0 ? meta->frame_recv_size : ctx->frame_size;
      pub->timestamp = meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
      pub->rtp_timestamp = meta->rtp_timestamp;
      pub->flags = 0;

      if (meta->status == ST_FRAME_STATUS_COMPLETE ||
          meta->status == ST_FRAME_STATUS_RECONSTRUCTED) {
        pub->status = MTL_FRAME_STATUS_COMPLETE;
      } else {
        pub->status = MTL_FRAME_STATUS_INCOMPLETE;
        pub->flags |= MTL_BUF_FLAG_INCOMPLETE;
      }

      /* Video-specific fields */
      pub->video.width = meta->width;
      pub->video.height = meta->height;
      pub->video.fmt = meta->fmt;
      pub->video.pkts_total = meta->pkts_total;
      pub->video.pkts_recv[0] = meta->pkts_recv[0];
      if (MTL_SESSION_PORT_MAX > 1)
        pub->video.pkts_recv[1] = meta->pkts_recv[1];

      /* Update stats */
      rte_spinlock_lock(&s->stats_lock);
      s->stats.buffers_processed++;
      s->stats.bytes_processed += pub->data_size;
      rte_spinlock_unlock(&s->stats_lock);

      *buf = pub;
      return 0;
    }

    /* No frame available */
    if (timeout_ms == 0) {
      return -ETIMEDOUT;
    }

    usleep(100);

    if (deadline_ns > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
      if (now >= deadline_ns) {
        return -ETIMEDOUT;
      }
    }
  } while (1);
}

static int video_rx_buffer_put(struct mtl_session_impl* s, mtl_buffer_t* buf) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  struct mtl_buffer_impl* b = MTL_BUFFER_IMPL(buf);

  if (!b || !b->frame_trans) {
    return -EINVAL;
  }

  /* Return frame to the low-level library */
  int ret = st20_rx_put_framebuff(ctx->handle, b->frame_trans->addr);
  b->frame_trans = NULL;

  return ret;
}

static int video_rx_stats_get(struct mtl_session_impl* s,
                              mtl_session_stats_t* stats) {
  rte_spinlock_lock(&s->stats_lock);
  *stats = s->stats;

  struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
  if (rx_impl) {
    uint32_t free_cnt = 0;
    for (int i = 0; i < rx_impl->st20_frames_cnt; i++) {
      if (rte_atomic32_read(&rx_impl->st20_frames[i].refcnt) == 0) free_cnt++;
    }
    stats->buffers_free = free_cnt;
    stats->buffers_in_use = rx_impl->st20_frames_cnt - free_cnt;
  }

  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

static int video_rx_stats_reset(struct mtl_session_impl* s) {
  rte_spinlock_lock(&s->stats_lock);
  memset(&s->stats, 0, sizeof(s->stats));
  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

static int video_rx_update_source(struct mtl_session_impl* s,
                                  const struct st_rx_source_info* src) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  if (ctx && ctx->handle) {
    return st20_rx_update_source(ctx->handle, (struct st_rx_source_info*)src);
  }
  return -EINVAL;
}

static int video_rx_slice_query(struct mtl_session_impl* s, mtl_buffer_t* buf,
                                uint16_t* lines) {
  (void)s;
  (void)buf;
  (void)lines;
  /* TODO: Implement slice query using internal slot line counters */
  return -ENOTSUP;
}

static int video_rx_event_poll(struct mtl_session_impl* s, mtl_event_t* event,
                               uint32_t timeout_ms) {
  if (mtl_session_check_stopped(s)) {
    return -EAGAIN;
  }

  /* Try to dequeue from event ring */
  if (s->event_ring) {
    void* obj = NULL;
    if (rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
      mtl_event_t* ev = (mtl_event_t*)obj;
      *event = *ev;
      mt_rte_free(ev);
      return 0;
    }
  }

  if (timeout_ms == 0) {
    return -ETIMEDOUT;
  }

  /* Poll with timeout */
  uint64_t deadline_ns = 0;
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
                  (uint64_t)timeout_ms * 1000000ULL;
  }

  while (!mtl_session_check_stopped(s)) {
    void* obj = NULL;
    if (s->event_ring && rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
      mtl_event_t* ev = (mtl_event_t*)obj;
      *event = *ev;
      mt_rte_free(ev);
      return 0;
    }

    usleep(100);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    if (now >= deadline_ns) return -ETIMEDOUT;
  }

  return -EAGAIN;
}

/*************************************************************************
 * Video RX VTable
 *************************************************************************/

const mtl_session_vtable_t mtl_video_rx_vtable = {
    .start = video_rx_start,
    .stop = video_rx_stop,
    .destroy = video_rx_destroy,
    .buffer_get = video_rx_buffer_get,
    .buffer_put = video_rx_buffer_put,
    .buffer_post = NULL, /* TODO: implement for user-owned mode */
    .buffer_flush = NULL,
    .mem_register = NULL, /* TODO: implement DMA registration */
    .mem_unregister = NULL,
    .event_poll = video_rx_event_poll,
    .get_event_fd = NULL,
    .stats_get = video_rx_stats_get,
    .stats_reset = video_rx_stats_reset,
    .update_destination = NULL, /* RX only */
    .update_source = video_rx_update_source,
    .slice_ready = NULL, /* RX doesn't send slices */
    .slice_query = video_rx_slice_query,
    .get_plugin_info = NULL, /* TODO: ST22 plugin support */
    .get_queue_meta = NULL,
};

/*************************************************************************
 * Session Initialization
 *************************************************************************/

int mtl_video_rx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config) {
  struct video_rx_ctx* ctx;
  struct st20_rx_ops ops;
  st20_rx_handle handle;
  char ring_name[RTE_RING_NAMESIZE];

  /* Allocate callback context */
  ctx = mt_rte_zmalloc_socket(sizeof(*ctx), s->socket_id);
  if (!ctx) {
    err("%s, failed to alloc ctx\n", __func__);
    return -ENOMEM;
  }
  ctx->session = s;

  /* Create ready ring for received frames */
  snprintf(ring_name, sizeof(ring_name), "mtl_rx_%p", s);
  ctx->ready_ring =
      rte_ring_create(ring_name, 32, s->socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!ctx->ready_ring) {
    err("%s(%s), failed to create ready ring\n", __func__, s->name);
    mt_rte_free(ctx);
    return -ENOMEM;
  }

  /* Translate mtl_video_config_t → st20_rx_ops */
  memset(&ops, 0, sizeof(ops));

  /* Copy port config */
  memcpy(ops.port, config->rx_port.port, sizeof(ops.port));
  memcpy(ops.ip_addr, config->rx_port.ip_addr, sizeof(ops.ip_addr));
  ops.num_port = config->rx_port.num_port;
  if (ops.num_port == 0) ops.num_port = 1;
  memcpy(ops.udp_port, config->rx_port.udp_port, sizeof(ops.udp_port));
  ops.payload_type = config->rx_port.payload_type;
  ops.ssrc = config->rx_port.ssrc;
  memcpy(ops.mcast_sip_addr, config->rx_port.mcast_sip_addr,
         sizeof(ops.mcast_sip_addr));

  /* Video format */
  ops.width = config->width;
  ops.height = config->height;
  ops.fps = config->fps;
  ops.interlaced = config->interlaced;
  ops.fmt = config->transport_fmt;
  ops.packing = config->packing;
  ops.linesize = config->linesize;

  /* Session config */
  ops.name = config->base.name;
  ops.priv = ctx;
  ops.framebuff_cnt = config->base.num_buffers;
  if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 2;

  /* Set type based on mode */
  if (config->mode == MTL_VIDEO_MODE_SLICE) {
    ops.type = ST20_TYPE_SLICE_LEVEL;
    ops.slice_lines = config->height / 4; /* Default: 4 slices */
  } else {
    ops.type = ST20_TYPE_FRAME_LEVEL;
  }

  /* Callbacks */
  ops.notify_frame_ready = video_rx_notify_frame_ready;

  /* Auto-detect support */
  if (config->enable_auto_detect) {
    ops.flags |= ST20_RX_FLAG_AUTO_DETECT;
    ops.notify_detected = video_rx_notify_detected;
  }

  /* Optional callbacks based on flags */
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_VSYNC) {
    ops.notify_event = video_rx_notify_event;
  }

  /* Map session flags to ST20 flags */
  if (config->base.ownership == MTL_BUFFER_USER_OWNED) {
    /* User-owned: use ext_frame mode via wrapper callback */
    if (config->base.query_ext_frame) {
      ctx->user_query_ext_frame = config->base.query_ext_frame;
      ctx->user_priv = config->base.priv;
      ops.query_ext_frame = video_rx_query_ext_frame_wrapper;
    }
  }
  if (config->base.flags & MTL_SESSION_FLAG_RECEIVE_INCOMPLETE_FRAME) {
    ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  }
  if (config->base.flags & MTL_SESSION_FLAG_DMA_OFFLOAD) {
    ops.flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  }
  if (config->base.flags & MTL_SESSION_FLAG_DATA_PATH_ONLY) {
    ops.flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  }
  if (config->base.flags & MTL_SESSION_FLAG_HDR_SPLIT) {
    ops.flags |= ST20_RX_FLAG_HDR_SPLIT;
  }
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_RTCP) {
    ops.flags |= ST20_RX_FLAG_ENABLE_RTCP;
  }
  if (config->base.flags & MTL_SESSION_FLAG_FORCE_NUMA) {
    ops.socket_id = config->base.socket_id;
  }
  if (config->enable_timing_parser) {
    ops.flags |= ST20_RX_FLAG_TIMING_PARSER_STAT;
  }

  /* Create the low-level RX session */
  handle = st20_rx_create(impl, &ops);
  if (!handle) {
    err("%s(%s), st20_rx_create failed\n", __func__, s->name);
    rte_ring_free(ctx->ready_ring);
    mt_rte_free(ctx);
    return -EIO;
  }

  ctx->handle = handle;
  ctx->frame_size = st20_rx_get_framebuffer_size(handle);

  /* Link the inner session implementation */
  struct st_rx_video_session_handle_impl* handle_impl =
      (struct st_rx_video_session_handle_impl*)handle;
  s->inner.video_rx = handle_impl->impl;
  s->idx = s->inner.video_rx->idx;

  info("%s(%s), created RX video session, frame_size %zu, fb_cnt %u\n", __func__,
       s->name, ctx->frame_size, ops.framebuff_cnt);

  return 0;
}

void mtl_video_rx_session_uinit(struct mtl_session_impl* s) {
  video_rx_destroy(s);
}
