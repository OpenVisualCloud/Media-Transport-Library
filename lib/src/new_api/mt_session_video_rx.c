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

#include "mt_session_video_common.h"

#include "../mt_log.h"
#include "../mt_mem.h"

/*************************************************************************
 * Callback Context
 *************************************************************************/

struct video_rx_ctx {
  struct mtl_session_impl* session;
  st20_rx_handle handle;           /**< low-level RX handle */
  struct video_convert_ctx convert; /**< shared format conversion context */

  /** Lock-free ring to queue received frames for buffer_get() */
  struct rte_ring* ready_ring;

  /* User ext_frame callback (if any) */
  int (*user_query_ext_frame)(void* priv, struct st_ext_frame* ext_frame,
                              struct mtl_buffer* frame_meta);
  void* user_priv;
};

/*************************************************************************
 * Internal Helpers
 *************************************************************************/

/** Get the video_rx_ctx from session (caller must ensure session is valid). */
static inline struct video_rx_ctx* rx_ctx_from_session(struct mtl_session_impl* s) {
  return s->inner.video_rx->ops.priv;
}

/*************************************************************************
 * ST20 RX Callbacks → Unified Event Queue / Ready Ring
 *************************************************************************/

/**
 * Save received frame metadata into st_frame_trans for later retrieval.
 * The meta pointer from the callback is transient, so we must copy now.
 */
static void rx_save_frame_metadata(struct st_rx_video_session_impl* rx_impl,
                                   void* frame, struct st20_rx_frame_meta* meta) {
  if (!meta || !rx_impl) return;

  for (uint16_t i = 0; i < rx_impl->st20_frames_cnt; i++) {
    if (rx_impl->st20_frames[i].addr == frame) {
      rx_impl->st20_frames[i].rv_meta = *meta;
      return;
    }
  }
}

/**
 * Enqueue a received frame pointer onto the ready ring.
 * Returns 0 on success, or drops the frame and updates stats on failure.
 */
static int rx_enqueue_frame(struct video_rx_ctx* ctx, void* frame) {
  if (!ctx->ready_ring) return -EINVAL;

  if (rte_ring_enqueue(ctx->ready_ring, frame) != 0) {
    struct mtl_session_impl* s = ctx->session;
    dbg("%s(%s), ready ring full, dropping frame\n", __func__, s->name);
    st20_rx_put_framebuff(ctx->handle, frame);

    __atomic_add_fetch(&s->stats.buffers_dropped, 1, __ATOMIC_RELAXED);
    return -ENOSPC;
  }
  return 0;
}

/**
 * Post a buffer-ready event with optional timestamp from RX metadata.
 */
static void rx_post_buffer_ready_event(struct mtl_session_impl* s,
                                       struct st20_rx_frame_meta* meta,
                                       void* user_ctx) {
  mtl_event_t event = {0};
  event.type = MTL_EVENT_BUFFER_READY;
  event.ctx = user_ctx;
  if (meta) {
    event.timestamp = meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
  }
  mtl_session_event_post(s, &event);
}

/**
 * notify_frame_ready callback - library delivered a received frame.
 * Thread context: library datapath thread. Must be non-blocking.
 *
 * For user-owned mode: saves the user_ctx (from ext_frame opaque) per frame_idx,
 * so that buffer_get or event_poll can return it to the app.
 */
static int video_rx_notify_frame_ready(void* priv, void* frame,
                                       struct st20_rx_frame_meta* meta) {
  struct video_rx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  rx_save_frame_metadata(s->inner.video_rx, frame, meta);

  /*
   * User-owned mode via buffer_post (no explicit query_ext_frame):
   * The library receives into its own internal framebuffers.
   * Here we convert/copy into the user's buffer, return the library frame,
   * and post an event carrying the user context.
   */
  if (s->ownership == MTL_BUFFER_USER_OWNED && !ctx->user_query_ext_frame) {
    struct mtl_user_buffer_entry entry;
    if (mtl_session_user_buf_dequeue(s, &entry) != 0) {
      dbg("%s(%s), no user buffer for received frame, dropping\n", __func__, s->name);
      st20_rx_put_framebuff(ctx->handle, frame);
      __atomic_add_fetch(&s->stats.buffers_dropped, 1, __ATOMIC_RELAXED);
      return 0;
    }

    /* Convert transport → app format, or copy if derive */
    struct video_convert_ctx* cvt = &ctx->convert;
    if (cvt->derive) {
      size_t copy_len = cvt->transport_frame_size;
      if (copy_len > entry.size) copy_len = entry.size;
      mtl_memcpy(entry.data, frame, copy_len);
    } else {
      int ret = video_convert_frame(cvt, frame, 0, cvt->transport_frame_size,
                                    entry.data, entry.iova, cvt->app_frame_size,
                                    false /* RX */);
      if (ret < 0) {
        err("%s(%s), conversion failed: %d\n", __func__, s->name, ret);
        mtl_session_user_buf_enqueue(s, entry.data, entry.iova, entry.size,
                                     entry.user_ctx);
        st20_rx_put_framebuff(ctx->handle, frame);
        return 0;
      }
    }

    /* Return library frame immediately */
    st20_rx_put_framebuff(ctx->handle, frame);

    /* Post event with user context */
    rx_post_buffer_ready_event(s, meta, entry.user_ctx);
    __atomic_add_fetch(&s->stats.buffers_processed, 1, __ATOMIC_RELAXED);
    return 0;
  }

  /* User-owned mode with explicit query_ext_frame: save opaque user_ctx per frame */
  if (s->ownership == MTL_BUFFER_USER_OWNED && s->user_buf_ctx) {
    struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
    for (uint16_t i = 0; i < rx_impl->st20_frames_cnt; i++) {
      if (rx_impl->st20_frames[i].addr == frame) {
        if (i < s->user_buf_ctx_cnt) {
          s->user_buf_ctx[i] = rx_impl->st20_frames[i].user_meta;
        }
        break;
      }
    }
  }

  if (rx_enqueue_frame(ctx, frame) == 0) {
    rx_post_buffer_ready_event(s, meta, NULL);
  }

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

  (void)reply; /* Accept detected format with default reply */
  return 0;
}

/**
 * Wrapper for query_ext_frame: translates st20_ext_frame to st_ext_frame.
 *
 * In user-owned mode without an explicit query_ext_frame callback from the app,
 * this implementation dequeues from the user_buf_ring (populated by buffer_post).
 */
static int video_rx_query_ext_frame_wrapper(void* priv,
                                            struct st20_ext_frame* st20_ext,
                                            struct st20_rx_frame_meta* meta) {
  struct video_rx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  /* If app provided its own query_ext_frame callback, use it */
  if (ctx->user_query_ext_frame) {
    struct st_ext_frame ext = {0};
    mtl_buffer_t buf = {0};
    buf.video.width = meta->width;
    buf.video.height = meta->height;
    buf.size = meta->frame_total_size;

    int ret = ctx->user_query_ext_frame(ctx->user_priv, &ext, &buf);
    if (ret < 0) return ret;

    st20_ext->buf_addr = ext.addr[0];
    st20_ext->buf_iova = ext.iova[0];
    st20_ext->buf_len = ext.size;
    st20_ext->opaque = ext.opaque;
    return 0;
  }

  /* User-owned mode via buffer_post(): dequeue from ring */
  struct mtl_user_buffer_entry entry;
  int ret = mtl_session_user_buf_dequeue(s, &entry);
  if (ret < 0) {
    dbg("%s(%s), no user buffer available for ext_frame\n", __func__, s->name);
    return -EAGAIN;
  }

  st20_ext->buf_addr = entry.data;
  st20_ext->buf_iova = entry.iova;
  st20_ext->buf_len = entry.size;
  st20_ext->opaque = entry.user_ctx;

  return 0;
}

/*************************************************************************
 * Buffer Get/Put Helpers
 *************************************************************************/

/**
 * Find the st_frame_trans matching a frame address.
 * Returns the frame_trans and sets *frame_idx, or NULL if not found.
 */
static struct st_frame_trans* rx_find_frame_trans(
    struct st_rx_video_session_impl* rx_impl, void* frame, uint16_t* frame_idx) {
  for (uint16_t i = 0; i < rx_impl->st20_frames_cnt; i++) {
    if (rx_impl->st20_frames[i].addr == frame) {
      *frame_idx = i;
      return &rx_impl->st20_frames[i];
    }
  }
  return NULL;
}

/**
 * Fill buffer status and timestamp fields from RX frame metadata.
 */
static void rx_fill_buffer_status(mtl_buffer_t* pub, struct st20_rx_frame_meta* meta) {
  pub->rtp_timestamp = meta->rtp_timestamp;
  pub->tfmt = meta->tfmt;
  pub->timestamp = meta->timestamp;

  if (meta->status == ST_FRAME_STATUS_COMPLETE ||
      meta->status == ST_FRAME_STATUS_RECONSTRUCTED) {
    pub->status = MTL_FRAME_STATUS_COMPLETE;
  } else {
    pub->status = MTL_FRAME_STATUS_INCOMPLETE;
    pub->flags |= MTL_BUF_FLAG_INCOMPLETE;
  }
}

/**
 * Fill video-specific fields in the buffer from RX metadata.
 */
static void rx_fill_buffer_video_fields(mtl_buffer_t* pub,
                                        struct st20_rx_frame_meta* meta,
                                        struct video_rx_ctx* ctx) {
  pub->video.width = meta->width;
  pub->video.height = meta->height;
  pub->video.pkts_total = meta->pkts_total;
  pub->video.pkts_recv[0] = meta->pkts_recv[0];
  if (MTL_SESSION_PORT_MAX > 1)
    pub->video.pkts_recv[1] = meta->pkts_recv[1];
  pub->video.interlaced = ctx->convert.interlaced;
  pub->video.second_field = meta->second_field;
}

/**
 * Fill user metadata pass-through fields from frame_trans.
 */
static void rx_fill_user_metadata(mtl_buffer_t* pub, struct st_frame_trans* ft) {
  if (ft->user_meta && ft->user_meta_data_size > 0) {
    pub->user_meta = ft->user_meta;
    pub->user_meta_size = ft->user_meta_data_size;
  }
}

/**
 * Perform format conversion for a received frame (transport → app format).
 * On success, sets pub->data/size to the converted buffer.
 * On failure, returns the transport frame to the library.
 */
static int rx_convert_and_fill_buffer(struct video_rx_ctx* ctx,
                                      struct st_frame_trans* ft,
                                      uint16_t frame_idx,
                                      mtl_buffer_t* pub) {
  struct video_convert_ctx* cvt = &ctx->convert;

  if (cvt->derive || !cvt->app_bufs || frame_idx >= cvt->app_bufs_cnt ||
      !cvt->app_bufs[frame_idx]) {
    /* Derive mode or missing buffer - give transport buffer directly */
    pub->data = ft->addr;
    pub->iova = ft->iova;
    pub->size = cvt->transport_frame_size;
    pub->data_size = ft->rv_meta.frame_recv_size > 0 ? ft->rv_meta.frame_recv_size
                                                     : cvt->transport_frame_size;
    pub->video.fmt = st_frame_fmt_from_transport(cvt->transport_fmt);
    return 0;
  }

  /* Convert transport frame → app format */
  int ret = video_convert_frame(cvt, ft->addr, ft->iova, cvt->transport_frame_size,
                                cvt->app_bufs[frame_idx], 0, cvt->app_frame_size,
                                false /* is_tx=false, RX direction */);
  if (ret < 0) {
    st20_rx_put_framebuff(ctx->handle, ft->addr);
    return ret;
  }

  pub->data = cvt->app_bufs[frame_idx];
  pub->iova = 0;
  pub->size = cvt->app_frame_size;
  pub->data_size = cvt->app_frame_size;
  pub->video.fmt = cvt->frame_fmt;
  return 0;
}

/**
 * Try to dequeue one received frame and populate the buffer.
 * Returns 0 on success with *buf set, or negative errno.
 *
 * Thread safety: lock-free. The ready_ring supports multi-consumer dequeue.
 * Each dequeued frame has a unique frame_idx mapping to a unique buffer wrapper.
 */
static int rx_try_dequeue_frame(struct mtl_session_impl* s, mtl_buffer_t** buf) {
  struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  void* frame = NULL;

  if (!ctx->ready_ring || rte_ring_dequeue(ctx->ready_ring, &frame) != 0 || !frame) {
    return -EAGAIN;
  }

  /* Find the frame_trans for this address */
  uint16_t frame_idx = 0;
  struct st_frame_trans* ft = rx_find_frame_trans(rx_impl, frame, &frame_idx);
  if (!ft) {
    err("%s(%s), frame addr %p not found\n", __func__, s->name, frame);
    return -EIO;
  }

  /* Fill buffer wrapper (under buffer_lock) */
  struct mtl_buffer_impl* b = &s->buffers[frame_idx % s->buffer_count];
  b->frame_trans = ft;
  b->idx = frame_idx;

  mtl_buffer_t* pub = &b->pub;
  memset(pub, 0, sizeof(*pub));
  pub->priv = b;
  pub->flags = 0;

  struct st20_rx_frame_meta* meta = &ft->rv_meta;
  rx_fill_buffer_status(pub, meta);

  /* Convert or pass through the frame data */
  int ret = rx_convert_and_fill_buffer(ctx, ft, frame_idx, pub);
  if (ret < 0) {
    b->frame_trans = NULL;
    return ret;
  }

  rx_fill_buffer_video_fields(pub, meta, ctx);
  rx_fill_user_metadata(pub, ft);

  /* For user-owned mode: attach user_ctx to buffer */
  if (s->ownership == MTL_BUFFER_USER_OWNED && s->user_buf_ctx &&
      frame_idx < s->user_buf_ctx_cnt) {
    pub->user_data = s->user_buf_ctx[frame_idx];
    b->user_ctx = s->user_buf_ctx[frame_idx];
    b->user_owned = true;
    s->user_buf_ctx[frame_idx] = NULL;
  }

  /* Update stats (lock-free, relaxed ordering for counters) */
  __atomic_add_fetch(&s->stats.buffers_processed, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&s->stats.bytes_processed, pub->data_size, __ATOMIC_RELAXED);

  *buf = pub;
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

  /* Clean up user-owned buffer resources */
  mtl_session_user_buf_uinit(s);

  if (ctx) {
    if (ctx->ready_ring) {
      rte_ring_free(ctx->ready_ring);
      ctx->ready_ring = NULL;
    }
    video_convert_bufs_free(&ctx->convert);
    mt_rte_free(ctx);
  }
}

static int video_rx_buffer_get(struct mtl_session_impl* s, mtl_buffer_t** buf,
                               uint32_t timeout_ms) {
  uint64_t deadline_ns = video_calc_deadline_ns(timeout_ms);

  do {
    if (mtl_session_check_stopped(s)) return -EAGAIN;

    int ret = rx_try_dequeue_frame(s, buf);

    if (ret == 0) return 0;
    if (ret != -EAGAIN) return ret; /* Real error */

    /* No frame available */
    if (timeout_ms == 0) return -ETIMEDOUT;

    usleep(100);

    if (video_deadline_reached(deadline_ns)) return -ETIMEDOUT;
  } while (1);
}

static int video_rx_buffer_put(struct mtl_session_impl* s, mtl_buffer_t* buf) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  struct mtl_buffer_impl* b = MTL_BUFFER_IMPL(buf);

  if (!b || !b->frame_trans) return -EINVAL;

  /* Return frame to the low-level library (thread-safe via st20_rx_put_framebuff) */
  int ret = st20_rx_put_framebuff(ctx->handle, b->frame_trans->addr);

  b->frame_trans = NULL;
  b->user_ctx = NULL;
  b->user_owned = false;

  return ret;
}

/*************************************************************************
 * User-Owned Buffer Operations (RX)
 *************************************************************************/

/**
 * Post a user-owned buffer for receiving (zero-copy mode).
 *
 * Looks up IOVA from registered DMA regions, then enqueues the buffer.
 * The query_ext_frame callback will dequeue it when the library needs a buffer.
 * Received data is signaled via MTL_EVENT_BUFFER_READY with user_ctx.
 */
static int video_rx_buffer_post(struct mtl_session_impl* s, void* data,
                                size_t size, void* user_ctx) {
  if (s->ownership != MTL_BUFFER_USER_OWNED) {
    err("%s(%s), buffer_post only valid in USER_OWNED mode\n", __func__, s->name);
    return -EINVAL;
  }

  /* Look up IOVA for the user buffer */
  mtl_iova_t iova = mtl_session_lookup_iova(s, data, size);
  if (iova == MTL_BAD_IOVA) {
    err("%s(%s), failed to get IOVA for buffer %p (not registered?)\n",
        __func__, s->name, data);
    return -EINVAL;
  }

  return mtl_session_user_buf_enqueue(s, data, iova, size, user_ctx);
}

/**
 * Register a memory region for DMA access (user-owned mode).
 * After registration, buffers from this region can be passed to buffer_post().
 */
static int video_rx_mem_register(struct mtl_session_impl* s, void* addr,
                                 size_t size, mtl_dma_mem_t** handle) {
  if (s->dma_registration_cnt >= 8) {
    err("%s(%s), too many DMA registrations (max 8)\n", __func__, s->name);
    return -ENOSPC;
  }

  struct mtl_dma_mem_impl* reg =
      mt_rte_zmalloc_socket(sizeof(*reg), s->socket_id);
  if (!reg) return -ENOMEM;

  reg->parent = s->parent;
  reg->addr = addr;
  reg->size = size;

  /* Try to get IOVA mapping */
  reg->iova = rte_mem_virt2iova(addr);
  if (reg->iova == RTE_BAD_IOVA || reg->iova == 0) {
    reg->iova = mtl_hp_virt2iova(s->parent, addr);
    if (reg->iova == MTL_BAD_IOVA || reg->iova == 0) {
      warn("%s(%s), could not get IOVA for region %p, will try per-buffer lookup\n",
           __func__, s->name, addr);
      reg->iova = 0;
      reg->hp_mapped = false;
    } else {
      reg->hp_mapped = true;
    }
  }

  s->dma_registrations[s->dma_registration_cnt++] = reg;

  info("%s(%s), registered DMA region %p, size %zu, iova 0x%" PRIx64 "\n",
       __func__, s->name, addr, size, reg->iova);

  *handle = (mtl_dma_mem_t*)reg;
  return 0;
}

/**
 * Unregister a previously registered DMA memory region.
 */
static int video_rx_mem_unregister(struct mtl_session_impl* s,
                                   mtl_dma_mem_t* handle) {
  struct mtl_dma_mem_impl* reg = (struct mtl_dma_mem_impl*)handle;

  for (uint8_t i = 0; i < s->dma_registration_cnt; i++) {
    if (s->dma_registrations[i] == reg) {
      info("%s(%s), unregistered DMA region %p\n", __func__, s->name, reg->addr);
      mt_rte_free(reg);
      for (uint8_t j = i; j < s->dma_registration_cnt - 1; j++) {
        s->dma_registrations[j] = s->dma_registrations[j + 1];
      }
      s->dma_registrations[--s->dma_registration_cnt] = NULL;
      return 0;
    }
  }

  err("%s(%s), DMA handle not found\n", __func__, s->name);
  return -EINVAL;
}

static int video_rx_stats_get(struct mtl_session_impl* s,
                              mtl_session_stats_t* stats) {
  /* Read stats atomically — no lock needed */
  stats->buffers_processed =
      __atomic_load_n(&s->stats.buffers_processed, __ATOMIC_RELAXED);
  stats->bytes_processed =
      __atomic_load_n(&s->stats.bytes_processed, __ATOMIC_RELAXED);
  stats->buffers_dropped =
      __atomic_load_n(&s->stats.buffers_dropped, __ATOMIC_RELAXED);
  stats->epochs_missed =
      __atomic_load_n(&s->stats.epochs_missed, __ATOMIC_RELAXED);

  struct st_rx_video_session_impl* rx_impl = s->inner.video_rx;
  if (rx_impl) {
    uint32_t free_cnt = 0;
    for (int i = 0; i < rx_impl->st20_frames_cnt; i++) {
      if (rte_atomic32_read(&rx_impl->st20_frames[i].refcnt) == 0) free_cnt++;
    }
    stats->buffers_free = free_cnt;
    stats->buffers_in_use = rx_impl->st20_frames_cnt - free_cnt;
  } else {
    stats->buffers_free = 0;
    stats->buffers_in_use = 0;
  }

  return 0;
}

static int video_rx_update_source(struct mtl_session_impl* s,
                                  const struct st_rx_source_info* src) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  if (ctx && ctx->handle) {
    return st20_rx_update_source(ctx->handle, (struct st_rx_source_info*)src);
  }
  return -EINVAL;
}

static size_t video_rx_get_frame_size(struct mtl_session_impl* s) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  if (!ctx) return 0;
  return ctx->convert.derive ? ctx->convert.transport_frame_size
                             : ctx->convert.app_frame_size;
}

static int video_rx_io_stats_get(struct mtl_session_impl* s, void* stats,
                                 size_t stats_size) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  if (!ctx || !ctx->handle) return -EINVAL;
  if (stats_size < sizeof(struct st20_rx_user_stats)) return -EINVAL;
  return st20_rx_get_session_stats(ctx->handle, (struct st20_rx_user_stats*)stats);
}

static int video_rx_io_stats_reset(struct mtl_session_impl* s) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
  if (!ctx || !ctx->handle) return -EINVAL;
  return st20_rx_reset_session_stats(ctx->handle);
}

static int video_rx_pcap_dump(struct mtl_session_impl* s, uint32_t max_pkts,
                              bool sync, struct st_pcap_dump_meta* meta) {
  struct video_rx_ctx* ctx = rx_ctx_from_session(s);
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

/*************************************************************************
 * Video RX VTable
 *************************************************************************/

const mtl_session_vtable_t mtl_video_rx_vtable = {
    .start = video_rx_start,
    .stop = video_rx_stop,
    .destroy = video_rx_destroy,
    .buffer_get = video_rx_buffer_get,
    .buffer_put = video_rx_buffer_put,
    .buffer_post = video_rx_buffer_post,
    .buffer_flush = NULL,
    .mem_register = video_rx_mem_register,
    .mem_unregister = video_rx_mem_unregister,
    .event_poll = video_session_event_poll,   /* shared implementation */
    .get_event_fd = NULL,
    .stats_get = video_rx_stats_get,
    .stats_reset = video_session_stats_reset, /* shared implementation */
    .get_frame_size = video_rx_get_frame_size,
    .io_stats_get = video_rx_io_stats_get,
    .io_stats_reset = video_rx_io_stats_reset,
    .pcap_dump = video_rx_pcap_dump,
    .update_destination = NULL,
    .update_source = video_rx_update_source,
    .slice_ready = NULL,
    .slice_query = video_rx_slice_query,
    .get_plugin_info = NULL,
    .get_queue_meta = NULL,
};

/*************************************************************************
 * Session Initialization - Helpers
 *************************************************************************/

/**
 * Create the ready ring for received frame queuing.
 */
static int rx_create_ready_ring(struct video_rx_ctx* ctx, struct mtl_session_impl* s) {
  char ring_name[RTE_RING_NAMESIZE];
  snprintf(ring_name, sizeof(ring_name), "mtl_rx_%p", s);

  ctx->ready_ring =
      rte_ring_create(ring_name, 32, s->socket_id, RING_F_SP_ENQ);
  if (!ctx->ready_ring) {
    err("%s(%s), failed to create ready ring\n", __func__, s->name);
    return -ENOMEM;
  }
  return 0;
}

/**
 * Populate st20_rx_ops port fields from mtl_video_config_t.
 */
static void rx_fill_port_config(struct st20_rx_ops* ops,
                                const mtl_video_config_t* config) {
  memcpy(ops->port, config->rx_port.port, sizeof(ops->port));
  memcpy(ops->ip_addr, config->rx_port.ip_addr, sizeof(ops->ip_addr));
  ops->num_port = config->rx_port.num_port;
  if (ops->num_port == 0) ops->num_port = 1;
  memcpy(ops->udp_port, config->rx_port.udp_port, sizeof(ops->udp_port));
  ops->payload_type = config->rx_port.payload_type;
  ops->ssrc = config->rx_port.ssrc;
  memcpy(ops->mcast_sip_addr, config->rx_port.mcast_sip_addr,
         sizeof(ops->mcast_sip_addr));
}

/**
 * Populate st20_rx_ops video format fields from mtl_video_config_t.
 */
static void rx_fill_video_format(struct st20_rx_ops* ops,
                                 const mtl_video_config_t* config) {
  ops->width = config->width;
  ops->height = config->height;
  ops->fps = config->fps;
  ops->interlaced = config->interlaced;
  ops->fmt = config->transport_fmt;
  ops->packing = config->packing;
  ops->linesize = config->linesize;
}

/**
 * Map unified session flags to st20_rx flags and set callbacks.
 */
static void rx_apply_session_flags(struct st20_rx_ops* ops,
                                   const mtl_video_config_t* config,
                                   struct video_rx_ctx* ctx) {
  /* Auto-detect */
  if (config->enable_auto_detect) {
    ops->flags |= ST20_RX_FLAG_AUTO_DETECT;
    ops->notify_detected = video_rx_notify_detected;
  }

  /* Vsync events - use shared callback via session pointer */
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_VSYNC) {
    ops->notify_event = video_session_notify_event;
  }

  /* User-owned ext_frame mode: only when app provides an explicit callback.
   * The default buffer_post() path uses library's internal framebuffers and
   * converts/copies into user buffers in notify_frame_ready. */
  if (config->base.ownership == MTL_BUFFER_USER_OWNED &&
      config->base.query_ext_frame) {
    ops->query_ext_frame = video_rx_query_ext_frame_wrapper;
    ops->flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ctx->user_query_ext_frame = config->base.query_ext_frame;
    ctx->user_priv = config->base.priv;
  }

  /* Individual flag mappings */
  if (config->base.flags & MTL_SESSION_FLAG_RECEIVE_INCOMPLETE_FRAME)
    ops->flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
  if (config->base.flags & MTL_SESSION_FLAG_DMA_OFFLOAD)
    ops->flags |= ST20_RX_FLAG_DMA_OFFLOAD;
  if (config->base.flags & MTL_SESSION_FLAG_DATA_PATH_ONLY)
    ops->flags |= ST20_RX_FLAG_DATA_PATH_ONLY;
  if (config->base.flags & MTL_SESSION_FLAG_HDR_SPLIT)
    ops->flags |= ST20_RX_FLAG_HDR_SPLIT;
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_RTCP)
    ops->flags |= ST20_RX_FLAG_ENABLE_RTCP;
  if (config->base.flags & MTL_SESSION_FLAG_FORCE_NUMA)
    ops->socket_id = config->base.socket_id;
  if (config->base.flags & MTL_SESSION_FLAG_USE_MULTI_THREADS)
    ops->flags |= ST20_RX_FLAG_USE_MULTI_THREADS;
  if (config->enable_timing_parser)
    ops->flags |= ST20_RX_FLAG_TIMING_PARSER_STAT;

  /* Advanced RX options */
  if (config->rx_burst_size) ops->rx_burst_size = config->rx_burst_size;
}

/**
 * Cleanup all resources on init failure.
 */
static void rx_cleanup_on_failure(struct video_rx_ctx* ctx) {
  if (ctx->handle) {
    st20_rx_free(ctx->handle);
    ctx->handle = NULL;
  }
  video_convert_bufs_free(&ctx->convert);
  if (ctx->ready_ring) {
    rte_ring_free(ctx->ready_ring);
    ctx->ready_ring = NULL;
  }
  mt_rte_free(ctx);
}

/*************************************************************************
 * Session Initialization
 *************************************************************************/

int mtl_video_rx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config) {
  int ret;

  /* Allocate callback context */
  struct video_rx_ctx* ctx = mt_rte_zmalloc_socket(sizeof(*ctx), s->socket_id);
  if (!ctx) {
    err("%s, failed to alloc ctx\n", __func__);
    return -ENOMEM;
  }
  ctx->session = s;

  /* Initialize format conversion (shared helper) */
  ret = video_convert_ctx_init(&ctx->convert, config, false /* RX */);
  if (ret < 0) {
    mt_rte_free(ctx);
    return ret;
  }
  s->video.frame_fmt = ctx->convert.frame_fmt;
  s->video.derive = ctx->convert.derive;

  /* Create frame queuing ring */
  ret = rx_create_ready_ring(ctx, s);
  if (ret < 0) {
    mt_rte_free(ctx);
    return ret;
  }

  /* Build st20_rx_ops from config */
  struct st20_rx_ops ops;
  memset(&ops, 0, sizeof(ops));

  rx_fill_port_config(&ops, config);
  rx_fill_video_format(&ops, config);

  ops.name = config->base.name;
  ops.priv = ctx;
  ops.framebuff_cnt = config->base.num_buffers;
  if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 2;

  /* Mode: frame vs slice */
  if (config->mode == MTL_VIDEO_MODE_SLICE) {
    ops.type = ST20_TYPE_SLICE_LEVEL;
    ops.slice_lines = config->height / 4;
  } else {
    ops.type = ST20_TYPE_FRAME_LEVEL;
  }

  ops.notify_frame_ready = video_rx_notify_frame_ready;

  rx_apply_session_flags(&ops, config, ctx);

  /* Create the low-level RX session */
  st20_rx_handle handle = st20_rx_create(impl, &ops);
  if (!handle) {
    err("%s(%s), st20_rx_create failed\n", __func__, s->name);
    rx_cleanup_on_failure(ctx);
    return -EIO;
  }

  ctx->handle = handle;
  ctx->convert.transport_frame_size = st20_rx_get_framebuffer_size(handle);

  /* Link inner session implementation */
  struct st_rx_video_session_handle_impl* handle_impl =
      (struct st_rx_video_session_handle_impl*)handle;
  s->inner.video_rx = handle_impl->impl;
  s->idx = s->inner.video_rx->idx;

  /* Allocate conversion buffers if needed (shared helper) */
  if (!ctx->convert.derive) {
    ret = video_convert_bufs_alloc(&ctx->convert, s->inner.video_rx->st20_frames_cnt,
                                   s->socket_id);
    if (ret < 0) {
      s->inner.video_rx = NULL;
      rx_cleanup_on_failure(ctx);
      return ret;
    }
  }

  /* Initialize user-owned buffer management if needed */
  if (s->ownership == MTL_BUFFER_USER_OWNED) {
    ret = mtl_session_user_buf_init(s, s->inner.video_rx->st20_frames_cnt);
    if (ret < 0) {
      err("%s(%s), user_buf_init failed: %d\n", __func__, s->name, ret);
      s->inner.video_rx = NULL;
      rx_cleanup_on_failure(ctx);
      return ret;
    }
  }

  info("%s(%d), transport fmt %s, output fmt %s, frame_size %zu, fb_cnt %u, derive %d%s\n",
       __func__, s->idx, st20_fmt_name(config->transport_fmt),
       st_frame_fmt_name(config->frame_fmt),
       ctx->convert.transport_frame_size, ops.framebuff_cnt,
       ctx->convert.derive,
       s->ownership == MTL_BUFFER_USER_OWNED ? ", user-owned" : "");

  return 0;
}

void mtl_video_rx_session_uinit(struct mtl_session_impl* s) {
  video_rx_destroy(s);
}
