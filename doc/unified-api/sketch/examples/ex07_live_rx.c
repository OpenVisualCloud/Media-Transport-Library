/* ex07 — a receiver that knows what it got: each unit, the status and the unit's detail
   say what the stream did, and what the program does about it. Needs: MS3. */
#include <mtl/experimental/mtl_observe.h>

#include "ex_common.h"

void use_frame(const struct mtl_unit* u);
void note(const char* what, int64_t media_index, int64_t n); /* the application's log */

/* base: a video RX config on two legs. A monitor wants the newest frame (RX_LATEST, a
   small pool: a full pool gives up its oldest unread frame); a recorder wants every frame
   (the default, a deeper pool: a full pool drops the newest, and counts it). */
int open_rx(mtl_instance_h mt, const struct mtl_session_config* base, int monitor,
            mtl_session_h* s) {
  struct mtl_session_config sc = *base;
  sc.flags |= monitor ? MTL_SESSION_RX_LATEST : 0;
  sc.pool_count = monitor ? 3 : 8;
  return mtl_session_open(mt, &sc, s);
}

/* No unit: why. The status is a published snapshot: reading it costs nothing. */
static void no_unit(mtl_session_h s) {
  struct mtl_session_status st;
  if (mtl_session_get_status(s, &st, sizeof(st)) < 0) return;
  if (!(st.flags & MTL_STATUS_RX_SIGNAL)) note("no signal", -1, 0);
  for (uint32_t i = 0; i < st.leg_count; i++)
    if (!st.leg[i].oper) note("leg down, or its join failed", -1, st.leg[i].flow_state);
}

/* What one unit says. Indices count fields when interlaced. */
static void check_unit(mtl_session_h s, const struct mtl_unit* u) {
  struct mtl_rx_detail d;
  int64_t k = (u->flags & MTL_UNITF_INDEX_VALID) ? u->media_index : -1;
  if (mtl_rx_get_detail(s, u->lease, &d, sizeof(d)) < 0) return;
  if (u->missed_before) note("we were slow: our full pool dropped", k, u->missed_before);
  if (d.units_missing_before)
    note("never arrived from the network", k, d.units_missing_before);
  /* RELOCKED: the sender restarted, and k may go back */
  if (u->flags & MTL_UNITF_DISCONTINUITY)
    note(u->flags & MTL_UNITF_RELOCKED ? "sender restarted" : "sender jumped", k, 0);
  if (u->flags & MTL_UNITF_USED_REDUNDANCY) note("one leg lost packets", k, 0);
  if (u->status == MTL_RX_INCOMPLETE)
    note("lost on both legs, runs", k, d.missing_ranges);
  if ((d.flags & MTL_RXF_ARRIVAL_LEG0) && (u->flags & MTL_UNITF_TAI_VALID))
    note("latency ns", k, d.arrival_first_tai_ns[0] - u->media_tai_ns);
  if ((d.flags & MTL_RXF_TIMING_LEG0) && d.timing[0].compliance == MTL_NOT_COMPLIANT)
    note(mtl_reason_name(d.timing[0].failed_cause), k, 0); /* with rx.timing_parser */
}

int receive(mtl_session_h s) {
  struct mtl_unit u;
  int ret = 0;
  MTL_INIT(&u);
  while (ret >= 0) {
    ret = mtl_rx_dequeue(s, &u, MTL_MS(100));
    if (ret == -MTL_EAGAIN) { /* nothing for 100 ms */
      no_unit(s);
      ret = 0;
    } else if (ret == -MTL_EIO) { /* ERROR, status.error_reason: restart the handle */
      ret = mtl_session_stop(&s, 1, MTL_STOP_FLUSH, 0);
      if (ret >= 0) ret = mtl_session_start(&s, 1, NULL, NULL); /* -MTL_ENODEV: no port */
    } else if (ret == 0) {
      check_unit(s, &u);
      use_frame(&u);
      ret = mtl_rx_release(s, u.lease);
    }
  }
  if (ret != -MTL_ECANCELED) ex_fail("rx", ret);
  mtl_session_close(s, 0);
  return ret == -MTL_ECANCELED ? 0 : ret;
}
