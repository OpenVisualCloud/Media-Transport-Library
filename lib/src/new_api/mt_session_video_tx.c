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

#include "mt_session_video_common.h"

#include "../mt_log.h"
#include "../mt_mem.h"

/*************************************************************************
 * TX Frame State Machine
 *
 * Tracks the app-facing lifecycle of each framebuffer, separate from
 * the low-level library's internal refcnt.
 *
 *   FREE → APP_OWNED → READY → TRANSMITTING → FREE
 *          (get)       (put)   (get_next_frame) (frame_done)
 *
 * Thread safety: lock-free using C11 __atomic operations.
 * - tx_try_claim_frame: CAS (FREE → APP_OWNED) with __ATOMIC_ACQ_REL
 * - buffer_put: atomic store (APP_OWNED → READY) with __ATOMIC_RELEASE
 * - get_next_frame: CAS (READY → TRANSMITTING) with __ATOMIC_ACQUIRE
 * - frame_done: atomic store (TRANSMITTING → FREE) with __ATOMIC_RELEASE
 *
 * This forms an acquire-release chain ensuring frame data visibility:
 *   frame_done(RELEASE:FREE) → try_claim(ACQUIRE:APP_OWNED) →
 *   buffer_put(RELEASE:READY) → get_next_frame(ACQUIRE:TRANSMITTING) → ...
 *************************************************************************/

enum tx_frame_state {
  TX_FRAME_FREE = 0,        /**< Available for buffer_get */
  TX_FRAME_APP_OWNED = 1,   /**< App is filling it (between get and put) */
  TX_FRAME_READY = 2,       /**< App called put, awaiting get_next_frame */
  TX_FRAME_TRANSMITTING = 3 /**< Library picked it for transmission */
};

/*************************************************************************
 * Callback Context
 *************************************************************************/

struct video_tx_ctx {
  struct mtl_session_impl* session;
  st20_tx_handle handle;           /**< low-level TX handle */
  struct video_convert_ctx convert; /**< shared format conversion context */

  /** Per-frame state tracking (protected by session->buffer_lock) */
  enum tx_frame_state* frame_state;
  uint16_t frame_cnt;

  /* User slice callback (if any) */
  int (*user_query_lines_ready)(void* priv, uint16_t frame_idx, uint16_t* lines_ready);
  void* user_priv;
};

/*************************************************************************
 * Internal Helpers
 *************************************************************************/

/** Get the video_tx_ctx from session (caller must ensure session is valid). */
static inline struct video_tx_ctx* tx_ctx_from_session(struct mtl_session_impl* s) {
  return s->inner.video_tx->ops.priv;
}

/*************************************************************************
 * ST20 TX Callbacks → Unified Event Queue
 *
 * These run on library datapath threads. They acquire buffer_lock
 * briefly for frame_state transitions.
 *************************************************************************/

/**
 * get_next_frame callback - library asks which frame to transmit next.
 * Scans for a frame in READY state and transitions it to TRANSMITTING.
 *
 * For user-owned mode: also checks the user_buf_ring for posted buffers
 * and sets ext_frame on a free frame slot before marking it READY.
 */
static int video_tx_get_next_frame(void* priv, uint16_t* next_frame_idx,
                                   struct st20_tx_frame_meta* meta) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  (void)meta;

  if (!tx_impl || !tx_impl->st20_frames) return -EIO;
  if (!ctx->frame_state) return -EAGAIN; /* init not yet complete */

  /* User-owned mode: check for posted buffers and bind to free frame slots */
  if (s->ownership == MTL_BUFFER_USER_OWNED) {
    struct mtl_user_buffer_entry entry;
    while (mtl_session_user_buf_dequeue(s, &entry) == 0) {
      /* Find a free frame slot to bind this user buffer */
      bool bound = false;
      for (uint16_t i = 0; i < tx_impl->st20_frames_cnt; i++) {
        enum tx_frame_state expected = TX_FRAME_FREE;
        if (__atomic_compare_exchange_n(&ctx->frame_state[i], &expected,
                                        TX_FRAME_APP_OWNED, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
          int ret;

          if (ctx->convert.derive) {
            /* Formats match — true zero-copy via ext_frame.
             * st20_tx sends the user buffer directly; no conversion. */
            struct st20_ext_frame ext = {0};
            ext.buf_addr = entry.data;
            ext.buf_iova = entry.iova;
            ext.buf_len = entry.size;
            ext.opaque = entry.user_ctx;

            ret = st20_tx_set_ext_frame(ctx->handle, i, &ext);
            if (ret < 0) {
              err("%s(%s), st20_tx_set_ext_frame failed for slot %u: %d\n",
                  __func__, s->name, i, ret);
              __atomic_store_n(&ctx->frame_state[i], TX_FRAME_FREE, __ATOMIC_RELEASE);
              continue;
            }
          } else {
            /* Format conversion needed: convert user data (app format) into
             * the library's own framebuffer (transport format).
             * st20_tx will transmit the correctly-formatted framebuffer. */
            ret = video_convert_frame(
                &ctx->convert, entry.data, entry.iova, entry.size,
                tx_impl->st20_frames[i].addr, tx_impl->st20_frames[i].iova,
                ctx->convert.transport_frame_size, true /* TX: app→transport */);
            if (ret < 0) {
              err("%s(%s), format conversion failed for slot %u: %d\n",
                  __func__, s->name, i, ret);
              __atomic_store_n(&ctx->frame_state[i], TX_FRAME_FREE, __ATOMIC_RELEASE);
              continue;
            }
          }

          /* Save user context for completion event */
          if (s->user_buf_ctx && i < s->user_buf_ctx_cnt) {
            s->user_buf_ctx[i] = entry.user_ctx;
          }

          /* Mark ready for transmission */
          __atomic_store_n(&ctx->frame_state[i], TX_FRAME_READY, __ATOMIC_RELEASE);
          bound = true;
          break;
        }
      }
      if (!bound) {
        dbg("%s(%s), no free frame slot for user buffer, requeueing\n",
            __func__, s->name);
        /* Re-enqueue - no slot free yet */
        mtl_session_user_buf_enqueue(s, entry.data, entry.iova, entry.size,
                                     entry.user_ctx);
        break;
      }
    }
  }

  for (uint16_t i = 0; i < tx_impl->st20_frames_cnt; i++) {
    enum tx_frame_state expected = TX_FRAME_READY;
    if (__atomic_compare_exchange_n(&ctx->frame_state[i], &expected,
                                    TX_FRAME_TRANSMITTING, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      *next_frame_idx = i;
      rte_atomic32_set(&tx_impl->st20_frames[i].refcnt, 0);
      return 0;
    }
  }

  return -EBUSY;
}

/**
 * notify_frame_done callback - transmission complete, release frame.
 * Transitions frame from TRANSMITTING → FREE.
 * For user-owned mode: includes user_ctx in the completion event.
 */
static int video_tx_notify_frame_done(void* priv, uint16_t frame_idx,
                                      struct st20_tx_frame_meta* meta) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;

  if (frame_idx >= tx_impl->st20_frames_cnt) return -EINVAL;

  /* Retrieve user context before clearing frame state */
  void* user_ctx = NULL;
  if (s->ownership == MTL_BUFFER_USER_OWNED && s->user_buf_ctx &&
      frame_idx < s->user_buf_ctx_cnt) {
    user_ctx = s->user_buf_ctx[frame_idx];
    s->user_buf_ctx[frame_idx] = NULL;
  }

  __atomic_store_n(&ctx->frame_state[frame_idx], TX_FRAME_FREE, __ATOMIC_RELEASE);

  /* Update stats (lock-free, relaxed ordering for counters) */
  __atomic_add_fetch(&s->stats.buffers_processed, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&s->stats.bytes_processed, ctx->convert.transport_frame_size,
                     __ATOMIC_RELAXED);

  /* Post completion event */
  mtl_event_t event = {0};
  event.type = MTL_EVENT_BUFFER_DONE;
  event.timestamp = meta ? meta->epoch : 0;
  event.ctx = user_ctx; /* User context for user-owned mode */
  mtl_session_event_post(s, &event);

  return 0;
}

/**
 * notify_frame_late callback - frame missed its epoch.
 */
static int video_tx_notify_frame_late(void* priv, uint64_t epoch_skipped) {
  struct video_tx_ctx* ctx = priv;
  struct mtl_session_impl* s = ctx->session;

  __atomic_add_fetch(&s->stats.epochs_missed, 1, __ATOMIC_RELAXED);

  mtl_event_t event = {0};
  event.type = MTL_EVENT_FRAME_LATE;
  event.frame_late.epoch_skipped = epoch_skipped;
  mtl_session_event_post(s, &event);

  return 0;
}

/**
 * Wrapper for query_frame_lines_ready (slice mode).
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
 * Buffer Get/Put Helpers
 *************************************************************************/

/**
 * Fill buffer data pointers for the app.
 * In conversion mode, gives the app-format source buffer.
 * In derive mode, gives the transport framebuffer directly.
 */
static void tx_fill_buffer_data(mtl_buffer_t* pub, struct video_tx_ctx* ctx,
                                struct st_tx_video_session_impl* tx_impl,
                                uint16_t frame_idx) {
  struct video_convert_ctx* cvt = &ctx->convert;

  if (!cvt->derive && cvt->app_bufs && frame_idx < cvt->app_bufs_cnt &&
      cvt->app_bufs[frame_idx]) {
    /* Conversion mode: give app the source buffer (app pixel format) */
    pub->data = cvt->app_bufs[frame_idx];
    pub->iova = 0;
    pub->size = cvt->app_frame_size;
    pub->data_size = cvt->app_frame_size;
    pub->video.fmt = cvt->frame_fmt;
  } else {
    /* Derive mode: give app the transport framebuffer directly */
    pub->data = tx_impl->st20_frames[frame_idx].addr;
    pub->iova = tx_impl->st20_frames[frame_idx].iova;
    pub->size = cvt->transport_frame_size;
    pub->data_size = cvt->transport_frame_size;
    pub->video.fmt = st_frame_fmt_from_transport(cvt->transport_fmt);
  }

  pub->video.width = cvt->width;
  pub->video.height = cvt->height;
}

/**
 * Try to find a free frame and claim it for the app.
 * Returns 0 on success with *buf set, or -EAGAIN if no frame free.
 *
 * Thread safety: lock-free. Uses atomic CAS to claim exclusive ownership.
 * Multiple threads can call this concurrently; only one CAS succeeds per frame.
 */
static int tx_try_claim_frame(struct mtl_session_impl* s, mtl_buffer_t** buf) {
  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);

  for (uint16_t i = 0; i < tx_impl->st20_frames_cnt; i++) {
    if (rte_atomic32_read(&tx_impl->st20_frames[i].refcnt) != 0) continue;

    enum tx_frame_state expected = TX_FRAME_FREE;
    if (__atomic_compare_exchange_n(&ctx->frame_state[i], &expected,
                                    TX_FRAME_APP_OWNED, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      /* Claimed this frame for the app */

      struct mtl_buffer_impl* b = &s->buffers[i % s->buffer_count];
      b->frame_trans = &tx_impl->st20_frames[i];
      b->idx = i;

      mtl_buffer_t* pub = &b->pub;
      memset(pub, 0, sizeof(*pub));
      pub->priv = b;
      pub->flags = 0;
      pub->status = MTL_FRAME_STATUS_COMPLETE;

      tx_fill_buffer_data(pub, ctx, tx_impl, i);

      *buf = pub;
      return 0;
    }
  }

  return -EAGAIN;
}

/**
 * Perform format conversion on buffer_put (app format → transport).
 * Returns 0 on success, negative errno on failure.
 */
static int tx_convert_on_put(struct video_tx_ctx* ctx, struct mtl_buffer_impl* b) {
  struct video_convert_ctx* cvt = &ctx->convert;

  if (cvt->derive || !cvt->app_bufs || b->idx >= cvt->app_bufs_cnt ||
      !cvt->app_bufs[b->idx]) {
    return 0; /* No conversion needed */
  }

  return video_convert_frame(cvt, cvt->app_bufs[b->idx], 0, cvt->app_frame_size,
                             b->frame_trans->addr, b->frame_trans->iova,
                             cvt->transport_frame_size, true /* TX direction */);
}

/**
 * Pass user metadata and timestamp from the buffer to the frame_trans.
 */
static void tx_apply_buffer_metadata(mtl_buffer_t* buf, struct st_frame_trans* ft) {
  if (buf->user_meta && buf->user_meta_size > 0) {
    ft->tv_meta.user_meta = buf->user_meta;
    ft->tv_meta.user_meta_size = buf->user_meta_size;
  } else {
    ft->tv_meta.user_meta = NULL;
    ft->tv_meta.user_meta_size = 0;
  }

  if (buf->timestamp) {
    ft->tv_meta.timestamp = buf->timestamp;
    ft->tv_meta.tfmt = buf->tfmt;
  }
}

/*************************************************************************
 * VTable Implementation
 *************************************************************************/

static int video_tx_start(struct mtl_session_impl* s) {
  (void)s;
  return 0;
}

static int video_tx_stop(struct mtl_session_impl* s) {
  (void)s;
  return 0;
}

static void video_tx_destroy(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = NULL;

  if (s->inner.video_tx) {
    ctx = s->inner.video_tx->ops.priv;
  }

  /* Free the low-level session */
  if (ctx && ctx->handle) {
    st20_tx_free(ctx->handle);
    ctx->handle = NULL;
  }

  s->inner.video_tx = NULL;

  /* Clean up user-owned buffer resources */
  mtl_session_user_buf_uinit(s);

  if (ctx) {
    if (ctx->frame_state) {
      mt_rte_free(ctx->frame_state);
      ctx->frame_state = NULL;
    }
    video_convert_bufs_free(&ctx->convert);
    mt_rte_free(ctx);
  }
}

static int video_tx_buffer_get(struct mtl_session_impl* s, mtl_buffer_t** buf,
                               uint32_t timeout_ms) {
  uint64_t deadline_ns = video_calc_deadline_ns(timeout_ms);

  do {
    if (mtl_session_check_stopped(s)) return -EAGAIN;

    int ret = tx_try_claim_frame(s, buf);

    if (ret == 0) return 0;

    /* No free frame - check timeout */
    if (timeout_ms == 0) return -ETIMEDOUT;

    usleep(100);

    if (video_deadline_reached(deadline_ns)) return -ETIMEDOUT;
  } while (1);
}

static int video_tx_buffer_put(struct mtl_session_impl* s, mtl_buffer_t* buf) {
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);
  struct mtl_buffer_impl* b = MTL_BUFFER_IMPL(buf);

  if (!b || !b->frame_trans) return -EINVAL;

  /* Perform format conversion if needed (app → transport) */
  int ret = tx_convert_on_put(ctx, b);
  if (ret < 0) {
    __atomic_store_n(&ctx->frame_state[b->idx], TX_FRAME_FREE, __ATOMIC_RELEASE);
    return ret;
  }

  /* Apply metadata to the low-level frame */
  tx_apply_buffer_metadata(buf, b->frame_trans);

  /* Mark frame as ready for transmission (atomic release ensures data visibility) */
  __atomic_store_n(&ctx->frame_state[b->idx], TX_FRAME_READY, __ATOMIC_RELEASE);

  return 0;
}

/*************************************************************************
 * User-Owned Buffer Operations (TX)
 *************************************************************************/

/**
 * Post a user-owned buffer for transmission (zero-copy mode).
 *
 * Looks up IOVA from registered DMA regions, then enqueues the buffer.
 * The get_next_frame callback will bind it to a frame slot and transmit.
 * Completion is signaled via MTL_EVENT_BUFFER_DONE with user_ctx.
 */
static int video_tx_buffer_post(struct mtl_session_impl* s, void* data,
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
static int video_tx_mem_register(struct mtl_session_impl* s, void* addr,
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
    /* Try hugepage mapping */
    reg->iova = mtl_hp_virt2iova(s->parent, addr);
    if (reg->iova == MTL_BAD_IOVA || reg->iova == 0) {
      /* Memory might be from a custom allocator - try to use it anyway.
       * The IOVA lookup will try rte_mem_virt2iova per-buffer later. */
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
static int video_tx_mem_unregister(struct mtl_session_impl* s,
                                   mtl_dma_mem_t* handle) {
  struct mtl_dma_mem_impl* reg = (struct mtl_dma_mem_impl*)handle;

  for (uint8_t i = 0; i < s->dma_registration_cnt; i++) {
    if (s->dma_registrations[i] == reg) {
      info("%s(%s), unregistered DMA region %p\n", __func__, s->name, reg->addr);
      mt_rte_free(reg);
      /* Shift remaining entries */
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

static int video_tx_stats_get(struct mtl_session_impl* s,
                              mtl_session_stats_t* stats) {
  /* Read stats atomically — no lock needed, no deadlock possible */
  stats->buffers_processed =
      __atomic_load_n(&s->stats.buffers_processed, __ATOMIC_RELAXED);
  stats->bytes_processed =
      __atomic_load_n(&s->stats.bytes_processed, __ATOMIC_RELAXED);
  stats->buffers_dropped =
      __atomic_load_n(&s->stats.buffers_dropped, __ATOMIC_RELAXED);
  stats->epochs_missed =
      __atomic_load_n(&s->stats.epochs_missed, __ATOMIC_RELAXED);

  struct st_tx_video_session_impl* tx_impl = s->inner.video_tx;
  struct video_tx_ctx* ctx = tx_impl ? tx_impl->ops.priv : NULL;
  if (tx_impl && ctx && ctx->frame_state) {
    uint32_t free_cnt = 0;
    for (uint16_t i = 0; i < ctx->frame_cnt; i++) {
      if (__atomic_load_n(&ctx->frame_state[i], __ATOMIC_RELAXED) == TX_FRAME_FREE)
        free_cnt++;
    }
    stats->buffers_free = free_cnt;
    stats->buffers_in_use = ctx->frame_cnt - free_cnt;
  } else {
    stats->buffers_free = 0;
    stats->buffers_in_use = 0;
  }

  return 0;
}

static int video_tx_update_destination(struct mtl_session_impl* s,
                                       const struct st_tx_dest_info* dst) {
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);
  if (ctx && ctx->handle) {
    return st20_tx_update_destination(ctx->handle, (struct st_tx_dest_info*)dst);
  }
  return -EINVAL;
}

static size_t video_tx_get_frame_size(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);
  if (!ctx) return 0;
  return ctx->convert.derive ? ctx->convert.transport_frame_size
                             : ctx->convert.app_frame_size;
}

static int video_tx_io_stats_get(struct mtl_session_impl* s, void* stats,
                                 size_t stats_size) {
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);
  if (!ctx || !ctx->handle) return -EINVAL;
  if (stats_size < sizeof(struct st20_tx_user_stats)) return -EINVAL;
  return st20_tx_get_session_stats(ctx->handle, (struct st20_tx_user_stats*)stats);
}

static int video_tx_io_stats_reset(struct mtl_session_impl* s) {
  struct video_tx_ctx* ctx = tx_ctx_from_session(s);
  if (!ctx || !ctx->handle) return -EINVAL;
  return st20_tx_reset_session_stats(ctx->handle);
}

static int video_tx_slice_ready(struct mtl_session_impl* s, mtl_buffer_t* buf,
                                uint16_t lines) {
  (void)s;
  (void)buf;
  (void)lines;
  return 0;
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
    .buffer_post = video_tx_buffer_post,
    .buffer_flush = NULL,
    .mem_register = video_tx_mem_register,
    .mem_unregister = video_tx_mem_unregister,
    .event_poll = video_session_event_poll,   /* shared implementation */
    .get_event_fd = NULL,
    .stats_get = video_tx_stats_get,
    .stats_reset = video_session_stats_reset, /* shared implementation */
    .get_frame_size = video_tx_get_frame_size,
    .io_stats_get = video_tx_io_stats_get,
    .io_stats_reset = video_tx_io_stats_reset,
    .pcap_dump = NULL,
    .update_destination = video_tx_update_destination,
    .update_source = NULL,
    .slice_ready = video_tx_slice_ready,
    .slice_query = NULL,
    .get_plugin_info = NULL,
    .get_queue_meta = NULL,
};

/*************************************************************************
 * Session Initialization - Helpers
 *************************************************************************/

/**
 * Populate st20_tx_ops port fields from mtl_video_config_t.
 */
static void tx_fill_port_config(struct st20_tx_ops* ops,
                                const mtl_video_config_t* config) {
  memcpy(ops->port, config->tx_port.port, sizeof(ops->port));
  memcpy(ops->dip_addr, config->tx_port.dip_addr, sizeof(ops->dip_addr));
  ops->num_port = config->tx_port.num_port;
  if (ops->num_port == 0) ops->num_port = 1;
  memcpy(ops->udp_port, config->tx_port.udp_port, sizeof(ops->udp_port));
  ops->payload_type = config->tx_port.payload_type;
  ops->ssrc = config->tx_port.ssrc;
  memcpy(ops->udp_src_port, config->tx_port.udp_src_port, sizeof(ops->udp_src_port));
}

/**
 * Populate st20_tx_ops video format fields from mtl_video_config_t.
 */
static void tx_fill_video_format(struct st20_tx_ops* ops,
                                 const mtl_video_config_t* config) {
  ops->width = config->width;
  ops->height = config->height;
  ops->fps = config->fps;
  ops->interlaced = config->interlaced;
  ops->fmt = config->transport_fmt;
  ops->packing = config->packing;
  ops->pacing = config->pacing;
  ops->linesize = config->linesize;
}

/**
 * Map unified session flags to st20_tx flags and set callbacks.
 */
static void tx_apply_session_flags(struct st20_tx_ops* ops,
                                   const mtl_video_config_t* config,
                                   struct video_tx_ctx* ctx) {
  /* Vsync events - use shared callback */
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_VSYNC) {
    ops->notify_event = video_session_notify_event;
    ops->flags |= ST20_TX_FLAG_ENABLE_VSYNC;
  }

  /* Buffer ownership flags:
   * Only use ext_frame when formats match (derive) — true zero-copy.
   * When conversion is needed (!derive), we must convert app→transport into
   * the library's own framebuffers, so ext_frame cannot be used. */
  if (config->base.ownership == MTL_BUFFER_USER_OWNED && ctx->convert.derive)
    ops->flags |= ST20_TX_FLAG_EXT_FRAME;

  /* Individual flag mappings */
  if (config->base.flags & MTL_SESSION_FLAG_USER_PACING)
    ops->flags |= ST20_TX_FLAG_USER_PACING;
  if (config->base.flags & MTL_SESSION_FLAG_USER_TIMESTAMP)
    ops->flags |= ST20_TX_FLAG_USER_TIMESTAMP;
  if (config->base.flags & MTL_SESSION_FLAG_ENABLE_RTCP)
    ops->flags |= ST20_TX_FLAG_ENABLE_RTCP;
  if (config->base.flags & MTL_SESSION_FLAG_FORCE_NUMA) {
    ops->flags |= ST20_TX_FLAG_FORCE_NUMA;
    ops->socket_id = config->base.socket_id;
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_P_MAC) {
    ops->flags |= ST20_TX_FLAG_USER_P_MAC;
    memcpy(ops->tx_dst_mac[MTL_SESSION_PORT_P], config->tx_dst_mac[MTL_SESSION_PORT_P],
           MTL_MAC_ADDR_LEN);
  }
  if (config->base.flags & MTL_SESSION_FLAG_USER_R_MAC) {
    ops->flags |= ST20_TX_FLAG_USER_R_MAC;
    memcpy(ops->tx_dst_mac[MTL_SESSION_PORT_R], config->tx_dst_mac[MTL_SESSION_PORT_R],
           MTL_MAC_ADDR_LEN);
  }
  if (config->base.flags & MTL_SESSION_FLAG_EXACT_USER_PACING)
    ops->flags |= ST20_TX_FLAG_EXACT_USER_PACING;
  if (config->base.flags & MTL_SESSION_FLAG_RTP_TIMESTAMP_EPOCH)
    ops->flags |= ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH;
  if (config->base.flags & MTL_SESSION_FLAG_DISABLE_BULK)
    ops->flags |= ST20_TX_FLAG_DISABLE_BULK;
  if (config->base.flags & MTL_SESSION_FLAG_STATIC_PAD_P)
    ops->flags |= ST20_TX_FLAG_ENABLE_STATIC_PAD_P;

  /* Advanced TX options */
  if (config->start_vrx) ops->start_vrx = config->start_vrx;
  if (config->pad_interval) ops->pad_interval = config->pad_interval;
  if (config->rtp_timestamp_delta_us)
    ops->rtp_timestamp_delta_us = config->rtp_timestamp_delta_us;

  /* Slice mode */
  (void)ctx; /* ctx used for slice callback setup below */
}

/**
 * Allocate the per-frame state tracking array.
 * All frames start in TX_FRAME_FREE state.
 */
static int tx_alloc_frame_state(struct video_tx_ctx* ctx, uint16_t fb_cnt,
                                int socket_id) {
  ctx->frame_state =
      mt_rte_zmalloc_socket(sizeof(enum tx_frame_state) * fb_cnt, socket_id);
  if (!ctx->frame_state) {
    err("%s, failed to alloc frame_state array (%u entries)\n", __func__, fb_cnt);
    return -ENOMEM;
  }
  ctx->frame_cnt = fb_cnt;
  for (uint16_t i = 0; i < fb_cnt; i++) {
    ctx->frame_state[i] = TX_FRAME_FREE;
  }
  return 0;
}

/**
 * Cleanup all resources on init failure.
 */
static void tx_cleanup_on_failure(struct video_tx_ctx* ctx) {
  if (ctx->handle) {
    st20_tx_free(ctx->handle);
    ctx->handle = NULL;
  }
  if (ctx->frame_state) {
    mt_rte_free(ctx->frame_state);
    ctx->frame_state = NULL;
  }
  video_convert_bufs_free(&ctx->convert);
  mt_rte_free(ctx);
}

/*************************************************************************
 * Session Initialization
 *************************************************************************/

int mtl_video_tx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config) {
  int ret;

  /* Allocate callback context */
  struct video_tx_ctx* ctx = mt_rte_zmalloc_socket(sizeof(*ctx), s->socket_id);
  if (!ctx) {
    err("%s, failed to alloc ctx\n", __func__);
    return -ENOMEM;
  }
  ctx->session = s;

  /* Initialize format conversion (shared helper) */
  ret = video_convert_ctx_init(&ctx->convert, config, true /* TX */);
  if (ret < 0) {
    mt_rte_free(ctx);
    return ret;
  }
  s->video.frame_fmt = ctx->convert.frame_fmt;
  s->video.derive = ctx->convert.derive;

  /* Build st20_tx_ops from config */
  struct st20_tx_ops ops;
  memset(&ops, 0, sizeof(ops));

  tx_fill_port_config(&ops, config);
  tx_fill_video_format(&ops, config);

  ops.name = config->base.name;
  ops.priv = ctx;
  ops.framebuff_cnt = config->base.num_buffers;
  if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 2;

  /* Mode: frame vs slice */
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

  /* Core TX callbacks */
  ops.get_next_frame = video_tx_get_next_frame;
  ops.notify_frame_done = video_tx_notify_frame_done;
  ops.notify_frame_late = video_tx_notify_frame_late;

  tx_apply_session_flags(&ops, config, ctx);

  /* Allocate per-frame state tracking BEFORE st20_tx_create, because the
   * scheduler may call video_tx_get_next_frame as soon as the handle exists. */
  uint16_t fb_cnt = ops.framebuff_cnt;
  ret = tx_alloc_frame_state(ctx, fb_cnt, s->socket_id);
  if (ret < 0) {
    video_convert_bufs_free(&ctx->convert);
    mt_rte_free(ctx);
    return ret;
  }

  /* Initialize user-owned buffer management before create too */
  if (s->ownership == MTL_BUFFER_USER_OWNED) {
    ret = mtl_session_user_buf_init(s, fb_cnt);
    if (ret < 0) {
      err("%s(%s), user_buf_init failed: %d\n", __func__, s->name, ret);
      tx_cleanup_on_failure(ctx);
      return ret;
    }
  }

  /* Create the low-level TX session */
  st20_tx_handle handle = st20_tx_create(impl, &ops);
  if (!handle) {
    err("%s(%s), st20_tx_create failed\n", __func__, s->name);
    tx_cleanup_on_failure(ctx);
    return -EIO;
  }

  ctx->handle = handle;
  ctx->convert.transport_frame_size = st20_tx_get_framebuffer_size(handle);

  /* Link inner session implementation */
  struct st_tx_video_session_handle_impl* handle_impl =
      (struct st_tx_video_session_handle_impl*)handle;
  s->inner.video_tx = handle_impl->impl;
  s->idx = s->inner.video_tx->idx;

  /* Allocate conversion buffers if needed (shared helper) */
  if (!ctx->convert.derive) {
    ret = video_convert_bufs_alloc(&ctx->convert, fb_cnt, s->socket_id);
    if (ret < 0) {
      s->inner.video_tx = NULL;
      tx_cleanup_on_failure(ctx);
      return ret;
    }
  }

  info("%s(%d), transport fmt %s, input fmt: %s, frame_size %zu, fb_cnt %u, derive %d\n",
       __func__, s->idx, st20_fmt_name(config->transport_fmt),
       st_frame_fmt_name(config->frame_fmt),
       ctx->convert.transport_frame_size, ops.framebuff_cnt,
       ctx->convert.derive);

  return 0;
}

void mtl_video_tx_session_uinit(struct mtl_session_impl* s) {
  video_tx_destroy(s);
}
