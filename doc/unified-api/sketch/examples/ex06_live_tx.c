/* ex06 — a live sender that survives late and missing frames: a camera or capture card
   writes each frame into an acquired slot, and the program reads what happened from the
   results and the events. Needs: MS3 (events, the update; the rest MS1). */
#include <mtl/experimental/mtl_events.h>

#include "ex_common.h"

#define FRAME_NS ((int64_t)1001 * 1000000000 / 60000) /* 59.94: 16 683 333 ns */

/* The capture driver writes the next frame into u's planes (its DMA target) and returns 0
   when the frame's readout ends, with its sampling instant on TAI; 1: no frame in time.
 */
int capture_into(struct mtl_unit* u, int64_t* tai_ns, int64_t timeout_ns);
void capture_drop(void); /* the driver has nowhere to write: the frame is dropped */
void count(const char* what, int64_t n);

/* base: flows (two legs), raster, format. TAI mode with the sampling instant, a launch
   delay of one frame (the readout, then the rate-limit pick-up lead), tsmode SAMP. A late
   frame is DROPPED (TAI: MTL_LATE_DROP); a frame time with none stays empty (SKIP). */
void live_config(struct mtl_session_config* sc) {
  sc->media_mode = MTL_MEDIA_TAI;
  sc->min_tx_delay_ns = FRAME_NS + MTL_US(500);
  sc->tsmode = MTL_TSMODE_SAMP;
  sc->flags |= MTL_SESSION_RESULTS;
  sc->pool_count = 3; /* live: slack for a frame or two, never a deep queue */
}

/* Each frame's outcome: ON_TIME (margin_ns), or DROPPED with TOO_LATE, SNAP_COLLISION,
   WAITING_NEIGHBOUR, LINK_DOWN or RECOVERY. */
static int reap(mtl_session_h s) {
  struct mtl_tx_result r[8];
  int n;
  while ((n = mtl_tx_reap(s, r, 8, 0)) > 0)
    for (int i = 0; i < n; i++)
      count(r[i].status == MTL_TX_ON_TIME ? "margin ns" : mtl_reason_name(r[i].reason),
            r[i].status == MTL_TX_ON_TIME ? r[i].margin_ns : 1);
  return n == -MTL_EAGAIN ? 0 : n;
}

/* Each incident once; each also has a getter. A delay too short is lengthened on the same
   handle: stop, the update (MTL_UPDATE_MEDIA takes min_tx_delay_ns), start. */
static int incidents(mtl_session_h s, struct mtl_session_config* sc) {
  struct mtl_event ev[8];
  int n, ret = 0;
  while (ret >= 0 && (n = mtl_session_read_events(s, ev, 8, 0)) > 0)
    for (int i = 0; ret >= 0 && i < n; i++) {
      count(mtl_reason_name(ev[i].reason), 1); /* LEG_STATE, TX_UNDERRUN, RECOVERY, ... */
      if (ev[i].type != MTL_EVENT_TIMING_INFEASIBLE) continue;
      sc->min_tx_delay_ns = ev[i].value[1]; /* status.suggested_min_tx_delay_ns */
      ret = mtl_session_stop(&s, 1, MTL_STOP_DRAIN, MTL_SEC(1));
      if (ret >= 0) ret = mtl_session_update(s, sc, MTL_UPDATE_MEDIA, NULL, NULL);
      if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);
    }
  return ret;
}

int live_tx(mtl_instance_h mt, struct mtl_session_config* sc) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_unit u;
  MTL_INIT(&u);
  live_config(sc);
  int ret = mtl_session_open(mt, sc, &s);
  while (ret >= 0 && g_running) {
    int64_t tai;
    ret = reap(s);
    if (ret >= 0) ret = incidents(s, sc);
    if (ret >= 0) ret = mtl_tx_acquire(s, &u, 0); /* live never waits for MTL */
    if (ret == -MTL_EAGAIN) {
      capture_drop();
      ret = 0;
    } else if (ret == 0 && capture_into(&u, &tai, MTL_MS(40)) != 0) {
      ret = mtl_tx_release(s, u.lease); /* the source stalled: its time stays empty */
    } else if (ret == 0) {
      u.media_tai_ns = tai;
      ret = mtl_tx_submit(s, &u);
    }
    if (ret == -MTL_EIO) { /* ERROR: status.error_reason; restart on the same handle */
      mtl_session_stop(&s, 1, MTL_STOP_FLUSH, 0);
      ret = mtl_session_start(&s, 1, NULL, NULL); /* -MTL_ENODEV: the port is gone */
    }
  }
  if (ret < 0) ex_fail("live", ret);
  mtl_session_close(s, MTL_SEC(1));
  return ret;
}
