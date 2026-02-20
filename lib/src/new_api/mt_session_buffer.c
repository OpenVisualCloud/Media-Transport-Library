/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_buffer.c
 *
 * Buffer wrapper implementation for the unified session API.
 * Wraps st_frame_trans as mtl_buffer_impl with public mtl_buffer_t view.
 */

#include "mt_session.h"

#include <errno.h>

#include "../mt_log.h"
#include "../mt_mem.h"

/*************************************************************************
 * Buffer Pool Management
 *************************************************************************/

int mtl_session_buffers_init(struct mtl_session_impl* s, uint32_t count) {
  struct mtl_buffer_impl* buffers;

  if (!count) return 0;

  buffers = mt_rte_zmalloc_socket(sizeof(*buffers) * count, s->socket_id);
  if (!buffers) {
    err("%s(%s), failed to alloc %u buffer wrappers\n", __func__, s->name, count);
    return -ENOMEM;
  }

  for (uint32_t i = 0; i < count; i++) {
    buffers[i].session = s;
    buffers[i].idx = i;
    buffers[i].frame_trans = NULL;
    buffers[i].user_ctx = NULL;
    buffers[i].user_owned = false;
    buffers[i].pub.priv = &buffers[i]; /* Link back to impl */
  }

  s->buffers = buffers;
  s->buffer_count = count;

  dbg("%s(%s), initialized %u buffer wrappers\n", __func__, s->name, count);
  return 0;
}

void mtl_session_buffers_uinit(struct mtl_session_impl* s) {
  if (s->buffers) {
    mt_rte_free(s->buffers);
    s->buffers = NULL;
  }
  s->buffer_count = 0;
}

/*************************************************************************
 * Buffer Fill from st_frame_trans
 *************************************************************************/

void mtl_buffer_fill_from_frame_trans(struct mtl_buffer_impl* b,
                                      struct st_frame_trans* ft,
                                      mtl_media_type_t type) {
  mtl_buffer_t* pub = &b->pub;

  b->frame_trans = ft;

  /* Common fields */
  pub->data = ft->addr;
  pub->iova = ft->iova;
  pub->priv = b;
  pub->user_data = ft->user_meta;
  pub->flags = 0;

  if (ft->flags & ST_FT_FLAG_EXT) {
    pub->flags |= MTL_BUF_FLAG_EXT;
  }

  /* Fill type-specific fields from frame metadata */
  switch (type) {
    case MTL_TYPE_VIDEO: {
      struct mtl_session_impl* s = b->session;
      if (s->direction == MTL_SESSION_TX) {
        struct st20_tx_frame_meta* meta = &ft->tv_meta;
        pub->timestamp = meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
        pub->rtp_timestamp = meta->rtp_timestamp;
        pub->epoch = meta->epoch;
        pub->status = MTL_FRAME_STATUS_COMPLETE;
      } else {
        struct st20_rx_frame_meta* meta = &ft->rv_meta;
        pub->timestamp = meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
        pub->rtp_timestamp = meta->rtp_timestamp;
        pub->epoch = meta->timestamp_first_pkt; /* RX meta has no epoch, use first pkt ts */

        /* Frame status from metadata */
        if (meta->status == ST_FRAME_STATUS_COMPLETE)
          pub->status = MTL_FRAME_STATUS_COMPLETE;
        else
          pub->status = MTL_FRAME_STATUS_INCOMPLETE;

        if (pub->status != MTL_FRAME_STATUS_COMPLETE) {
          pub->flags |= MTL_BUF_FLAG_INCOMPLETE;
        }

        /* RX video-specific extended fields */
        pub->video.pkts_total = meta->pkts_total;
        pub->video.pkts_recv[0] = meta->pkts_recv[0];
        if (MTL_SESSION_PORT_MAX > 1) pub->video.pkts_recv[1] = meta->pkts_recv[1];
      }
      break;
    }

    case MTL_TYPE_AUDIO: {
      struct mtl_session_impl* s = b->session;
      if (s->direction == MTL_SESSION_TX) {
        struct st30_tx_frame_meta* meta = &ft->ta_meta;
        pub->rtp_timestamp = meta->rtp_timestamp;
        pub->epoch = meta->epoch;
      } else {
        struct st30_rx_frame_meta* meta = &ft->ra_meta;
        pub->rtp_timestamp = meta->rtp_timestamp;
        pub->timestamp =
            meta->tfmt == ST10_TIMESTAMP_FMT_TAI ? meta->timestamp : 0;
      }
      break;
    }

    case MTL_TYPE_ANCILLARY: {
      /* Ancillary basic fill */
      pub->timestamp = 0;
      pub->rtp_timestamp = 0;
      break;
    }

    default:
      break;
  }
}

/*************************************************************************
 * Frame Trans Pool Helpers
 *************************************************************************/

struct st_frame_trans* mtl_session_get_frame_trans(struct mtl_session_impl* s) {
  struct st_frame_trans* frames = NULL;
  uint32_t count = 0;

  /* Get frame array from inner session */
  switch (s->type) {
    case MTL_TYPE_VIDEO:
      if (s->direction == MTL_SESSION_TX && s->inner.video_tx) {
        frames = s->inner.video_tx->st20_frames;
        count = s->inner.video_tx->st20_frames_cnt;
      } else if (s->direction == MTL_SESSION_RX && s->inner.video_rx) {
        frames = s->inner.video_rx->st20_frames;
        count = s->inner.video_rx->st20_frames_cnt;
      }
      break;
    default:
      err("%s(%s), unsupported type %d\n", __func__, s->name, s->type);
      return NULL;
  }

  if (!frames || !count) {
    err("%s(%s), no frames available\n", __func__, s->name);
    return NULL;
  }

  /* Find a free frame (refcnt == 0) */
  for (uint32_t i = 0; i < count; i++) {
    if (rte_atomic32_read(&frames[i].refcnt) == 0) {
      rte_atomic32_inc(&frames[i].refcnt);
      return &frames[i];
    }
  }

  return NULL; /* No free frames */
}

void mtl_session_put_frame_trans(struct st_frame_trans* ft) {
  if (ft) {
    rte_atomic32_dec(&ft->refcnt);
  }
}

/*************************************************************************
 * User-Owned Buffer Management
 *************************************************************************/

#define MTL_USER_BUF_RING_SIZE 32 /* Must be power of 2 */

int mtl_session_user_buf_init(struct mtl_session_impl* s, uint16_t frame_cnt) {
  char ring_name[RTE_RING_NAMESIZE];

  snprintf(ring_name, sizeof(ring_name), "mtl_ub_%p", s);

  s->user_buf_ring =
      rte_ring_create(ring_name, MTL_USER_BUF_RING_SIZE, s->socket_id, 0);
  if (!s->user_buf_ring) {
    err("%s(%s), failed to create user buffer ring\n", __func__, s->name);
    return -ENOMEM;
  }

  s->user_buf_ctx =
      mt_rte_zmalloc_socket(sizeof(void*) * frame_cnt, s->socket_id);
  if (!s->user_buf_ctx) {
    err("%s(%s), failed to alloc user_buf_ctx array\n", __func__, s->name);
    rte_ring_free(s->user_buf_ring);
    s->user_buf_ring = NULL;
    return -ENOMEM;
  }
  s->user_buf_ctx_cnt = frame_cnt;

  dbg("%s(%s), initialized user buffer ring, frame_cnt %u\n", __func__, s->name,
      frame_cnt);
  return 0;
}

void mtl_session_user_buf_uinit(struct mtl_session_impl* s) {
  /* Drain and free any remaining entries in the ring */
  if (s->user_buf_ring) {
    void* obj = NULL;
    while (rte_ring_dequeue(s->user_buf_ring, &obj) == 0 && obj) {
      mt_rte_free(obj);
      obj = NULL;
    }
    rte_ring_free(s->user_buf_ring);
    s->user_buf_ring = NULL;
  }

  if (s->user_buf_ctx) {
    mt_rte_free(s->user_buf_ctx);
    s->user_buf_ctx = NULL;
  }
  s->user_buf_ctx_cnt = 0;

  /* Free DMA registrations */
  for (uint8_t i = 0; i < s->dma_registration_cnt; i++) {
    if (s->dma_registrations[i]) {
      mt_rte_free(s->dma_registrations[i]);
      s->dma_registrations[i] = NULL;
    }
  }
  s->dma_registration_cnt = 0;
}

int mtl_session_user_buf_enqueue(struct mtl_session_impl* s, void* data,
                                 mtl_iova_t iova, size_t size, void* user_ctx) {
  if (!s->user_buf_ring) return -EINVAL;

  struct mtl_user_buffer_entry* entry =
      mt_rte_zmalloc_socket(sizeof(*entry), s->socket_id);
  if (!entry) {
    err("%s(%s), failed to alloc user buffer entry\n", __func__, s->name);
    return -ENOMEM;
  }

  entry->data = data;
  entry->iova = iova;
  entry->size = size;
  entry->user_ctx = user_ctx;

  if (rte_ring_enqueue(s->user_buf_ring, entry) != 0) {
    mt_rte_free(entry);
    dbg("%s(%s), user buffer ring full\n", __func__, s->name);
    return -ENOSPC;
  }

  return 0;
}

int mtl_session_user_buf_dequeue(struct mtl_session_impl* s,
                                 struct mtl_user_buffer_entry* entry) {
  if (!s->user_buf_ring) return -EINVAL;

  void* obj = NULL;
  if (rte_ring_dequeue(s->user_buf_ring, &obj) != 0 || !obj) {
    return -EAGAIN;
  }

  struct mtl_user_buffer_entry* queued = (struct mtl_user_buffer_entry*)obj;
  *entry = *queued;
  mt_rte_free(queued);
  return 0;
}

mtl_iova_t mtl_session_lookup_iova(struct mtl_session_impl* s, void* addr,
                                   size_t size) {
  /* Search registered DMA memory regions */
  for (uint8_t i = 0; i < s->dma_registration_cnt; i++) {
    struct mtl_dma_mem_impl* reg = s->dma_registrations[i];
    if (!reg) continue;

    uintptr_t region_start = (uintptr_t)reg->addr;
    uintptr_t region_end = region_start + reg->size;
    uintptr_t buf_start = (uintptr_t)addr;
    uintptr_t buf_end = buf_start + size;

    if (buf_start >= region_start && buf_end <= region_end) {
      /* Buffer is within this registered region */
      size_t offset = buf_start - region_start;
      return reg->iova + offset;
    }
  }

  /* Fallback: try direct IOVA lookup via DPDK */
  mtl_iova_t iova = rte_mem_virt2iova(addr);
  if (iova != RTE_BAD_IOVA && iova != 0) {
    return iova;
  }

  /* Try hugepage lookup if parent available */
  if (s->parent) {
    iova = mtl_hp_virt2iova(s->parent, addr);
    if (iova != MTL_BAD_IOVA && iova != 0) {
      return iova;
    }
  }

  err("%s(%s), failed to find IOVA for addr %p\n", __func__, s->name, addr);
  return MTL_BAD_IOVA;
}
