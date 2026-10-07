/* ex21 — a live framework sink (GStreamer without basesink sync, ffmpeg -re, an OBS
   output): each buffer's presentation time becomes its TAI media time, and the sinks of a
   programme share one latency and one phase, so they stay on the same frame times.
   Needs: MS3 (TAI mode and the copy MS1; discard MS2; audio sinks in TAI mode MS6). */
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

/* A buffer: planes in the framework's memory, its presentation time on CLOCK_MONOTONIC
   (GStreamer's system clock: base_time + running time; FFmpeg's start_time_realtime + pts
   is on MTL_CLOCK_REALTIME) and its id. */
struct fw_buffer {
  uint64_t id;
  int64_t pts_monotonic_ns;
  const void* plane[MTL_MAX_PLANES];
  uint32_t stride[MTL_MAX_PLANES];
};
/* The framework's QoS for one buffer: ON_TIME, or DROPPED with SNAP_COLLISION (two
   buffers for one frame time) or TOO_LATE (the latency did not cover the producer). */
void fw_report(uint64_t id, uint32_t status, uint32_t reason, int64_t margin_ns);

/* One per pipeline or process. latency_ns: the pipeline's latency (GStreamer's LATENCY
   event), at least the largest minimum its sinks reported. phase_ns: -1 until the first
   buffer of any sink. Each sink adds both itself (one sink alone could declare the
   latency as sc.media_time_offset_ns, which MTL adds the same way, before snapping). */
struct programme {
  struct mtl_rational fps; /* the video's raster.fps */
  int64_t latency_ns;
  int64_t phase_ns;
};
struct live_sink {
  mtl_instance_h mt;
  mtl_session_h s;
  struct programme* prog;
};

/* set_caps: base has the flows, raster and formats of the caps. *min_ns: this sink's
   minimum latency for the latency query: MTL's lead, the copy inside submit, a margin,
   and one frame for the phase, which adds less than one. */
int sink_open(struct live_sink* k, const struct mtl_session_config* base,
              int64_t* min_ns) {
  struct mtl_session_config sc = *base;
  struct mtl_session_info info;
  sc.media_mode = MTL_MEDIA_TAI;   /* snapped to the nearest frame time */
  sc.flags |= MTL_SESSION_RESULTS; /* a result per buffer, by cookie */
  int ret = mtl_session_open(k->mt, &sc, &k->s);
  if (ret >= 0) ret = mtl_session_get_info(k->s, &info, sizeof(info));
  if (ret >= 0)
    *min_ns = info.latency_min_ns + info.convert_ns + MTL_MS(1) +
              MTL_SEC(1) * base->video.raster.fps.den / base->video.raster.fps.num;
  return ret < 0 ? ex_fail("sink", ret) : 0;
}

static int sink_reap(struct live_sink* k) { /* ex06's reap, reported by cookie */
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(k->s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++)
      fw_report(r[i].cookie, r[i].status, r[i].reason, r[i].margin_ns);
  return n == -MTL_EAGAIN ? 0 : n;
}

/* render: 0 = handed over (its report follows); -MTL_ECANCELED after unlock. */
int sink_render(struct live_sink* k, const struct fw_buffer* buf) {
  struct programme* p = k->prog;
  struct mtl_unit u;
  int64_t tai = 0;
  int ret;
  MTL_INIT(&u);
  do { /* a full pool waits; unread results would block acquire, so reap in between */
    ret = sink_reap(k);
    if (ret >= 0) ret = mtl_tx_acquire(k->s, &u, MTL_MS(20));
  } while (ret == -MTL_EAGAIN && g_running);
  if (ret < 0) return ret;
  ret = mtl_time_convert(k->mt, buf->pts_monotonic_ns, MTL_CLOCK_MONOTONIC, MTL_CLOCK_TAI,
                         &tai);
  /* the first sink to render sets the phase (atomically, in a plugin) */
  if (ret >= 0 && p->phase_ns < 0)
    ret = mtl_grid_offset(tai + p->latency_ns, p->fps, &p->phase_ns);
  if (ret < 0) {
    mtl_tx_release(k->s, u.lease);
    return ret;
  }
  u.media_tai_ns = tai + p->latency_ns + p->phase_ns;
  u.flags = MTL_SUBMIT_SRC_PLANES; /* submit copies the framework's planes */
  u.cookie = buf->id;
  for (uint32_t i = 0; i < u.plane_count; i++) {
    u.plane[i].addr = (void*)buf->plane[i];
    u.plane[i].stride = buf->stride[i];
  }
  return mtl_tx_submit(k->s, &u); /* on a failure the slot goes back, without a result */
}

/* EOS drains and reports every buffer; a seek (FLUSH_STOP) discards them: FLUSHED,
   DISCARD, and TAI needs no rebase. */
int sink_eos(struct live_sink* k, int seek) {
  int ret = seek ? mtl_session_discard(k->s, 0, 0)
                 : mtl_session_stop(&k->s, 1, MTL_STOP_DRAIN, MTL_SEC(2));
  int r = sink_reap(k);
  return ret < 0 ? ret : r;
}

/* unlock and unlock_stop, from another thread: an acquire in sink_render returns at once.
   A sink whose reaper waits on another thread interrupts acquire alone:
   mtl_interrupt(MTL_OBJ_OF_SESSION(s), MTL_INTR_ON, MTL_WAIT_ACQUIRE). */
int sink_unlock(struct live_sink* k, int on) {
  return mtl_session_interrupt(k->s, on);
}

/* stop: sends what is queued, then retires (MTL_RETIRING: it retires on its own). */
int sink_stop(struct live_sink* k) {
  return mtl_session_close(k->s, MTL_SEC(1)) < 0 ? -MTL_EIO : 0;
}
