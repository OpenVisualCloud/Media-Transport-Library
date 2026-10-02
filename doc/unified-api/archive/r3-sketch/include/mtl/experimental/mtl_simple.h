/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_simple.h - L4 "simple" layer over the unified API, experimental revision 0.1
 * (design sketch; C5 §3.1, response A13). Since 0.1 throughout.
 *
 * Seven calls for a first program. A simple session is started at open, uses a library
 * pool and completion mode NONE, and hides leases, views, submissions, CQs and EQs.
 * Errors are negative MTL_E* codes with detail in mtl_last_error(). A simple handle
 * bridges to the full API through mtl_simple_session().
 *
 * One application thread per simple handle; a frame obtained with *_frame is handed back
 * (*_send or *_done) before the next *_frame call on that handle.
 */

#ifndef MTL_EXPERIMENTAL_MTL_SIMPLE_H
#define MTL_EXPERIMENTAL_MTL_SIMPLE_H

#include "mtl_unified.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct mtl_simple_h {
  uint64_t id;
} mtl_simple_h;
#if !defined(__cplusplus)
#define MTL_SIMPLE_NULL ((mtl_simple_h){0})
#else
#define MTL_SIMPLE_NULL (mtl_simple_h{0})
#endif
#if defined(MTL_UNIFIED_NO_INLINE)
MTL_API_AS int mtl_simple_is_null(mtl_simple_h h);
#else
static inline int mtl_simple_is_null(mtl_simple_h h) {
  return h.id == 0;
}
#endif

/* port_spec: as mtl_instance_open_simple ("0000:af:01.0=192.168.1.10", "null:1"); the
   default instance is acquired with the handle and released by close.
   flow_spec: as mtl_flow_parse ("239.168.85.20:20000,pt=112").
   fps: as mtl_fps_parse ("59.94").
   format: an app format name ("yuv422p10le", "v210", "uyvy"), transported as YUV 4:2:2
   10-bit, or a transport name ("yuv422_10bit"). */
MTL_API_CP int mtl_simple_tx_open(const char* port_spec, const char* flow_spec,
                                  uint32_t w, uint32_t h, const char* fps,
                                  const char* format, mtl_simple_h* out);
/* The next frame to fill: plane 0 address and stride (planes of multi-plane formats are
   contiguous). -MTL_ETIMEDOUT, -MTL_ECANCELED, ... */
MTL_API_WT int mtl_simple_tx_frame(mtl_simple_h s, void** addr, uint32_t* stride,
                                   int64_t timeout_ns);
MTL_API_DPC int mtl_simple_tx_send(mtl_simple_h s);

MTL_API_CP int mtl_simple_rx_open(const char* port_spec, const char* flow_spec,
                                  uint32_t w, uint32_t h, const char* fps,
                                  const char* format, mtl_simple_h* out);
MTL_API_WT int mtl_simple_rx_frame(mtl_simple_h s, const void** addr, uint32_t* stride,
                                   int64_t timeout_ns);
MTL_API_DP int mtl_simple_rx_done(mtl_simple_h s);

/* Stops (DRAIN, 1 s), destroys and releases the instance reference. */
MTL_API_CP int mtl_simple_close(mtl_simple_h s);
/* The full session behind a simple handle (null if invalid). */
MTL_API_DP mtl_session_h mtl_simple_session(mtl_simple_h s);
/* AS: interrupt the handle's waiters (sticky), for signal handlers. */
MTL_API_AS int mtl_simple_interrupt(mtl_simple_h s);

MTL_SIZE_CHECK(mtl_simple_h, 8);

#if defined(__cplusplus)
}
#endif

#endif /* MTL_EXPERIMENTAL_MTL_SIMPLE_H */
