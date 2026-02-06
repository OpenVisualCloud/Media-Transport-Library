/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session.c
 *
 * Core polymorphic dispatch layer for the unified session API.
 * This file implements the public mtl_session_* functions that dispatch
 * to type-specific implementations via the vtable.
 */

#include "mt_session.h"

#include <errno.h>

#include "mt_log.h"
#include "mt_mem.h"

/*************************************************************************
 * Session Allocation / Deallocation
 *************************************************************************/

struct mtl_session_impl* mtl_session_alloc(struct mtl_main_impl* impl, int socket_id) {
  struct mtl_session_impl* s;

  s = mt_rte_zmalloc_socket(sizeof(*s), socket_id);
  if (!s) {
    err("%s, failed to alloc session memory\n", __func__);
    return NULL;
  }

  s->parent = impl;
  s->socket_id = socket_id;
  s->state = MTL_SESSION_STATE_CREATED;
  s->stopped = false;
  s->event_fd = -1;
  rte_spinlock_init(&s->state_lock);
  rte_spinlock_init(&s->stats_lock);

  return s;
}

void mtl_session_free(struct mtl_session_impl* s) {
  if (!s) return;

  mtl_session_events_uinit(s);
  mtl_session_buffers_uinit(s);

  s->magic = 0; /* Invalidate handle */
  mt_rte_free(s);
}

/*************************************************************************
 * Session Creation - Type-Specific Entry Points
 *************************************************************************/

int mtl_video_session_create(mtl_handle mt, const mtl_video_config_t* config,
                             mtl_session_t** session) {
  struct mtl_main_impl* impl = mt;
  struct mtl_session_impl* s;
  int ret;

  if (!mt || !config || !session) {
    err("%s, invalid args\n", __func__);
    return -EINVAL;
  }

  int socket_id = config->base.socket_id;
  if (socket_id < 0) socket_id = mt_socket_id(impl, MTL_PORT_P);

  s = mtl_session_alloc(impl, socket_id);
  if (!s) return -ENOMEM;

  s->type = MTL_TYPE_VIDEO;
  s->direction = config->base.direction;
  s->ownership = config->base.ownership;
  s->flags = config->base.flags;
  s->notify_buffer_ready = config->base.notify_buffer_ready;
  s->notify_priv = config->base.priv;
  s->video.compressed = config->compressed;
  s->video.mode = config->mode;

  if (config->base.name) {
    snprintf(s->name, ST_MAX_NAME_LEN, "%s", config->base.name);
  }

  /* Initialize event queue */
  ret = mtl_session_events_init(s);
  if (ret < 0) {
    err("%s, events init failed: %d\n", __func__, ret);
    mtl_session_free(s);
    return ret;
  }

  /* Initialize type-specific session */
  if (config->base.direction == MTL_SESSION_TX) {
    s->magic = MTL_SESSION_MAGIC_VIDEO_TX;
    s->vt = &mtl_video_tx_vtable;
    ret = mtl_video_tx_session_init(s, impl, config);
  } else {
    s->magic = MTL_SESSION_MAGIC_VIDEO_RX;
    s->vt = &mtl_video_rx_vtable;
    ret = mtl_video_rx_session_init(s, impl, config);
  }

  if (ret < 0) {
    err("%s, session init failed: %d\n", __func__, ret);
    mtl_session_free(s);
    return ret;
  }

  /* Initialize buffer wrappers */
  if (config->base.num_buffers > 0) {
    ret = mtl_session_buffers_init(s, config->base.num_buffers);
    if (ret < 0) {
      err("%s, buffers init failed: %d\n", __func__, ret);
      if (config->base.direction == MTL_SESSION_TX)
        mtl_video_tx_session_uinit(s);
      else
        mtl_video_rx_session_uinit(s);
      mtl_session_free(s);
      return ret;
    }
  }

  info("%s(%s), created %s video %s session\n", __func__, s->name,
       config->compressed ? "ST22" : "ST20",
       config->base.direction == MTL_SESSION_TX ? "TX" : "RX");

  *session = MTL_SESSION_PUB(s);
  return 0;
}

int mtl_audio_session_create(mtl_handle mt, const mtl_audio_config_t* config,
                             mtl_session_t** session) {
  /* Audio session creation - to be implemented */
  (void)mt;
  (void)config;
  (void)session;
  err("%s, not yet implemented\n", __func__);
  return -ENOTSUP;
}

int mtl_ancillary_session_create(mtl_handle mt, const mtl_ancillary_config_t* config,
                                 mtl_session_t** session) {
  /* Ancillary session creation - to be implemented */
  (void)mt;
  (void)config;
  (void)session;
  err("%s, not yet implemented\n", __func__);
  return -ENOTSUP;
}

/*************************************************************************
 * Session Lifecycle - Polymorphic
 *************************************************************************/

int mtl_session_start(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    err("%s, invalid session handle\n", __func__);
    return -EINVAL;
  }

  rte_spinlock_lock(&s->state_lock);

  if (s->state == MTL_SESSION_STATE_STARTED) {
    rte_spinlock_unlock(&s->state_lock);
    return 0; /* Already started */
  }

  mtl_session_clear_stopped(s);

  rte_spinlock_unlock(&s->state_lock);

  int ret = 0;
  if (s->vt && s->vt->start) {
    ret = s->vt->start(s);
  }

  if (ret == 0) {
    info("%s(%s), session started\n", __func__, s->name);
  }

  return ret;
}

int mtl_session_stop(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  /* Set stopped flag - this is safe from signal handlers */
  mtl_session_set_stopped(s);

  /* Call type-specific stop if available */
  if (s->vt && s->vt->stop) {
    s->vt->stop(s);
  }

  dbg("%s(%s), session stopped\n", __func__, s->name);
  return 0;
}

bool mtl_session_is_stopped(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) return true;

  return mtl_session_check_stopped(s);
}

int mtl_session_destroy(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    err("%s, invalid session handle\n", __func__);
    return -EINVAL;
  }

  info("%s(%s), destroying session\n", __func__, s->name);

  /* Call type-specific destroy */
  if (s->vt && s->vt->destroy) {
    s->vt->destroy(s);
  }

  mtl_session_free(s);
  return 0;
}

mtl_media_type_t mtl_session_get_type(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) return MTL_TYPE_VIDEO; /* Default */

  return s->type;
}

/*************************************************************************
 * Buffer Operations - Polymorphic
 *************************************************************************/

int mtl_session_buffer_get(mtl_session_t* session, mtl_buffer_t** buffer,
                           uint32_t timeout_ms) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !buffer) {
    return -EINVAL;
  }

  /* Check stopped flag first - fast path for shutdown */
  if (mtl_session_check_stopped(s)) {
    return -EAGAIN;
  }

  if (!s->vt || !s->vt->buffer_get) {
    return -ENOTSUP;
  }

  return s->vt->buffer_get(s, buffer, timeout_ms);
}

int mtl_session_buffer_put(mtl_session_t* session, mtl_buffer_t* buffer) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !buffer) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->buffer_put) {
    return -ENOTSUP;
  }

  return s->vt->buffer_put(s, buffer);
}

int mtl_session_buffer_post(mtl_session_t* session, void* data, size_t size,
                            void* user_ctx) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !data) {
    return -EINVAL;
  }

  if (mtl_session_check_stopped(s)) {
    return -EAGAIN;
  }

  if (!s->vt || !s->vt->buffer_post) {
    return -ENOTSUP;
  }

  return s->vt->buffer_post(s, data, size, user_ctx);
}

int mtl_session_buffer_flush(mtl_session_t* session, uint32_t timeout_ms) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->buffer_flush) {
    return -ENOTSUP;
  }

  return s->vt->buffer_flush(s, timeout_ms);
}

/*************************************************************************
 * Memory Registration
 *************************************************************************/

int mtl_session_mem_register(mtl_session_t* session, void* addr, size_t size,
                             mtl_dma_mem_t** handle) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !addr || !handle) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->mem_register) {
    return -ENOTSUP;
  }

  return s->vt->mem_register(s, addr, size, handle);
}

int mtl_session_mem_unregister(mtl_session_t* session, mtl_dma_mem_t* handle) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !handle) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->mem_unregister) {
    return -ENOTSUP;
  }

  return s->vt->mem_unregister(s, handle);
}

/*************************************************************************
 * Event Polling
 *************************************************************************/

int mtl_session_event_poll(mtl_session_t* session, mtl_event_t* event,
                           uint32_t timeout_ms) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !event) {
    return -EINVAL;
  }

  /* Check stopped flag first */
  if (mtl_session_check_stopped(s)) {
    return -EAGAIN;
  }

  if (!s->vt || !s->vt->event_poll) {
    /* Default: try to dequeue from event ring */
    if (s->event_ring) {
      void* obj = NULL;
      if (rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
        mtl_event_t* ev = (mtl_event_t*)obj;
        *event = *ev;
        mt_rte_free(ev);
        return 0;
      }
    }
    return -ETIMEDOUT;
  }

  return s->vt->event_poll(s, event, timeout_ms);
}

int mtl_session_get_event_fd(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  if (s->vt && s->vt->get_event_fd) {
    return s->vt->get_event_fd(s);
  }

  return s->event_fd;
}

/*************************************************************************
 * Statistics
 *************************************************************************/

int mtl_session_stats_get(mtl_session_t* session, mtl_session_stats_t* stats) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !stats) {
    return -EINVAL;
  }

  if (s->vt && s->vt->stats_get) {
    return s->vt->stats_get(s, stats);
  }

  /* Default: return cached stats */
  rte_spinlock_lock(&s->stats_lock);
  *stats = s->stats;
  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

int mtl_session_stats_reset(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  if (s->vt && s->vt->stats_reset) {
    return s->vt->stats_reset(s);
  }

  rte_spinlock_lock(&s->stats_lock);
  memset(&s->stats, 0, sizeof(s->stats));
  rte_spinlock_unlock(&s->stats_lock);
  return 0;
}

size_t mtl_session_get_frame_size(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return 0;
  }

  if (!s->vt || !s->vt->get_frame_size) {
    return 0;
  }

  return s->vt->get_frame_size(s);
}

int mtl_session_io_stats_get(mtl_session_t* session, void* stats, size_t stats_size) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !stats || !stats_size) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->io_stats_get) {
    return -ENOTSUP;
  }

  return s->vt->io_stats_get(s, stats, stats_size);
}

int mtl_session_io_stats_reset(mtl_session_t* session) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->io_stats_reset) {
    return -ENOTSUP;
  }

  return s->vt->io_stats_reset(s);
}

int mtl_session_pcap_dump(mtl_session_t* session, uint32_t max_dump_packets,
                          bool sync, struct st_pcap_dump_meta* meta) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->pcap_dump) {
    return -ENOTSUP;
  }

  return s->vt->pcap_dump(s, max_dump_packets, sync, meta);
}

/*************************************************************************
 * Online Updates
 *************************************************************************/

int mtl_session_update_destination(mtl_session_t* session,
                                   const struct st_tx_dest_info* dst) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !dst) {
    return -EINVAL;
  }

  if (s->direction != MTL_SESSION_TX) {
    err("%s(%s), not a TX session\n", __func__, s->name);
    return -EINVAL;
  }

  if (!s->vt || !s->vt->update_destination) {
    return -ENOTSUP;
  }

  return s->vt->update_destination(s, dst);
}

int mtl_session_update_source(mtl_session_t* session,
                              const struct st_rx_source_info* src) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !src) {
    return -EINVAL;
  }

  if (s->direction != MTL_SESSION_RX) {
    err("%s(%s), not an RX session\n", __func__, s->name);
    return -EINVAL;
  }

  if (!s->vt || !s->vt->update_source) {
    return -ENOTSUP;
  }

  return s->vt->update_source(s, src);
}

/*************************************************************************
 * Slice Mode
 *************************************************************************/

int mtl_session_slice_ready(mtl_session_t* session, mtl_buffer_t* buffer,
                            uint16_t lines_ready) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !buffer) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->slice_ready) {
    return -ENOTSUP;
  }

  return s->vt->slice_ready(s, buffer, lines_ready);
}

int mtl_session_slice_query(mtl_session_t* session, mtl_buffer_t* buffer,
                            uint16_t* lines_ready) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !buffer || !lines_ready) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->slice_query) {
    return -ENOTSUP;
  }

  return s->vt->slice_query(s, buffer, lines_ready);
}

/*************************************************************************
 * Plugin Info
 *************************************************************************/

int mtl_session_get_plugin_info(mtl_session_t* session, mtl_plugin_info_t* info) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !info) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->get_plugin_info) {
    return -ENOTSUP;
  }

  return s->vt->get_plugin_info(s, info);
}

/*************************************************************************
 * Queue Meta
 *************************************************************************/

int mtl_session_get_queue_meta(mtl_session_t* session, struct st_queue_meta* meta) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s) || !meta) {
    return -EINVAL;
  }

  if (!s->vt || !s->vt->get_queue_meta) {
    return -ENOTSUP;
  }

  return s->vt->get_queue_meta(s, meta);
}

/*************************************************************************
 * Block Timeout
 *************************************************************************/

int mtl_session_set_block_timeout(mtl_session_t* session, uint64_t timeout_us) {
  struct mtl_session_impl* s = MTL_SESSION_IMPL(session);

  if (!s || !MTL_SESSION_VALID(s)) {
    return -EINVAL;
  }

  (void)timeout_us; /* TODO: implement when block mode is needed */
  return 0;
}
