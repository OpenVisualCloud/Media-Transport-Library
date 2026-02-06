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
#include "st_convert.h"
#include "st_fmt.h"

/*************************************************************************
 * Callback Context
 *************************************************************************/

struct video_rx_ctx {
  struct mtl_session_impl* session;
  st20_rx_handle handle; /* low-level RX handle */
  size_t frame_size;     /* transport framebuffer size */
  /* Ring to queue received frames for buffer_get() */
  struct rte_ring* ready_ring;

  /* Format conversion */
  bool derive;                         /* true if no conversion needed */
  enum st_frame_fmt frame_fmt;         /* app pixel format (output) */
  enum st20_fmt transport_fmt;         /* wire format */
  struct st_frame_converter converter; /* cached converter */
  size_t dst_frame_size;               /* app-format buffer size per frame */
  uint32_t width;
  uint32_t height;
  bool interlaced;

  /**
   * Per-framebuffer destination buffers in app pixel format (frame_fmt).
   * Only allocated when !derive (conversion needed).
   * On buffer_get, we convert transport framebuffer → dst_bufs[i].
   * The app reads from dst_bufs[i].
   */
  void** dst_bufs;
  uint16_t dst_bufs_cnt;

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

  /*
   * Save metadata into the frame_trans so buffer_get() can read it later.
   * The 'meta' pointer comes from a per-slot struct that gets reused,
   * so we must copy it now.
   */
  if (meta) {
    struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
    if (rx_impl) {
      for (uint16_t i = 0; i < rx_impl->st20_frames_cnt; i++) {
        if (rx_impl->st20_frames[i].addr == frame) {
          rx_impl->st20_frames[i].rv_meta = *meta;
          break;
        }
      }
    }
  }

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
    /* Free conversion destination buffers */
    if (ctx->dst_bufs) {
      for (uint16_t i = 0; i < ctx->dst_bufs_cnt; i++) {
        if (ctx->dst_bufs[i]) {
          mt_rte_free(ctx->dst_bufs[i]);
          ctx->dst_bufs[i] = NULL;
        }
      }
      mt_rte_free(ctx->dst_bufs);
      ctx->dst_bufs = NULL;
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
      pub->priv = b;

      /* Fill from RX metadata */
      struct st20_rx_frame_meta* meta = &ft->rv_meta;
      pub->rtp_timestamp = meta->rtp_timestamp;
      pub->flags = 0;

      /* Timestamp: pass through raw value and format */
      pub->tfmt = meta->tfmt;
      pub->timestamp = meta->timestamp;

      if (meta->status == ST_FRAME_STATUS_COMPLETE ||
          meta->status == ST_FRAME_STATUS_RECONSTRUCTED) {
        pub->status = MTL_FRAME_STATUS_COMPLETE;
      } else {
        pub->status = MTL_FRAME_STATUS_INCOMPLETE;
        pub->flags |= MTL_BUF_FLAG_INCOMPLETE;
      }

      /* Format conversion: convert transport format → app format */
      if (!ctx->derive && ctx->dst_bufs && frame_idx < ctx->dst_bufs_cnt &&
          ctx->dst_bufs[frame_idx]) {
        /* Build source st_frame (transport/wire format) */
        struct st_frame src_frame;
        memset(&src_frame, 0, sizeof(src_frame));
        src_frame.fmt = st_frame_fmt_from_transport(ctx->transport_fmt);
        src_frame.width = ctx->width;
        src_frame.height = ctx->height;
        src_frame.interlaced = ctx->interlaced;
        src_frame.buffer_size = ctx->frame_size;
        src_frame.data_size = ctx->frame_size;
        st_frame_init_plane_single_src(&src_frame, ft->addr, ft->iova);

        /* Build destination st_frame (app pixel format) */
        struct st_frame dst_frame;
        memset(&dst_frame, 0, sizeof(dst_frame));
        dst_frame.fmt = ctx->frame_fmt;
        dst_frame.width = ctx->width;
        dst_frame.height = ctx->height;
        dst_frame.interlaced = ctx->interlaced;
        dst_frame.buffer_size = ctx->dst_frame_size;
        dst_frame.data_size = ctx->dst_frame_size;
        st_frame_init_plane_single_src(&dst_frame, ctx->dst_bufs[frame_idx], 0);

        /* Do the conversion */
        int ret = ctx->converter.convert_func(&src_frame, &dst_frame);
        if (ret < 0) {
          err("%s, conversion failed %d, src %s -> dst %s\n", __func__, ret,
              st_frame_fmt_name(src_frame.fmt), st_frame_fmt_name(dst_frame.fmt));
          /* Return the transport frame on failure */
          st20_rx_put_framebuff(ctx->handle, ft->addr);
          b->frame_trans = NULL;
          return ret;
        }

        /* Give app the converted buffer */
        pub->data = ctx->dst_bufs[frame_idx];
        pub->iova = 0;
        pub->size = ctx->dst_frame_size;
        pub->data_size = ctx->dst_frame_size;
        pub->video.fmt = ctx->frame_fmt;
      } else {
        /* Derive mode: give app the transport framebuffer directly */
        pub->data = ft->addr;
        pub->iova = ft->iova;
        pub->size = ctx->frame_size;
        pub->data_size = meta->frame_recv_size > 0 ? meta->frame_recv_size : ctx->frame_size;
        pub->video.fmt = st_frame_fmt_from_transport(ctx->transport_fmt);
      }

      /* Video-specific fields */
      pub->video.width = meta->width;
      pub->video.height = meta->height;
      pub->video.pkts_total = meta->pkts_total;
      pub->video.pkts_recv[0] = meta->pkts_recv[0];
      if (MTL_SESSION_PORT_MAX > 1)
        pub->video.pkts_recv[1] = meta->pkts_recv[1];
      pub->video.interlaced = ctx->interlaced;
      pub->video.second_field = meta->second_field;

      /* User metadata pass-through */
      if (ft->user_meta && ft->user_meta_data_size > 0) {
        pub->user_meta = ft->user_meta;
        pub->user_meta_size = ft->user_meta_data_size;
      }

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

static size_t video_rx_get_frame_size(struct mtl_session_impl* s) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  if (!ctx) return 0;
  /* Return the app-visible frame size (converted format if conversion, else transport) */
  return ctx->derive ? ctx->frame_size : ctx->dst_frame_size;
}

static int video_rx_io_stats_get(struct mtl_session_impl* s, void* stats,
                                 size_t stats_size) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  if (!ctx || !ctx->handle) return -EINVAL;
  if (stats_size < sizeof(struct st20_rx_user_stats)) return -EINVAL;
  return st20_rx_get_session_stats(ctx->handle, (struct st20_rx_user_stats*)stats);
}

static int video_rx_io_stats_reset(struct mtl_session_impl* s) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  if (!ctx || !ctx->handle) return -EINVAL;
  return st20_rx_reset_session_stats(ctx->handle);
}

static int video_rx_pcap_dump(struct mtl_session_impl* s, uint32_t max_pkts,
                              bool sync, struct st_pcap_dump_meta* meta) {
  struct video_rx_ctx* ctx = s->inner.video_rx->ops.priv;
  if (!ctx || !ctx->handle) return -EINVAL;
  return st20_rx_pcapng_dump(ctx->handle, max_pkts, sync, meta);
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
    .get_frame_size = video_rx_get_frame_size,
    .io_stats_get = video_rx_io_stats_get,
    .io_stats_reset = video_rx_io_stats_reset,
    .pcap_dump = video_rx_pcap_dump,
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
  ctx->width = config->width;
  ctx->height = config->height;
  ctx->interlaced = config->interlaced;
  ctx->frame_fmt = config->frame_fmt;
  ctx->transport_fmt = config->transport_fmt;

  /* Determine if format conversion is needed */
  ctx->derive = st_frame_fmt_equal_transport(config->frame_fmt, config->transport_fmt);
  s->video.frame_fmt = config->frame_fmt;
  s->video.derive = ctx->derive;

  /* If conversion needed, look up the converter */
  if (!ctx->derive) {
    enum st_frame_fmt transport_frame_fmt = st_frame_fmt_from_transport(config->transport_fmt);
    if (transport_frame_fmt == ST_FRAME_FMT_MAX) {
      err("%s(%s), unsupported transport_fmt %d\n", __func__, config->base.name,
          config->transport_fmt);
      mt_rte_free(ctx);
      return -EINVAL;
    }
    /* RX converts: transport format → app format */
    int ret = st_frame_get_converter(transport_frame_fmt, config->frame_fmt, &ctx->converter);
    if (ret < 0) {
      err("%s(%s), no converter from %s to %s\n", __func__, config->base.name,
          st_frame_fmt_name(transport_frame_fmt), st_frame_fmt_name(config->frame_fmt));
      mt_rte_free(ctx);
      return ret;
    }
    ctx->dst_frame_size =
        st_frame_size(config->frame_fmt, config->width, config->height, config->interlaced);
    if (!ctx->dst_frame_size) {
      err("%s(%s), failed to get dst frame size for fmt %s\n", __func__, config->base.name,
          st_frame_fmt_name(config->frame_fmt));
      mt_rte_free(ctx);
      return -EINVAL;
    }
    info("%s(%s), conversion enabled: %s -> %s, dst_size %zu\n", __func__,
         config->base.name, st_frame_fmt_name(transport_frame_fmt),
         st_frame_fmt_name(config->frame_fmt), ctx->dst_frame_size);
  }

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
  if (config->base.flags & MTL_SESSION_FLAG_USE_MULTI_THREADS) {
    ops.flags |= ST20_RX_FLAG_USE_MULTI_THREADS;
  }
  if (config->enable_timing_parser) {
    ops.flags |= ST20_RX_FLAG_TIMING_PARSER_STAT;
  }

  /* Advanced RX options */
  if (config->rx_burst_size) ops.rx_burst_size = config->rx_burst_size;

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

  /* Allocate conversion destination buffers if needed */
  if (!ctx->derive) {
    uint16_t fb_cnt = s->inner.video_rx->st20_frames_cnt;
    ctx->dst_bufs = mt_rte_zmalloc_socket(sizeof(void*) * fb_cnt, s->socket_id);
    if (!ctx->dst_bufs) {
      err("%s(%s), failed to alloc dst_bufs array\n", __func__, s->name);
      st20_rx_free(handle);
      ctx->handle = NULL;
      s->inner.video_rx = NULL;
      rte_ring_free(ctx->ready_ring);
      mt_rte_free(ctx);
      return -ENOMEM;
    }
    ctx->dst_bufs_cnt = fb_cnt;
    for (uint16_t i = 0; i < fb_cnt; i++) {
      ctx->dst_bufs[i] = mt_rte_zmalloc_socket(ctx->dst_frame_size, s->socket_id);
      if (!ctx->dst_bufs[i]) {
        err("%s(%s), failed to alloc dst_buf[%u], size %zu\n", __func__, s->name,
            i, ctx->dst_frame_size);
        for (uint16_t j = 0; j < i; j++) {
          mt_rte_free(ctx->dst_bufs[j]);
        }
        mt_rte_free(ctx->dst_bufs);
        ctx->dst_bufs = NULL;
        st20_rx_free(handle);
        ctx->handle = NULL;
        s->inner.video_rx = NULL;
        rte_ring_free(ctx->ready_ring);
        mt_rte_free(ctx);
        return -ENOMEM;
      }
    }
    info("%s(%s), allocated %u conversion dst buffers, %zu bytes each\n", __func__,
         s->name, fb_cnt, ctx->dst_frame_size);
  }

  info("%s(%s), created RX video session, frame_size %zu, fb_cnt %u, derive %d\n",
       __func__, s->name, ctx->frame_size, ops.framebuff_cnt, ctx->derive);

  return 0;
}

void mtl_video_rx_session_uinit(struct mtl_session_impl* s) {
  video_rx_destroy(s);
}
