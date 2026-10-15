/* ex21 — a live sink that MTL paces (ffmpeg -re, an OBS output, an application; a
   GStreamer basesink waits on its clock instead, migration.md §12.1): each buffer's
   presentation time becomes its TAI media time, and the sinks of a programme share one
   latency and one phase, so they stay on the same frame times. Needs: MS2 (MS2a:
   discard). */
#include <mtl/experimental/mtl_sync.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

#define MARGIN MTL_MS(1) /* for the producer's jitter */

/* A buffer: planes in the framework's memory, its presentation time on CLOCK_MONOTONIC
   before the programme latency (OBS: its timestamp) and its id. A framework on another
   clock passes that one to mtl_time_convert (FFmpeg: REALTIME, start_time_realtime + the
   rescaled pts). */
struct fw_buffer {
  uint64_t id;
  int64_t pts_monotonic_ns;
  const void* plane[MTL_MAX_PLANES];
  uint32_t stride[MTL_MAX_PLANES];
};
/* The framework's QoS for one buffer: ON_TIME, or DROPPED with SNAP_COLLISION (two
   buffers for one frame time) or TOO_LATE (the latency did not cover the producer);
   margin_ns MTL_TIME_NONE when it has none. */
void fw_report(uint64_t id, uint32_t status, uint32_t reason, int64_t margin_ns);

/* One per programme, in the plugin's process state; every sink adds both to its media
   times. latency_ns: at least the largest minimum of its sinks (sink_open), set before
   the first render (FFmpeg: and at least -muxdelay). phase_ns: the application
   initialises it to MTL_TIME_NONE, and the first sink to render sets it. */
struct programme {
  struct mtl_rational fps; /* the video's raster.fps */
  int64_t latency_ns;
  int64_t phase_ns;
};
struct live_sink {
  mtl_instance_h mt;
  mtl_session_h s;
  struct programme* prog;
  uint64_t unreported; /* buffers handed over whose report has not come back; render and
                          the reap share a thread here (a reaper on another thread makes
                          it atomic) */
  uint64_t pins;       /* the last buffers MTL keeps to repeat: 0 (SKIP); 1, or 2 when
                          interlaced, under MTL_UNDERRUN_REPEAT */
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
  /* MTL's lead with the copy inside submit, the producer's jitter, and one frame for the
     snap's hysteresis: 5/8 frame at the default, one frame at most */
  *min_ns = info.latency_min_ns + MARGIN + mtl_frame_ns(base->video.raster.fps);
  k->unreported = 0;
  k->pins = 0; /* SKIP here; under MTL_UNDERRUN_REPEAT 1, or 2 when interlaced */
  return 0;
}

/* Each buffer's report, by cookie (ex06's results). */
static void on_result(void* priv, const struct mtl_tx_result* r) {
  struct live_sink* k = priv;
  k->unreported--; /* exactly one result per accepted submit */
  fw_report(r->cookie, r->status, r->reason, r->margin_ns);
}

/* render: 0 = handed over (its report follows); -MTL_ECANCELED after unlock. */
int sink_render(struct live_sink* k, const struct fw_buffer* buf) {
  struct programme* p = k->prog;
  struct mtl_unit u;
  int64_t tai = 0;
  int ret;
  MTL_INIT(&u);
  ret = mtl_tx_reap_each(k->s, on_result, k); /* before each submit: no acquire waits on
                                                  results, here or on another thread */
  if (ret >= 0) ret = mtl_tx_acquire(k->s, &u, MTL_FOREVER); /* a full pool waits */
  if (ret < 0) return ret;
  ret = mtl_time_convert(k->mt, buf->pts_monotonic_ns, MTL_CLOCK_MONOTONIC, MTL_CLOCK_TAI,
                         &tai);
  /* the first sink to render sets the phase (a plugin holds the programme's lock) */
  if (ret >= 0 && p->phase_ns == MTL_TIME_NONE)
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
  ret = mtl_tx_submit(k->s, &u); /* on a failure the slot goes back, without a result */
  if (ret == 0) k->unreported++;
  return ret;
}

/* End of input (GStreamer EOS; FFmpeg and OBS close next): the session stays RUNNING, and
   the frame times after the last buffer follow the underrun policy. It returns once every
   buffer but the k->pins last ones has its report, so the last buffer is on air, and a
   seek afterwards needs no start. A reap that times out ends the wait too (a stalled
   queue: close drains it). -MTL_ECANCELED: a flush came first (unlock). */
int sink_end(struct live_sink* k) {
  struct mtl_tx_result r;
  int ret = 0;
  while (k->unreported > k->pins && (ret = mtl_tx_reap(k->s, &r, 1, MTL_SEC(1))) > 0)
    on_result(k, &r);
  return ret < 0 && ret != -MTL_EAGAIN ? ret : 0;
}

/* A seek (GStreamer FLUSH_STOP): the queued buffers are FLUSHED, DISCARD, and a repeated
   frame is released, so the frame times until the next buffer are empty; TAI needs no
   rebase. */
int sink_seek(struct live_sink* k) {
  int ret = mtl_session_discard(k->s, 0, 0);
  int r = mtl_tx_reap_each(k->s, on_result, k);
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
