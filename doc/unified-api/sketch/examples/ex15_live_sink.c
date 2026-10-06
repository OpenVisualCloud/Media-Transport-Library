/* ex15 — a live sink (GStreamer sync=TRUE, ffmpeg -re, an OBS output, a playout engine on
   a wall clock): each buffer's presentation time on CLOCK_MONOTONIC becomes its TAI media
   time, and the session declares the producer's latency (media_time_offset_ns), so a
   buffer handed over just in time still makes its frame time. Submit copies the
   framework's planes (MTL_SUBMIT_SRC_PLANES), so the buffer is the framework's again when
   render returns; the result, by cookie, is that buffer's report (QoS). EOS drains; a
   seek discards. Needs: MS3 (media_time_offset_ns; TAI mode and the copy: MS1; discard:
   MS2). */
#include <mtl/experimental/mtl_reasons.h>
#include <mtl/experimental/mtl_sync.h>

#include "ex_common.h"

/* The framework's buffer: planes in its own memory, a presentation time on
   CLOCK_MONOTONIC (GStreamer: base_time + running time + latency; FFmpeg:
   start_time_realtime + pts), and the id its report names. */
struct fw_buffer {
  uint64_t id;
  int64_t pts_monotonic_ns;
  const void* plane[MTL_MAX_PLANES];
  uint32_t stride[MTL_MAX_PLANES];
};
/* The framework's QoS for one buffer (GStreamer: a QoS event when it was dropped). */
void fw_report(uint64_t id, uint32_t status, uint32_t reason, int64_t margin_ns);

struct live_sink {
  mtl_instance_h mt;
  mtl_session_h s;
  struct mtl_rational fps; /* the session's raster.fps */
  int64_t offset_ns;       /* the declared latency, media_time_offset_ns */
  int64_t phase_ns;        /* mtl_grid_offset at the first buffer; -1 before it */
  uint64_t collisions;     /* DROPPED, SNAP_COLLISION: two buffers for one frame time */
  uint64_t late;           /* DROPPED, TOO_LATE: the latency did not cover the producer */
};

/* base: flows, raster, format and app_format from the framework's caps. */
static int sink_create(struct live_sink* k, const struct mtl_session_config* base) {
  struct mtl_session_config sc = *base;
  sc.media_mode = MTL_MEDIA_TAI;   /* snapped to the nearest frame time, the default */
  sc.flags |= MTL_SESSION_RESULTS; /* a result per buffer, by cookie */
  sc.media_time_offset_ns = k->offset_ns;
  return mtl_session_create(k->mt, &sc, &k->s);
}

/* set_caps or write_header. upstream_ns: the latency the framework allows (GStreamer: its
   pipeline latency; FFmpeg: -muxdelay). The declared latency must cover what MTL needs
   before a frame time (latency_min_ns) and the copy inside submit (convert_ns, calibrated
   at create); when upstream_ns is short of it, the CREATED session, which retires at
   once, is created again with enough. *latency_ns: the latency the sink reports. */
int sink_start(struct live_sink* k, mtl_instance_h mt,
               const struct mtl_session_config* base, int64_t upstream_ns,
               int64_t* latency_ns) {
  struct mtl_session_info info;
  k->mt = mt;
  k->fps = base->video.raster.fps;
  k->offset_ns = upstream_ns;
  k->phase_ns = -1;
  k->collisions = k->late = 0;
  int ret = sink_create(k, base);
  if (ret >= 0) ret = mtl_session_get_info(k->s, &info, sizeof(info));
  if (ret >= 0 && info.latency_min_ns + info.convert_ns + MTL_MS(1) > k->offset_ns) {
    mtl_session_close(k->s, 0);
    k->offset_ns = info.latency_min_ns + info.convert_ns + MTL_MS(1);
    ret = sink_create(k, base);
  }
  if (ret >= 0) ret = mtl_session_start(&k->s, 1, NULL, NULL);
  *latency_ns = k->offset_ns;
  return ret < 0 ? ex_fail("sink start", ret) : 0;
}

static int sink_reap(struct live_sink* k) {
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(k->s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++) {
      if (r[i].status == MTL_TX_DROPPED && r[i].reason == MTL_REASON_SNAP_COLLISION)
        k->collisions++;
      if (r[i].status == MTL_TX_DROPPED && r[i].reason == MTL_REASON_TOO_LATE) k->late++;
      fw_report(r[i].cookie, r[i].status, r[i].reason, r[i].margin_ns);
    }
  return n == -MTL_EAGAIN ? 0 : n;
}

/* render, show_frame or write_packet: 0 = handed over (its report follows);
   -MTL_ECANCELED after unlock (the framework's FLUSHING); another negative code on an
   error. */
int sink_render(struct live_sink* k, const struct fw_buffer* buf) {
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
  /* once: the phase that puts the first buffer, after the declared latency, on a frame
     time, so buffers that follow at the frame rate snap to their own frame times
     (timing.md §10.5) */
  if (ret >= 0 && k->phase_ns < 0)
    ret = mtl_grid_offset(tai + k->offset_ns, k->fps, &k->phase_ns);
  if (ret < 0) {
    mtl_tx_release(k->s, u.lease);
    return ret;
  }
  u.media_tai_ns = tai + k->phase_ns; /* MTL adds media_time_offset_ns, then snaps */
  u.flags = MTL_SUBMIT_SRC_PLANES;    /* the framework's planes: submit copies them */
  u.cookie = buf->id;
  for (uint32_t i = 0; i < u.plane_count; i++) {
    u.plane[i].addr = (void*)buf->plane[i];
    u.plane[i].stride = buf->stride[i];
  }
  return mtl_tx_submit(k->s, &u); /* on a failure the slot goes back, without a result */
}

/* EOS: every queued buffer is sent and reported, then the framework posts EOS. A seek
   after it finds the session STOPPED: start it again with the next buffer. */
int sink_eos(struct live_sink* k) {
  int ret = mtl_session_stop(&k->s, 1, MTL_STOP_DRAIN, MTL_SEC(2));
  int r = sink_reap(k);
  return ret < 0 ? ret : r;
}

/* FLUSH_STOP (a seek): queued buffers are FLUSHED (DISCARD) and reported; TAI needs no
   rebase, and the phase stays. */
int sink_flush(struct live_sink* k) {
  int ret = mtl_session_discard(k->s, 0, 0);
  int r = sink_reap(k);
  return ret < 0 ? ret : r;
}

/* unlock (on = 1) and unlock_stop (on = 0), from another thread: an acquire waiting in
   sink_render returns -MTL_ECANCELED at once. */
int sink_unlock(struct live_sink* k, int on) {
  return mtl_session_interrupt(k->s, on);
}

/* stop: sends what is queued, then retires. */
int sink_stop(struct live_sink* k) {
  if (k->collisions || k->late)
    fprintf(stderr, "sink: %llu buffers shared a frame time, %llu came too late\n",
            (unsigned long long)k->collisions, (unsigned long long)k->late);
  int ret = mtl_session_close(k->s, MTL_SEC(1));
  return ret < 0 ? ret : 0; /* MTL_RETIRING: it retires on its own */
}
