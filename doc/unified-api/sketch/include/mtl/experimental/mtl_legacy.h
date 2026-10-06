/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_legacy.h - bridge between the unified API and a legacy MTL instance, revision 0.2.
 *
 * While the legacy headers are still installed, an application may run legacy and unified
 * sessions in one process on one instance. This header names the legacy handle only as an
 * opaque type, so it needs no legacy header. It is legacy tier, not part of MTL_1.0: its
 * function has its own version node (MTL_UNIFIED_EXPERIMENTAL_<rev>_BRIDGE, then
 * MTL_LEGACY_BRIDGE from release F), the header moves to mtl/legacy/ at F, and both leave
 * the public set with mtl_init() (migration.md §7.2, §8.3).
 */

#ifndef MTL_EXPERIMENTAL_MTL_LEGACY_H
#define MTL_EXPERIMENTAL_MTL_LEGACY_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct mtl_main_impl; /* the legacy mtl_handle */

/* Wraps a legacy instance; the wrapper's ports are the legacy ports in their order
   (instance port 0 is MTL_PORT_P). mtl_instance_close() on the wrapper closes the unified
   sessions, queues and regions made through it (with the close steps of mtl.h) but never
   stops the legacy instance's devices or its legacy sessions; mtl_uninit() does that, and
   returns -EBUSY while a reference to a wrapper of it is open, until the last one's close
   has returned 0. From MS2a the wrapper is the process's shared instance: another call on the
   same legacy handle returns another reference (-MTL_ENOTSUP, NOT_IMPLEMENTED, before
   MS2a), and mtl_instance_open() with MTL_INSTANCE_SHARED joins it under the rule of
   mtl.h. The wrapper runs on the legacy clock of port P, which its legacy sessions pace
   on; the core and the bindings read that clock (on tasklets, and in CP calls such as a
   start), and a legacy ptp_get_time_fn therefore runs on tasklets, as legacy does, and
   from MS2a on the wrapper's time thread: the one exception to R6 (mtl.h), which ends with
   this header. The call returns after the TSC calibration (about 1 s after mtl_init).
   mtl_time_now() reads PTP_SOURCE_TSC and the default clock directly (MS1, vDSO and
   arithmetic only; UTC read as TAI) and the user and built-in PTP clocks from a snapshot a
   wrapper thread refreshes every 5 ms (MS2a; -MTL_ENOTSUP before), with their flags and
   state (contract.md §2.8). Instance keys marked C are -MTL_EBUSY; from MS2a R keys act on
   the shared engine, legacy sessions included, through the legacy setters, and session
   defaults start from the legacy settings. CP. (MS1) */
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

/* Legacy enum values as unified ones, for the field maps of migration.md §4 (the tables are
   §5). A unified enum is legacy + 1 where 0 must mean "not set" and keeps the legacy values
   where 0 is a real default (D-97), so a port that copies a value, or adds 1, by the wrong rule
   still creates a valid session with another wire (ST20_PACKING_BPM + 1 is MTL_PACKING_GPM):
   convert every legacy enum through these. Each writes *out and returns 0, or -MTL_EINVAL for a
   value outside the legacy enum (its _MAX and above), leaving *out unchanged. AS. */

/* enum st20_fmt to enum mtl_video_format, + 1; ST20_FMT_YUV_422_PLANAR10LE (16) and
   ST20_FMT_V210 (17) to enum mtl_video_format_ext (mtl_format.h), 17 and 18. */
static inline int mtl_video_format_from_legacy(uint32_t st20_fmt, uint32_t* out) {
  if (st20_fmt > 17u) return -MTL_EINVAL; /* ST20_FMT_MAX = 18 */
  *out = st20_fmt + 1u;
  return 0;
}
/* enum st_frame_fmt to enum mtl_app_format (mtl_format.h), + 1; the codestream formats
   ST_FRAME_FMT_JPEGXS_CODESTREAM to ST_FRAME_FMT_H265_CODESTREAM (56-60) to 0: the
   application gives or takes the codestream (cvideo.app_format 0, the codec in cvideo.codec). */
static inline int mtl_app_format_from_legacy(uint32_t st_frame_fmt, uint32_t* out) {
  if (st_frame_fmt <= 15u || (st_frame_fmt >= 32u && st_frame_fmt <= 38u))
    *out = st_frame_fmt + 1u;
  else if (st_frame_fmt >= 56u && st_frame_fmt <= 60u)
    *out = 0;
  else
    return -MTL_EINVAL;
  return 0;
}
/* enum st20_packing to enum mtl_packing: the same values. */
static inline int mtl_packing_from_legacy(uint32_t st20_packing, uint32_t* out) {
  if (st20_packing > 2u) return -MTL_EINVAL; /* ST20_PACKING_MAX = 3 */
  *out = st20_packing;
  return 0;
}
/* enum st21_pacing to enum mtl_sender_type: the same values. */
static inline int mtl_sender_type_from_legacy(uint32_t st21_pacing, uint32_t* out) {
  if (st21_pacing > 2u) return -MTL_EINVAL; /* ST21_PACING_MAX = 3 */
  *out = st21_pacing;
  return 0;
}
/* enum st30_fmt to enum mtl_audio_format, + 1 (ST31_FMT_AM824 is MTL_AM824). */
static inline int mtl_audio_format_from_legacy(uint32_t st30_fmt, uint32_t* out) {
  if (st30_fmt > 3u) return -MTL_EINVAL; /* ST30_FMT_MAX = 4 */
  *out = st30_fmt + 1u;
  return 0;
}
/* enum st30_sampling to audio.sample_rate in Hz. */
static inline int mtl_sample_rate_from_legacy(uint32_t st30_sampling, uint32_t* out) {
  static const uint32_t hz[3] = {48000u, 96000u, 44100u}; /* 48K, 96K, ST31_SAMPLING_44K */
  if (st30_sampling > 2u) return -MTL_EINVAL;             /* ST30_SAMPLING_MAX = 3 */
  *out = hz[st30_sampling];
  return 0;
}
/* enum st30_ptime to enum mtl_ptime, + 1 (the ST31_PTIME_* values included). */
static inline int mtl_ptime_from_legacy(uint32_t st30_ptime, uint32_t* out) {
  if (st30_ptime > 8u) return -MTL_EINVAL; /* ST30_PTIME_MAX = 9 */
  *out = st30_ptime + 1u;
  return 0;
}
/* enum st20_type, st22_type, st30_type, st40_type, st41_type to enum mtl_unit_kind, a table:
   FRAME_LEVEL (0) to MTL_UNIT_FRAME, RTP_LEVEL (1) to MTL_UNIT_PACKETS, ST20_TYPE_SLICE_LEVEL
   (2) to MTL_UNIT_ROWS. */
static inline int mtl_unit_kind_from_legacy(uint32_t st_type, uint32_t* out) {
  static const uint32_t kind[3] = {MTL_UNIT_FRAME, MTL_UNIT_PACKETS, MTL_UNIT_ROWS};
  if (st_type > 2u) return -MTL_EINVAL; /* ST20_TYPE_MAX = 3; the others end at 2 */
  *out = kind[st_type];
  return 0;
}

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_LEGACY_H */
