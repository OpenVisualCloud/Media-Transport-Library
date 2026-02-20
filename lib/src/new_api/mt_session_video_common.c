/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_video_common.c
 *
 * Shared implementation for video TX and RX sessions.
 * Contains format conversion, event polling, stats, and deadline helpers
 * that are identical or near-identical between TX and RX.
 */

#include "mt_session_video_common.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "../mt_log.h"
#include "../mt_mem.h"

/*************************************************************************
 * Format Conversion Context
 *************************************************************************/

int video_convert_ctx_init(struct video_convert_ctx* cvt,
                           const mtl_video_config_t* config, bool is_tx) {
  cvt->width = config->width;
  cvt->height = config->height;
  cvt->interlaced = config->interlaced;
  cvt->frame_fmt = config->frame_fmt;
  cvt->transport_fmt = config->transport_fmt;
  cvt->app_bufs = NULL;
  cvt->app_bufs_cnt = 0;

  /* Check if app format matches transport format (no conversion needed) */
  cvt->derive = st_frame_fmt_equal_transport(config->frame_fmt, config->transport_fmt);

  if (cvt->derive) return 0;

  /* Conversion needed: validate transport format */
  enum st_frame_fmt transport_frame_fmt =
      st_frame_fmt_from_transport(config->transport_fmt);
  if (transport_frame_fmt == ST_FRAME_FMT_MAX) {
    err("%s(%s), unsupported transport_fmt %d\n", __func__, config->base.name,
        config->transport_fmt);
    return -EINVAL;
  }

  /* Look up converter: direction depends on TX vs RX */
  enum st_frame_fmt src_fmt = is_tx ? config->frame_fmt : transport_frame_fmt;
  enum st_frame_fmt dst_fmt = is_tx ? transport_frame_fmt : config->frame_fmt;
  int ret = st_frame_get_converter(src_fmt, dst_fmt, &cvt->converter);
  if (ret < 0) {
    err("%s(%s), no converter from %s to %s\n", __func__, config->base.name,
        st_frame_fmt_name(src_fmt), st_frame_fmt_name(dst_fmt));
    return ret;
  }

  /* Calculate app-side frame size */
  cvt->app_frame_size =
      st_frame_size(config->frame_fmt, config->width, config->height,
                    config->interlaced);
  if (!cvt->app_frame_size) {
    err("%s(%s), failed to get frame size for fmt %s\n", __func__, config->base.name,
        st_frame_fmt_name(config->frame_fmt));
    return -EINVAL;
  }

  info("%s(%s), conversion enabled: %s %s %s, app_frame_size %zu\n", __func__,
       config->base.name, st_frame_fmt_name(src_fmt), is_tx ? "->" : "<-",
       st_frame_fmt_name(dst_fmt), cvt->app_frame_size);

  return 0;
}

int video_convert_bufs_alloc(struct video_convert_ctx* cvt, uint16_t fb_cnt,
                             int socket_id) {
  if (cvt->derive || fb_cnt == 0) return 0;

  cvt->app_bufs = mt_rte_zmalloc_socket(sizeof(void*) * fb_cnt, socket_id);
  if (!cvt->app_bufs) {
    err("%s, failed to alloc app_bufs array (%u entries)\n", __func__, fb_cnt);
    return -ENOMEM;
  }
  cvt->app_bufs_cnt = fb_cnt;

  for (uint16_t i = 0; i < fb_cnt; i++) {
    cvt->app_bufs[i] = mt_rte_zmalloc_socket(cvt->app_frame_size, socket_id);
    if (!cvt->app_bufs[i]) {
      err("%s, failed to alloc app_buf[%u], size %zu\n", __func__, i,
          cvt->app_frame_size);
      /* Cleanup already-allocated buffers */
      for (uint16_t j = 0; j < i; j++) {
        mt_rte_free(cvt->app_bufs[j]);
        cvt->app_bufs[j] = NULL;
      }
      mt_rte_free(cvt->app_bufs);
      cvt->app_bufs = NULL;
      cvt->app_bufs_cnt = 0;
      return -ENOMEM;
    }
  }

  info("%s, allocated %u conversion buffers, %zu bytes each\n", __func__, fb_cnt,
       cvt->app_frame_size);
  return 0;
}

void video_convert_bufs_free(struct video_convert_ctx* cvt) {
  if (!cvt->app_bufs) return;

  for (uint16_t i = 0; i < cvt->app_bufs_cnt; i++) {
    if (cvt->app_bufs[i]) {
      mt_rte_free(cvt->app_bufs[i]);
      cvt->app_bufs[i] = NULL;
    }
  }
  mt_rte_free(cvt->app_bufs);
  cvt->app_bufs = NULL;
  cvt->app_bufs_cnt = 0;
}

/*************************************************************************
 * Frame Conversion
 *************************************************************************/

int video_convert_frame(struct video_convert_ctx* cvt, void* src_data,
                        mtl_iova_t src_iova, size_t src_size, void* dst_data,
                        mtl_iova_t dst_iova, size_t dst_size, bool is_tx) {
  struct st_frame src_frame;
  struct st_frame dst_frame;

  memset(&src_frame, 0, sizeof(src_frame));
  memset(&dst_frame, 0, sizeof(dst_frame));

  if (is_tx) {
    /* TX: app format → transport format */
    src_frame.fmt = cvt->frame_fmt;
    dst_frame.fmt = st_frame_fmt_from_transport(cvt->transport_fmt);
  } else {
    /* RX: transport format → app format */
    src_frame.fmt = st_frame_fmt_from_transport(cvt->transport_fmt);
    dst_frame.fmt = cvt->frame_fmt;
  }

  /* Common fields for both frames */
  src_frame.width = cvt->width;
  src_frame.height = cvt->height;
  src_frame.interlaced = cvt->interlaced;
  src_frame.buffer_size = src_size;
  src_frame.data_size = src_size;
  st_frame_init_plane_single_src(&src_frame, src_data, src_iova);

  dst_frame.width = cvt->width;
  dst_frame.height = cvt->height;
  dst_frame.interlaced = cvt->interlaced;
  dst_frame.buffer_size = dst_size;
  dst_frame.data_size = dst_size;
  st_frame_init_plane_single_src(&dst_frame, dst_data, dst_iova);

  int ret = cvt->converter.convert_func(&src_frame, &dst_frame);
  if (ret < 0) {
    err("%s, conversion failed %d, %s -> %s\n", __func__, ret,
        st_frame_fmt_name(src_frame.fmt), st_frame_fmt_name(dst_frame.fmt));
  }
  return ret;
}

/*************************************************************************
 * Shared Event Poll
 *************************************************************************/

int video_session_event_poll(struct mtl_session_impl* s, mtl_event_t* event,
                             uint32_t timeout_ms) {
  if (mtl_session_check_stopped(s)) {
    return -EAGAIN;
  }

  /* Non-blocking dequeue attempt */
  if (s->event_ring) {
    void* obj = NULL;
    if (rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
      *event = *(mtl_event_t*)obj;
      mt_rte_free(obj);
      return 0;
    }
  }

  if (timeout_ms == 0) {
    return -ETIMEDOUT;
  }

  /* Poll with timeout */
  uint64_t deadline_ns = video_calc_deadline_ns(timeout_ms);

  while (!mtl_session_check_stopped(s)) {
    void* obj = NULL;
    if (s->event_ring && rte_ring_dequeue(s->event_ring, &obj) == 0 && obj) {
      *event = *(mtl_event_t*)obj;
      mt_rte_free(obj);
      return 0;
    }

    usleep(100);

    if (video_deadline_reached(deadline_ns)) return -ETIMEDOUT;
  }

  return -EAGAIN;
}

/*************************************************************************
 * Shared Stats Reset
 *************************************************************************/

int video_session_stats_reset(struct mtl_session_impl* s) {
  __atomic_store_n(&s->stats.buffers_processed, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s->stats.bytes_processed, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s->stats.buffers_dropped, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&s->stats.epochs_missed, 0, __ATOMIC_RELAXED);
  return 0;
}

/*************************************************************************
 * Shared Vsync Callback
 *************************************************************************/

int video_session_notify_event(void* priv, enum st_event ev, void* args) {
  struct mtl_session_impl* s = priv;

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
