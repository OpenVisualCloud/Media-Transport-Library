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

#include "mt_log.h"
#include "mt_mem.h"

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
