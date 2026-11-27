/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef __GST_MTL_PTS_TAI_H__
#define __GST_MTL_PTS_TAI_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define GST_MTL_TIME_NONE UINT64_MAX
#define GST_MTL_TAI_RESYNC_NS 1000000000ULL
/* a larger change of the clock to TAI offset is a step of the TAI source */
#define GST_MTL_TAI_STEP_NS 100000LL
#define GST_MTL_TAI_BRACKET_TRIES 5
#define GST_MTL_TAI_BRACKET_NS 10000ULL
/* the onboard PTP is locked after this many syncs in a row under GST_MTL_PTP_LOCK_NS */
#define GST_MTL_PTP_LOCK_SYNCS 5
#define GST_MTL_PTP_LOCK_NS 30000LL

/* clock_to_tai is TAI minus pipeline clock time, valid for one base time */
struct gst_mtl_tai_sync {
  bool valid;
  uint64_t base_time;
  uint64_t measured_at;
  int64_t clock_to_tai;
};

enum gst_mtl_tai_update {
  GST_MTL_TAI_KEPT,
  GST_MTL_TAI_FIRST,
  GST_MTL_TAI_STEP,
  GST_MTL_TAI_REJECTED,
};

struct gst_mtl_ptp_lock {
  uint32_t good_syncs;
  bool locked;
};

/* cuts audio buffers into frames of frame_size bytes; a TAI of 0 is MTL pacing */
struct gst_mtl_audio_framer {
  uint32_t frame_size;
  uint32_t bpf;
  uint32_t rate;
  uint64_t buf_tai;
  uint64_t buf_pos;
  uint32_t fill;
  uint64_t frame_tai;
};

/* TAI minus pipeline clock time, from one TAI read between two clock reads */
static inline int64_t gst_mtl_clock_to_tai(uint64_t clock_before, uint64_t tai,
                                           uint64_t clock_after) {
  return (int64_t)(tai - (clock_before + (clock_after - clock_before) / 2));
}

/* stays locked once locked: the re-measure follows a later step */
static inline bool gst_mtl_ptp_lock_update(struct gst_mtl_ptp_lock* l, int64_t delta) {
  if (!l->locked) {
    l->good_syncs = llabs(delta) < GST_MTL_PTP_LOCK_NS ? l->good_syncs + 1 : 0;
    l->locked = l->good_syncs >= GST_MTL_PTP_LOCK_SYNCS;
  }
  return l->locked;
}

static inline bool gst_mtl_tai_sync_due(const struct gst_mtl_tai_sync* s,
                                        uint64_t base_time, uint64_t clock_now) {
  return !s->valid || base_time != s->base_time ||
         clock_now - s->measured_at >= GST_MTL_TAI_RESYNC_NS;
}

/* keep a change under GST_MTL_TAI_STEP_NS; a bracket over half of it changes nothing */
static inline enum gst_mtl_tai_update gst_mtl_tai_sync_update(struct gst_mtl_tai_sync* s,
                                                              uint64_t base_time,
                                                              uint64_t clock_now,
                                                              int64_t measured,
                                                              uint64_t bracket_ns) {
  enum gst_mtl_tai_update update = GST_MTL_TAI_KEPT;

  if (bracket_ns > GST_MTL_TAI_STEP_NS / 2) return GST_MTL_TAI_REJECTED;

  if (!s->valid || base_time != s->base_time)
    update = GST_MTL_TAI_FIRST;
  else if (llabs(measured - s->clock_to_tai) > GST_MTL_TAI_STEP_NS)
    update = GST_MTL_TAI_STEP;

  if (update != GST_MTL_TAI_KEPT) s->clock_to_tai = measured;
  s->base_time = base_time;
  s->measured_at = clock_now;
  s->valid = true;
  return update;
}

/* sync is NULL while the pipeline clock time is not known */
static inline uint64_t gst_mtl_buffer_tai(const struct gst_mtl_tai_sync* sync,
                                          bool pts_is_tai, uint64_t pts,
                                          uint64_t running_time, uint32_t pacing_offset) {
  if (pts_is_tai) return pts == GST_MTL_TIME_NONE ? 0 : pts + pacing_offset;
  if (!sync || !sync->valid || running_time == GST_MTL_TIME_NONE) return 0;
  return running_time + sync->base_time + (uint64_t)sync->clock_to_tai + pacing_offset;
}

static inline uint64_t gst_mtl_audio_bytes_to_ns(uint64_t bytes, uint32_t bpf,
                                                 uint32_t rate) {
  return bytes / bpf * 1000000000ULL / rate;
}

/* the samples of an open frame run on into this buffer: restamp it from here */
static inline void gst_mtl_audio_framer_buffer(struct gst_mtl_audio_framer* f,
                                               uint64_t buf_tai) {
  uint64_t held = gst_mtl_audio_bytes_to_ns(f->fill, f->bpf, f->rate);

  f->buf_tai = buf_tai;
  f->buf_pos = 0;
  if (f->fill) f->frame_tai = buf_tai > held ? buf_tai - held : 0;
}

/* returns the bytes of the next len bytes of the buffer that go into the open frame */
static inline uint32_t gst_mtl_audio_framer_take(struct gst_mtl_audio_framer* f,
                                                 uint64_t len) {
  uint32_t room = f->frame_size - f->fill;
  uint32_t take = len < room ? (uint32_t)len : room;

  if (!f->fill)
    f->frame_tai =
        f->buf_tai ? f->buf_tai + gst_mtl_audio_bytes_to_ns(f->buf_pos, f->bpf, f->rate)
                   : 0;
  f->fill += take;
  f->buf_pos += take;
  return take;
}

static inline void gst_mtl_audio_framer_reset(struct gst_mtl_audio_framer* f) {
  f->fill = 0;
}

#endif /* __GST_MTL_PTS_TAI_H__ */
