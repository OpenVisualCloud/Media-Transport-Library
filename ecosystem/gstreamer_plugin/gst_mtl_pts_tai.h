/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef __GST_MTL_PTS_TAI_H__
#define __GST_MTL_PTS_TAI_H__

#include <stdint.h>

/* TAI minus pipeline clock time, from one TAI read between two clock reads */
static inline int64_t gst_mtl_clock_to_tai(uint64_t clock_before, uint64_t tai,
                                           uint64_t clock_after) {
  return (int64_t)(tai - (clock_before + (clock_after - clock_before) / 2));
}

/* running_time + base_time is the pipeline clock time of the buffer */
static inline uint64_t gst_mtl_running_time_to_tai(uint64_t running_time,
                                                   uint64_t base_time,
                                                   int64_t clock_to_tai,
                                                   uint32_t pacing_offset) {
  return running_time + base_time + (uint64_t)clock_to_tai + pacing_offset;
}

/* duration of the whole sample frames in bytes of interleaved audio */
static inline uint64_t gst_mtl_audio_bytes_to_ns(uint64_t bytes, uint32_t bpf,
                                                 uint32_t rate) {
  return bytes / bpf * 1000000000ULL / rate;
}

#endif /* __GST_MTL_PTS_TAI_H__ */
