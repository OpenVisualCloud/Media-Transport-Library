/* ex21 — a live framework sink (GStreamer without basesink sync, ffmpeg -re, an OBS
   output): each buffer's presentation time becomes its TAI media time, and the sinks of a
   programme share one latency and one phase, so they stay on the same frame times.
   Needs: MS2. */
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define MARGIN MTL_MS(1) /* for the producer's jitter */

/* A buffer: planes in the framework's memory, its presentation time on CLOCK_MONOTONIC
   (GStreamer's system clock: base_time + running time) and its id. A framework on another
   clock passes that one to mtl_time_convert. */
struct fw_buffer {
  uint64_t id;
  int64_t pts_monotonic_ns;
  const void* plane[MTL_MAX_PLANES];
  uint32_t stride[MTL_MAX_PLANES];
};
/* The framework's QoS for one buffer: ON_TIME, or DROPPED with SNAP_COLLISION (two
   buffers for one frame time) or TOO_LATE (the latency did not cover the producer);
   margin_ns INT64_MIN when not valid. */
void fw_report(uint64_t id, uint32_t status, uint32_t reason, int64_t margin_ns);

/* One per pipeline or process; every sink adds both to its media times. latency_ns: the
   pipeline latency (GStreamer's LATENCY event), at least the largest minimum its sinks
   reported. phase_ns: set by the first sink to render; -1 before. */
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
   minimum latency, for the latency query. */
int sink_open(struct live_sink* k, const struct mtl_session_config* base,
              int64_t* min_ns) {
  struct mtl_session_config sc = *base;
  struct mtl_session_info info;
  sc.media_mode = MTL_MEDIA_TAI;   /* snapped to the nearest frame time */
  sc.flags |= MTL_SESSION_RESULTS; /* a result per buffer, by cookie */
  int ret = mtl_session_open(k->mt, &sc, &k->s);
  if (ret >= 0) ret = mtl_session_get_info(k->s, &info, sizeof(info));
  if (ret < 0) return ex_fail("sink", ret);
  *min_ns = info.latency_min_ns                     /* MTL's lead */
            + info.convert_ns                       /* the copy inside submit */
            + MARGIN                                /* the producer's jitter */
            + mtl_frame_ns(base->video.raster.fps); /* the phase: less than a frame */
  return 0;
}

/* Each buffer's report, by cookie (ex06's results). */
static void on_result(void* priv, const struct mtl_tx_result* r) {
  (void)priv;
  int64_t margin = (r->flags & MTL_TXR_MARGIN_VALID) ? r->margin_ns : INT64_MIN;
  fw_report(r->cookie, r->status, r->reason, margin);
}

/* render: 0 = handed over (its report follows); -MTL_ECANCELED after unlock. */
int sink_render(struct live_sink* k, const struct fw_buffer* buf) {
  struct programme* p = k->prog;
  struct mtl_unit u;
  int64_t tai = 0;
  int ret;
  MTL_INIT(&u);
  ret = mtl_tx_reap_each(k->s, on_result, NULL); /* then it never waits on results */
  if (ret >= 0) ret = mtl_tx_acquire(k->s, &u, MTL_FOREVER); /* a full pool waits */
  if (ret < 0) return ret;
  ret = mtl_time_convert(k->mt, buf->pts_monotonic_ns, MTL_CLOCK_MONOTONIC, MTL_CLOCK_TAI,
                         &tai);
  /* the first sink to render sets the phase (a plugin holds the programme's lock) */
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
  int r = mtl_tx_reap_each(k->s, on_result, NULL);
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
