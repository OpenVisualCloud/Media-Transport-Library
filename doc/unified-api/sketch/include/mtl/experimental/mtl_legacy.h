/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_legacy.h - bridge between the unified API and a legacy MTL instance, revision 0.2.
 *
 * While the legacy headers are still installed (until release F+2, migration.md), an application
 * may run legacy and unified sessions in one process on one instance. This header names
 * the legacy handle only as an opaque type, so it needs no legacy header.
 */

#ifndef MTL_EXPERIMENTAL_MTL_LEGACY_H
#define MTL_EXPERIMENTAL_MTL_LEGACY_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct mtl_main_impl; /* the legacy mtl_handle */

/* Wraps a legacy instance. mtl_instance_close() on the wrapper closes the unified sessions
   and regions made through it (with the close steps of mtl.h) but never stops the legacy
   instance's devices or its legacy sessions; mtl_uninit() does that, and returns -EBUSY
   while a wrapper of it is open, until the wrapper's close has returned 0. The wrapper
   runs on the legacy clock of port P, which its legacy sessions pace on; the core and the
   bindings read that clock (on tasklets, and in CP calls such as a start), and a legacy
   ptp_get_time_fn therefore runs on tasklets, as legacy does (the R6 exception). The call
   returns after the TSC calibration (about 1 s after mtl_init). mtl_time_now() reads
   PTP_SOURCE_TSC and the default clock directly (MS1, vDSO and arithmetic only) and the
   user and built-in PTP clocks from a snapshot a wrapper thread refreshes every 5 ms
   (MS2a; -MTL_ENOTSUP before), with their flags and state (contract.md §2.8). Instance
   keys marked C are -MTL_EBUSY; from MS2a R keys act on the
   shared engine, legacy sessions included, through the legacy setters, and session defaults
   start from the legacy settings. CP. (MS1) */
MTL_API_CP(1) int mtl_instance_from_legacy(struct mtl_main_impl* legacy, mtl_instance_h* out);

/* A legacy (enum st_fps, interlaced) pair as raster fps and scan; width and height are
   left as they are. Legacy fps counts fields when interlaced and raster.fps counts frames,
   so interlaced halves it (ST_FPS_P59_94 + interlaced is 30000/1001, 1080i59.94). Returns
   0, or -MTL_EINVAL for a value of ST_FPS_MAX or above and for interlaced ST_FPS_P100,
   P119_88 and P120, whose frame rates exceed 30 (FIELD_RATE, mtl.h). Use it for every
   legacy ops struct with fps (st20, st22, st40, st41 and their pipelines). AS. */
static inline int mtl_raster_from_legacy(uint32_t st_fps, int interlaced,
                                         struct mtl_raster* r) {
  struct mtl_rational f;
  if (st_fps > 10u) return -MTL_EINVAL; /* ST_FPS_P23_98 = 10, ST_FPS_MAX = 11 */
  f = mtl_fps_rational(st_fps + 1u);
  if (interlaced) {
    if (f.num % 2u == 0) f.num /= 2u;
    else f.den *= 2u;
    if (f.num > 30u * f.den) return -MTL_EINVAL;
  }
  r->scan = interlaced ? (uint32_t)MTL_INTERLACED : (uint32_t)MTL_PROGRESSIVE;
  r->fps = f;
  return 0;
}

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_LEGACY_H */
