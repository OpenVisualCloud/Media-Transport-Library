/* ex06 — a live sender that survives late and missing frames: a camera or capture card
   writes each frame into an acquired slot, and the program reads what happened from the
   results and the events. Needs: MS3. */
#include <mtl/experimental/mtl_events.h>
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_util.h>

#include "ex_common.h"

/* The capture driver writes the next frame into u's planes (its DMA target) and returns 0
   when its readout ends, with its sampling instant on TAI; 1: no frame in time. */
int capture_into(struct mtl_unit* u, int64_t* tai_ns, int64_t timeout_ns);
void capture_drop(void); /* no slot: the driver drops the frame it reads out */
void count(const char* what, int64_t n);
void count_event(uint32_t type, uint32_t reason); /* by type, then by reason */

/* One unit of raster r: a frame, or a field when interlaced. */
static int64_t unit_ns(const struct mtl_raster* r) {
  return mtl_frame_ns(r->fps) / (r->scan == MTL_INTERLACED ? 2 : 1);
}

/* base: flows (two legs), raster, format. TAI mode with the sampling instant, tsmode
   SAMP, and a launch delay of one frame (the readout) plus the pick-up lead MTL reports.
   A late frame is DROPPED (TAI: MTL_LATE_DROP); a frame time with none stays empty. */
int live_open(mtl_instance_h mt, struct mtl_session_config* sc, mtl_session_h* s) {
  int64_t lead = 0;
  sc->media_mode = MTL_MEDIA_TAI;
  sc->tsmode = MTL_TSMODE_SAMP;
  sc->flags |= MTL_SESSION_RESULTS;
  sc->pool_count = 3; /* live: slack for a frame or two, never a deep queue */
  int ret = mtl_session_create(mt, sc, s);
  if (ret >= 0) ret = mtl_stat_get(MTL_OBJ_OF_SESSION(*s), "info.pickup_lead_ns", &lead);
  sc->min_tx_delay_ns =
      unit_ns(&sc->video.raster) + lead; /* the readout, then the lead */
  if (ret >= 0) ret = mtl_session_update(*s, sc, MTL_UPDATE_MEDIA, NULL, NULL);
  if (ret >= 0) ret = mtl_session_start(s, 1, NULL, NULL);
  return ret;
}

/* Each frame's outcome: ON_TIME with its margin, or DROPPED with TOO_LATE,
   SNAP_COLLISION, WAITING_NEIGHBOUR, LINK_DOWN or RECOVERY. */
static void on_result(void* priv, const struct mtl_tx_result* r) {
  (void)priv;
  if (r->status != MTL_TX_ON_TIME)
    count(mtl_reason_name(r->reason), 1);
  else if (r->flags & MTL_TXR_MARGIN_VALID)
    count("margin ns", r->margin_ns);
}

/* Each incident once (LEG_STATE, TX_UNDERRUN, RECOVERY, ...); each also has a getter. A
   delay too short is lengthened on the same handle: stop, update, start. */
static int incidents(mtl_session_h s, struct mtl_session_config* sc) {
  struct mtl_event ev[8];
  struct mtl_session_status st;
  int n, ret = 0;
  while (ret >= 0 && (n = mtl_session_read_events(s, ev, 8, 0)) > 0)
    for (int i = 0; ret >= 0 && i < n; i++) {
      count_event(ev[i].type, ev[i].reason);
      if (ev[i].type != MTL_EVENT_TIMING_INFEASIBLE) continue;
      ret = mtl_session_get_status(s, &st, sizeof(st));
      if (ret >= 0) sc->min_tx_delay_ns = st.suggested_min_tx_delay_ns;
      if (ret >= 0) ret = mtl_session_stop(&s, 1, MTL_STOP_DRAIN, MTL_SEC(1));
      if (ret >= 0) ret = mtl_session_update(s, sc, MTL_UPDATE_MEDIA, NULL, NULL);
      if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL);
    }
  return ret;
}

int live_tx(mtl_instance_h mt, struct mtl_session_config* sc) {
  mtl_session_h s = MTL_NULL(mtl_session_h);
  struct mtl_unit u;
  MTL_INIT(&u);
  int ret = live_open(mt, sc, &s);
  const int64_t unit = unit_ns(&sc->video.raster);
  while (ret >= 0) {
    int64_t tai;
    ret = mtl_tx_reap_each(s, on_result, NULL);
    if (ret >= 0) ret = incidents(s, sc);
    /* a slot within one unit, else that frame is dropped: the camera never waits */
    if (ret >= 0) ret = mtl_tx_acquire(s, &u, unit);
    if (ret == -MTL_EAGAIN) {
      capture_drop();
      ret = 0;
    } else if (ret == 0 && capture_into(&u, &tai, 2 * unit) != 0) {
      ret = mtl_tx_release(s, u.lease); /* the source stalled: its time stays empty */
    } else if (ret == 0) {
      u.media_tai_ns = tai;
      ret = mtl_tx_submit(s, &u);
    }
    if (ret == -MTL_EIO) { /* ERROR: status.error_reason; restart on the same handle */
      ret = mtl_session_stop(&s, 1, MTL_STOP_FLUSH, 0);
      if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL); /* -MTL_ENODEV: no port */
    }
  }
  if (ret != -MTL_ECANCELED) ex_fail("live", ret);
  mtl_session_close(s, MTL_SEC(1));
  return ret == -MTL_ECANCELED ? 0 : ret;
}
