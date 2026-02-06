/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_video_tx.c
 *
 * Video TX session implementation for the unified session API.
 * Wraps st20_tx_create/free and translates between mtl_video_config_t
 * and st20_tx_ops.
 */

#include "../mt_log.h"
#include "../mt_mem.h"
#include "../mt_session.h"
#include "st_convert.h"
#include "st_fmt.h"

/*************************************************************************
 * Callback Context - bridges st20 callbacks to unified event queue
 *************************************************************************/

/**
 * Frame state tracked by the unified API wrapper.
 * Separate from the low-level library's refcnt to avoid conflicts.
 * The library manages refcnt for its own transmit lifecycle;
 * we use this enum to track the app-facing buffer_get/put lifecycle.
 */
enum tx_frame_state {
  TX_FRAME_FREE = 0,        /**< Available for buffer_get */
  TX_FRAME_APP_OWNED = 1,   /**< App is filling it (between get and put) */
  TX_FRAME_READY = 2,       /**< App called put, waiting for get_next_frame callback */
  TX_FRAME_TRANSMITTING = 3 /**< Library picked it via get_next_frame */
};

struct video_tx_ctx {
  struct mtl_session_impl* session;
  st20_tx_handle handle; /* low-level TX handle */
  size_t frame_size;     /* transport framebuffer size */

  /* Per-frame state tracking (does NOT touch library refcnt) */
  enum tx_frame_state* frame_state;
  uint16_t frame_cnt;

  /* Format conversion */
  bool derive;                       /* true if no conversion needed */
  enum st_frame_fmt frame_fmt;       /* app pixel format */
  enum st20_fmt transport_fmt;       /* wire format */
  struct st_frame_converter converter; /* cached converter */
  size_t src_frame_size;             /* app-format buffer size per frame */
  uint32_t width;
  uint32_t height;
  bool interlaced;

  /**
   * Per-framebuffer source buffers in app pixel format (frame_fmt).
   * Only allocated when !derive (conversion needed).
   * src_bufs[i] is the buffer the app writes into for framebuffer i.
   * On buffer_put, we convert src_bufs[i] → transport framebuffer[i].
   */
  void** src_bufs;
  uint16_t src_bufs_cnt;

  /* User slice callback (if any) */
  int (*user_query_lines_ready)(void* priv, uint16_t frame_idx, uint16_t* lines_ready);
  void* user_priv;
};

/*************************************************************************
 * ST20 TX Callbacks → Unified Event Queue
 *************************************************************************/

/**
 * get_next_frame callback - library asks which frame to transmit next.
 * We find a free frame with data ready (refcnt indicates state).
 */
static int video_tx_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  (void)meta;

  if (!tx_impl || !tx_impl->st20_frames) return -EIO;

  /* Find a frame that the app has submitted (our state == READY) */
  for (uint16_t i = 0; i < tx_impl->st20_frames_cnt; i++) {
    if (ctx->frame_state[i] == TX_FRAME_READY) {
      *next_frame_idx = i;
      ctx->frame_state[i] = TX_FRAME_TRANSMITTING;
      /* Library expects refcnt == 0 here; it will increment to 1 */
      rte_atomic32_set(&tx_impl->st20_frames[i].refcnt, 0);
      return 0;
    }
  }

  return -EBUSY; /* No frame ready */
}

/**
 * notify_frame_done callback - transmission of a frame is complete.
 * Release the frame (set refcnt back to 0 = free).
 */
static int video_tx_notify_frame_done(void* priv, uint16_t frame_idx,
                                      struct st20_tx_frame_meta* meta) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;

  if (frame_idx < tx_impl->st20_frames_cnt) {
    /* Mark frame as free for buffer_get (library manages its own refcnt) */
    ctx->frame_state[frame_idx] = TX_FRAME_FREE;

    /* Update stats */
    rte_spinlock_lock(&s->stats_lock);
    s->stats.buffers_processed++;
    s->stats.bytes_processed += ctx->frame_size;
    rte_spinlock_unlock(&s->stats_lock);

    /* Post event */
    mtl_event_t event = {0};
    event.type = MTL_EVENT_BUFFER_DONE;
    event.timestamp = meta ? meta->epoch : 0;
    mtl_session_event_post(s, &event);
  }

  return 0;
}

/**
 * notify_frame_late callback - frame missed its epoch.
 */
static int video_tx_notify_frame_late(void* priv, uint64_t epoch_skipped) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  rte_spinlock_lock(&s->stats_lock);
  s->stats.epochs_missed++;
  rte_spinlock_unlock(&s->stats_lock);

  mtl_event_t event = {0};
  event.type = MTL_EVENT_FRAME_LATE;
  event.frame_late.epoch_skipped = epoch_skipped;
  mtl_session_event_post(s, &event);

  return 0;
}

/**
 * notify_event callback - general events (vsync, etc.)
 */
static int video_tx_notify_event(void* priv, enum st_event ev, void* args) {
  struct video_tx_ctx* ctx = priv;
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
 * Wrapper for query_frame_lines_ready: translates st20_tx_slice_meta to uint16_t*.
 */
static int video_tx_query_lines_ready_wrapper(void* priv, uint16_t frame_idx,
                                              struct st20_tx_slice_meta* meta) {
  struct video_tx_ctx* ctx = priv;
  if (!ctx->user_query_lines_ready) return -ENOTSUP;

  uint16_t lines_ready = 0;
  int ret = ctx->user_query_lines_ready(ctx->user_priv, frame_idx, &lines_ready);
  if (ret == 0) {
    meta->lines_ready = lines_ready;
  }
  return ret;
}

/*************************************************************************
 * VTable Implementation
 *************************************************************************/

static int video_tx_start(struct mtl_session_impl* s) {
  /* The low-level session starts when mtl_start() is called on the parent.
   * For the unified API, we need the parent MTL instance to be started.
   * The session itself is already active after create. */
  (void)s;
  return 0;
}

static int video_tx_stop(struct mtl_session_impl* s) {
  /* Stop is handled by the core layer setting the stopped flag.
   * The low-level session is stopped when mtl_stop() is called. */
  (void)s;
  return 0;
}

static void video_tx_destroy(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = NULL;

  /* Get context from inner session priv */
  if (s->inner.video_tx) {
    ctx = s->inner.video_tx->ops.priv;
  }

  /* Free the low-level session */
  if (ctx && ctx->handle) {
    st20_tx_free(ctx->handle);
    ctx->handle = NULL;
  }

  s->inner.video_tx = NULL;

  if (ctx) {
    /* Free frame state array */
    if (ctx->frame_state) {
      mt_rte_free(ctx->frame_state);
      ctx->frame_state = NULL;
    }
    /* Free conversion source buffers */
    if (ctx->src_bufs) {
      for (uint16_t i = 0; i < ctx->src_bufs_cnt; i++) {
        if (ctx->src_bufs[i]) {
          mt_rte_free(ctx->src_bufs[i]);
          ctx->src_bufs[i] = NULL;
        }
      }
      mt_rte_free(ctx->src_bufs);
      ctx->src_bufs = NULL;
    }
    mt_rte_free(ctx);
  }
}

static int video_tx_buffer_get(struct mtl_session_impl* s, mtl_buffer_t** buf,
                               uint32_t timeout_ms) {
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  struct video_tx_ctx* ctx = tx_impl->ops.priv;
  uint64_t deadline_ns = 0;

  if (timeout_ms > 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
                  (uint64_t)timeout_ms * 1000000ULL;
  }

  do {
    /* Check stopped flag */
    if (mtl_session_check_stopped(s)) {
      return -EAGAIN;
    }

    /* Find a free frame (our state == FREE and library refcnt == 0) */
    for (uint16_t i = 0; i < tx_impl->st20_frames_cnt; i++) {
      if (ctx->frame_state[i] == TX_FRAME_FREE &&
          rte_atomic32_read(&tx_impl->st20_frames[i].refcnt) == 0) {
        /* Claim this frame for the app */
        ctx->frame_state[i] = TX_FRAME_APP_OWNED;

        /* Fill buffer wrapper */
        struct mtl_buffer_impl* b = &s->buffers[i % s->buffer_count];
        b->frame_trans = &tx_impl->st20_frames[i];
        b->idx = i;

        mtl_buffer_t* pub = &b->pub;

        if (!ctx->derive && ctx->src_bufs && ctx->src_bufs[i]) {
          /* Conversion mode: give app the source buffer (app pixel format) */
          pub->data = ctx->src_bufs[i];
          pub->iova = 0; /* src buffer doesn't need IOVA for app use */
          pub->size = ctx->src_frame_size;
          pub->data_size = ctx->src_frame_size;
          pub->video.fmt = ctx->frame_fmt;
        } else {
          /* Derive mode or fallback: give app the transport framebuffer directly */
          pub->data = tx_impl->st20_frames[i].addr;
          pub->iova = tx_impl->st20_frames[i].iova;
          pub->size = ctx->frame_size;
          pub->data_size = ctx->frame_size;
          pub->video.fmt = st_frame_fmt_from_transport(ctx->transport_fmt);
        }
        pub->priv = b;
        pub->flags = 0;
        pub->status = MTL_FRAME_STATUS_COMPLETE;

        /* Video-specific fields */
        pub->video.width = ctx->width;
        pub->video.height = ctx->height;

        *buf = pub;
        return 0;
      }
    }

    /* No free frame - busy wait or check timeout */
    if (timeout_ms == 0) {
      return -ETIMEDOUT;
    }

    /* Brief sleep to avoid busy-spin */
    usleep(100);

    /* Check timeout */
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

static int video_tx_buffer_put(struct mtl_session_impl* s, mtl_buffer_t* buf) {
  struct video_tx_ctx* ctx = s->inner.video_tx->ops.priv;
  struct mtl_buffer_impl* b = MTL_BUFFER_IMPL(buf);

  if (!b || !b->frame_trans) {
    return -EINVAL;
  }

  /* If conversion needed, convert app-format buffer → transport framebuffer */
  if (!ctx->derive && ctx->src_bufs && b->idx < ctx->src_bufs_cnt &&
      ctx->src_bufs[b->idx]) {
    /* Build source st_frame (app pixel format) */
    struct st_frame src_frame;
    memset(&src_frame, 0, sizeof(src_frame));
    src_frame.fmt = ctx->frame_fmt;
    src_frame.width = ctx->width;
    src_frame.height = ctx->height;
    src_frame.interlaced = ctx->interlaced;
    src_frame.buffer_size = ctx->src_frame_size;
    src_frame.data_size = ctx->src_frame_size;
    st_frame_init_plane_single_src(&src_frame, ctx->src_bufs[b->idx], 0);

    /* Build destination st_frame (transport/wire format) */
    struct st_frame dst_frame;
    memset(&dst_frame, 0, sizeof(dst_frame));
    dst_frame.fmt = st_frame_fmt_from_transport(ctx->transport_fmt);
    dst_frame.width = ctx->width;
    dst_frame.height = ctx->height;
    dst_frame.interlaced = ctx->interlaced;
    dst_frame.buffer_size = ctx->frame_size;
    dst_frame.data_size = ctx->frame_size;
    st_frame_init_plane_single_src(&dst_frame, b->frame_trans->addr, b->frame_trans->iova);

    /* Do the conversion */
    int ret = ctx->converter.convert_func(&src_frame, &dst_frame);
    if (ret < 0) {
      err("%s, conversion failed %d, src %s -> dst %s\n", __func__, ret,
          st_frame_fmt_name(src_frame.fmt), st_frame_fmt_name(dst_frame.fmt));
      /* Release the frame on conversion failure */
      ctx->frame_state[b->idx] = TX_FRAME_FREE;
      return ret;
    }
  }

  /* Pass user metadata to the low-level frame if provided */
  if (buf->user_meta && buf->user_meta_size > 0) {
    b->frame_trans->tv_meta.user_meta = buf->user_meta;
    b->frame_trans->tv_meta.user_meta_size = buf->user_meta_size;
  } else {
    b->frame_trans->tv_meta.user_meta = NULL;
    b->frame_trans->tv_meta.user_meta_size = 0;
  }

  /* Pass timestamp and format */
  if (buf->timestamp) {
    b->frame_trans->tv_meta.timestamp = buf->timestamp;
    b->frame_trans->tv_meta.tfmt = buf->tfmt;
  }

  /* Mark frame as ready for get_next_frame callback to pick up */
  ctx->frame_state[b->idx] = TX_FRAME_READY;

  return 0;
}

static int video_tx_stats_get(struct mtl_session_impl* s,
                              mtl_session_stats_t* stats) {
  rte_spinlock_lock(&s->stats_lock);
  *stats = s->stats;

  /* Count free/in-use buffers */
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  struct video_tx_ctx* ctx_stat = tx_impl ? tx_impl->ops.priv : NULL;
  if (tx_impl && ctx_stat && ctx_stat->frame_state) {
    uint32_t free_cnt = 0;
    for (uint16_t i = 0; i < ctx_stat->frame_cnt; i++) {
      if (ctx_stat->frame_state[i] == TX_FRAME_FREE) free_cnt++;
    }
    stats->buffers_free = free_cnt;
    stats->buffers_in_use = ctx_stat->frame_cnt - free_cnt;
  }

  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

static int video_tx_stats_reset(struct mtl_session_impl* s) {
  rte_spinlock_lock(&s->stats_lock);
  memset(&s->stats, 0, sizeof(s->stats));
  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

static int video_tx_update_destination(struct mtl_session_impl* s,
                                       const struct st_tx_dest_info* dst) {
  struct video_tx_ctx* ctx = s->inner.video_tx->ops.priv;
  if (ctx && ctx->handle) {
    return st20_tx_update_destination(ctx->handle, (struct st_tx_dest_info*)dst);
  }
  return -EINVAL;
}

static size_t video_tx_get_frame_size(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = s->inner.video_tx->ops.priv;
  if (!ctx) return 0;
  /* Return the app-visible frame size (source format if conversion, else transport) */
  return ctx->derive ? ctx->frame_size : ctx->src_frame_size;
}

static int video_tx_io_stats_get(struct mtl_session_impl* s, void* stats,
                                 size_t stats_size) {
  struct video_tx_ctx* ctx = s->inner.video_tx->ops.priv;
  if (!ctx || !ctx->handle) return -EINVAL;
  if (stats_size < sizeof(struct st20_tx_user_stats)) return -EINVAL;
  return st20_tx_get_session_stats(ctx->handle, (struct st20_tx_user_stats*)stats);
}

static int video_tx_io_stats_reset(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = s->inner.video_tx->ops.priv;
  if (!ctx || !ctx->handle) return -EINVAL;
  return st20_tx_reset_session_stats(ctx->handle);
}

static int video_tx_slice_ready(struct mtl_session_impl* s, mtl_buffer_t* buf,
                                uint16_t lines) {
  (void)s;
  (void)buf;
  (void)lines;
  /* Slice mode integration would need to call internal pacing functions.
   * For now, the query_lines_ready callback handles this. */
  return 0;
}

static int video_tx_event_poll(struct mtl_session_impl* s, mtl_event_t* event,
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
    if (rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
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
 * Video TX VTable
 *************************************************************************/

const mtl_session_vtable_t mtl_video_tx_vtable = {
    .start = video_tx_start,
    .stop = video_tx_stop,
    .destroy = video_tx_destroy,
    .buffer_get = video_tx_buffer_get,
    .buffer_put = video_tx_buffer_put,
    .buffer_post = NULL, /* TODO: implement for user-owned mode */
    .buffer_flush = NULL,
    .mem_register = NULL, /* TODO: implement DMA registration */
    .mem_unregister = NULL,
    .event_poll = video_tx_event_poll,
    .get_event_fd = NULL, /* Uses default from session impl */
    .stats_get = video_tx_stats_get,
    .stats_reset = video_tx_stats_reset,
    .get_frame_size = video_tx_get_frame_size,
    .io_stats_get = video_tx_io_stats_get,
    .io_stats_reset = video_tx_io_stats_reset,
    .pcap_dump = NULL, /* TX has no pcap dump */
    .update_destination = video_tx_update_destination,
    .update_source = NULL, /* TX only */
    .slice_ready = video_tx_slice_ready,
    .slice_query = NULL, /* TX only sends, no query */
    .get_plugin_info = NULL, /* TODO: ST22 plugin support */
    .get_queue_meta = NULL,
};

/*************************************************************************
 * Session Initialization
 *************************************************************************/

int mtl_video_tx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config) {
  struct video_tx_ctx* ctx;
  struct st20_tx_ops ops;
  st20_tx_handle handle;

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
    int ret = st_frame_get_converter(config->frame_fmt, transport_frame_fmt, &ctx->converter);
    if (ret < 0) {
      err("%s(%s), no converter from %s to %s\n", __func__, config->base.name,
          st_frame_fmt_name(config->frame_fmt), st_frame_fmt_name(transport_frame_fmt));
      mt_rte_free(ctx);
      return ret;
    }
    ctx->src_frame_size =
        st_frame_size(config->frame_fmt, config->width, config->height, config->interlaced);
    if (!ctx->src_frame_size) {
      err("%s(%s), failed to get src frame size for fmt %s\n", __func__, config->base.name,
          st_frame_fmt_name(config->frame_fmt));
      mt_rte_free(ctx);
      return -EINVAL;
    }
    info("%s(%s), conversion enabled: %s -> %s, src_size %zu\n", __func__,
         config->base.name, st_frame_fmt_name(config->frame_fmt),
         st_frame_fmt_name(transport_frame_fmt), ctx->src_frame_size);
  }

  /* Translate mtl_video_config_t → st20_tx_ops */
  memset(&ops, 0, sizeof(ops));

  /* Copy port config */
  memcpy(ops.port, config->tx_port.port, sizeof(ops.port));
  memcpy(ops.dip_addr, config->tx_port.dip_addr, sizeof(ops.dip_addr));
  ops.num_port = config->tx_port.num_port;
  if (ops.num_port == 0) ops.num_port = 1;
  memcpy(ops.udp_port, config->tx_port.udp_port, sizeof(ops.udp_port));
  ops.payload_type = config->tx_port.payload_type;
  ops.ssrc = config->tx_port.ssrc;
  memcpy(ops.udp_src_port, config->tx_port.udp_src_port, sizeof(ops.udp_src_port));

  /* Video format */
  ops.width = config->width;
  ops.height = config->height;
  ops.fps = config->fps;
  ops.interlaced = config->interlaced;
  ops.fmt = config->transport_fmt;
  ops.packing = config->packing;
  ops.pacing = config->pacing;
  ops.linesize = config->linesize;

  /* Session config */
  ops.name = config->base.name;
  ops.priv = ctx;
  ops.framebuff_cnt = config->base.num_buffers;
  if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 2;

  /* Set type based on mode */
  if (config->mode == MTL_VIDEO_MODE_SLICE) {
    ops.type = ST20_TYPE_SLICE_LEVEL;
    if (config->query_lines_ready) {
      ctx->user_query_lines_ready = config->query_lines_ready;
      ctx->user_priv = config->base.priv;
      ops.query_frame_lines_ready = video_tx_query_lines_ready_wrapper;
    }
  } else {
    ops.type = ST20_TYPE_FRAME_LEVEL;
  }

  /* Callbacks */
  ops.get_next_frame = video_tx_get_next_frame;
  ops.notify_frame_done = video_tx_notify_frame_done;

  /* Optional callbacks based on flags */
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_VSYNC) {
    ops.notify_event = video_tx_notify_event;
    ops.flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  }

  /* Map session flags to ST20 flags */
  if (config->base.ownership == MTL_BUFFER_USER_OWNED) {
    ops.flags |= ST20_TX_FLAG_EXT_FRAME;
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_PACING) {
    ops.flags |= ST20_TX_FLAG_USER_PACING;
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_TIMESTAMP) {
    ops.flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  }
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_RTCP) {
    ops.flags |= ST20_TX_FLAG_ENABLE_RTCP;
  }
  if (config->base.flags & MTL_SESSION_FLAG_FORCE_NUMA) {
    ops.flags |= ST20_TX_FLAG_FORCE_NUMA;
    ops.socket_id = config->base.socket_id;
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_P_MAC) {
    ops.flags |= ST20_TX_FLAG_USER_P_MAC;
    memcpy(ops.tx_dst_mac[MTL_SESSION_PORT_P], config->tx_dst_mac[MTL_SESSION_PORT_P],
           MTL_MAC_ADDR_LEN);
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_R_MAC) {
    ops.flags |= ST20_TX_FLAG_USER_R_MAC;
    memcpy(ops.tx_dst_mac[MTL_SESSION_PORT_R], config->tx_dst_mac[MTL_SESSION_PORT_R],
           MTL_MAC_ADDR_LEN);
  }
  if (config->base.flags & MTL_SESSION_FLAG_EXACT_USER_PACING) {
    ops.flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  }
  if (config->base.flags & MTL_SESSION_FLAG_RTP_TIMESTAMP_EPOCH) {
    ops.flags |= ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  }
  if (config->base.flags & MTL_SESSION_FLAG_DISABLE_BULK) {
    ops.flags |= ST20_TX_FLAG_DISABLE_BULK;
  }
  if (config->base.flags & MTL_SESSION_FLAG_STATIC_PAD_P) {
    ops.flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;
  }

  /* Advanced TX options */
  if (config->start_vrx) ops.start_vrx = config->start_vrx;
  if (config->pad_interval) ops.pad_interval = config->pad_interval;
  if (config->rtp_timestamp_delta_us) ops.rtp_timestamp_delta_us = config->rtp_timestamp_delta_us;

  ops.notify_frame_late = video_tx_notify_frame_late;

  /* Create the low-level TX session */
  handle = st20_tx_create(impl, &ops);
  if (!handle) {
    err("%s(%s), st20_tx_create failed\n", __func__, s->name);
    mt_rte_free(ctx);
    return -EIO;
  }

  ctx->handle = handle;
  ctx->frame_size = st20_tx_get_framebuffer_size(handle);

  /* Link the inner session implementation */
  struct st_tx_video_session_handle_impl* handle_impl =
      (struct st_tx_video_session_handle_impl*)handle;
  s->inner.video_tx = handle_impl->impl;
  s->idx = s->inner.video_tx->idx;

  /* Allocate per-frame state array */
  {
    uint16_t fb_cnt = s->inner.video_tx->st20_frames_cnt;
    ctx->frame_state = mt_rte_zmalloc_socket(
        sizeof(enum tx_frame_state) * fb_cnt, s->socket_id);
    if (!ctx->frame_state) {
      err("%s(%s), failed to alloc frame_state array\n", __func__, s->name);
      st20_tx_free(handle);
      ctx->handle = NULL;
      s->inner.video_tx = NULL;
      mt_rte_free(ctx);
      return -ENOMEM;
    }
    ctx->frame_cnt = fb_cnt;
    for (uint16_t i = 0; i < fb_cnt; i++) {
      ctx->frame_state[i] = TX_FRAME_FREE;
    }
  }

  /* Allocate conversion source buffers if needed */
  if (!ctx->derive) {
    uint16_t fb_cnt = s->inner.video_tx->st20_frames_cnt;
    ctx->src_bufs = mt_rte_zmalloc_socket(sizeof(void*) * fb_cnt, s->socket_id);
    if (!ctx->src_bufs) {
      err("%s(%s), failed to alloc src_bufs array\n", __func__, s->name);
      st20_tx_free(handle);
      ctx->handle = NULL;
      s->inner.video_tx = NULL;
      mt_rte_free(ctx);
      return -ENOMEM;
    }
    ctx->src_bufs_cnt = fb_cnt;
    for (uint16_t i = 0; i < fb_cnt; i++) {
      ctx->src_bufs[i] = mt_rte_zmalloc_socket(ctx->src_frame_size, s->socket_id);
      if (!ctx->src_bufs[i]) {
        err("%s(%s), failed to alloc src_buf[%u], size %zu\n", __func__, s->name,
            i, ctx->src_frame_size);
        /* Cleanup already-allocated buffers */
        for (uint16_t j = 0; j < i; j++) {
          mt_rte_free(ctx->src_bufs[j]);
        }
        mt_rte_free(ctx->src_bufs);
        ctx->src_bufs = NULL;
        st20_tx_free(handle);
        ctx->handle = NULL;
        s->inner.video_tx = NULL;
        mt_rte_free(ctx);
        return -ENOMEM;
      }
    }
    info("%s(%s), allocated %u conversion src buffers, %zu bytes each\n", __func__,
         s->name, fb_cnt, ctx->src_frame_size);
  }

  info("%s(%s), created TX video session, frame_size %zu, fb_cnt %u, derive %d\n",
       __func__, s->name, ctx->frame_size, ops.framebuff_cnt, ctx->derive);

  return 0;
}

void mtl_video_tx_session_uinit(struct mtl_session_impl* s) {
  video_tx_destroy(s);
}
