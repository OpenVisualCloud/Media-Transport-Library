/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session_video_common.h
 *
 * Shared helpers for video TX and RX session implementations.
 * Reduces code duplication between mt_session_video_tx.c and
 * mt_session_video_rx.c.
 */

#ifndef _MT_SESSION_VIDEO_COMMON_H_
#define _MT_SESSION_VIDEO_COMMON_H_

#include "mt_session.h"

#include "../st2110/st_convert.h"
#include "../st2110/st_fmt.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*************************************************************************
 * Shared Format Conversion Context
 *
 * Common fields used by both video_tx_ctx and video_rx_ctx for
 * format conversion between app pixel format and transport format.
 *************************************************************************/

struct video_convert_ctx {
  bool derive;                         /**< true if no conversion needed */
  enum st_frame_fmt frame_fmt;         /**< app pixel format */
  enum st20_fmt transport_fmt;         /**< wire format */
  struct st_frame_converter converter; /**< cached converter function */
  size_t app_frame_size;               /**< frame size in app pixel format */
  size_t transport_frame_size;         /**< frame size in transport format */
  uint32_t width;
  uint32_t height;
  bool interlaced;

  /**
   * Per-framebuffer app-format buffers.
   * - TX: source buffers (app writes, then converted to transport on put)
   * - RX: destination buffers (transport converted to app on get)
   * Only allocated when !derive (conversion needed).
   */
  void** app_bufs;
  uint16_t app_bufs_cnt;
};

/*************************************************************************
 * Initialization / Teardown
 *************************************************************************/

/**
 * Initialize format conversion context.
 * Determines if conversion is needed and looks up the converter.
 *
 * @param cvt        Conversion context to initialize.
 * @param config     Video configuration.
 * @param is_tx      true for TX (app→transport), false for RX (transport→app).
 * @return 0 on success, negative errno on failure.
 */
int video_convert_ctx_init(struct video_convert_ctx* cvt,
                           const mtl_video_config_t* config, bool is_tx);

/**
 * Allocate per-framebuffer app-format conversion buffers.
 *
 * @param cvt        Initialized conversion context.
 * @param fb_cnt     Number of framebuffers.
 * @param socket_id  NUMA socket for allocation.
 * @return 0 on success, negative errno on failure.
 */
int video_convert_bufs_alloc(struct video_convert_ctx* cvt, uint16_t fb_cnt,
                             int socket_id);

/**
 * Free per-framebuffer app-format conversion buffers.
 */
void video_convert_bufs_free(struct video_convert_ctx* cvt);

/*************************************************************************
 * Frame Conversion
 *************************************************************************/

/**
 * Perform frame format conversion.
 * Builds st_frame descriptors and calls the cached converter.
 *
 * @param cvt        Conversion context.
 * @param src_data   Source buffer data pointer.
 * @param src_iova   Source buffer IOVA (0 if not applicable).
 * @param src_size   Source buffer size.
 * @param dst_data   Destination buffer data pointer.
 * @param dst_iova   Destination buffer IOVA (0 if not applicable).
 * @param dst_size   Destination buffer size.
 * @param is_tx      true for TX (app→transport), false for RX (transport→app).
 * @return 0 on success, negative errno on failure.
 */
int video_convert_frame(struct video_convert_ctx* cvt, void* src_data,
                        mtl_iova_t src_iova, size_t src_size, void* dst_data,
                        mtl_iova_t dst_iova, size_t dst_size, bool is_tx);

/*************************************************************************
 * Shared Event Poll (identical for TX and RX)
 *************************************************************************/

/**
 * Generic event poll implementation shared by video TX and RX.
 * Dequeues events from the session's event ring with optional timeout.
 */
int video_session_event_poll(struct mtl_session_impl* s, mtl_event_t* event,
                             uint32_t timeout_ms);

/*************************************************************************
 * Shared Stats (identical for TX and RX)
 *************************************************************************/

/**
 * Reset session statistics (shared implementation).
 */
int video_session_stats_reset(struct mtl_session_impl* s);

/*************************************************************************
 * Shared Vsync Callback
 *************************************************************************/

/**
 * Common notify_event callback for vsync events.
 * Used identically by both TX and RX.
 *
 * @param priv  Pointer to mtl_session_impl (set as session wrapper's priv).
 * @param ev    Event type from low-level library.
 * @param args  Event arguments.
 */
int video_session_notify_event(void* priv, enum st_event ev, void* args);

/*************************************************************************
 * Deadline Helpers
 *************************************************************************/

/**
 * Calculate an absolute deadline in nanoseconds from a relative timeout.
 * Returns 0 if timeout_ms == 0 (non-blocking).
 */
static inline uint64_t video_calc_deadline_ns(uint32_t timeout_ms) {
  if (timeout_ms == 0) return 0;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
         (uint64_t)timeout_ms * 1000000ULL;
}

/**
 * Check if the deadline has been reached.
 * Returns true if the current time is past the deadline.
 */
static inline bool video_deadline_reached(uint64_t deadline_ns) {
  if (deadline_ns == 0) return true; /* Non-blocking mode */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  return now >= deadline_ns;
}

#if defined(__cplusplus)
}
#endif

#endif /* _MT_SESSION_VIDEO_COMMON_H_ */
