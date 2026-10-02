/* ex13 — NMOS IS-05 on one session. One PATCH is one update: new destinations and
   rtp_enabled per leg switch together at one instant, all or nothing. The call says which
   instant it switches on (the 202 response of a scheduled activation); the status says
   when it did (the 200 response of an immediate one, or /active of a scheduled one).
   master_enable stays the Node's own state: with every leg disabled the session is muted
   but RUNNING, so a disable can be scheduled too. */
#include <string.h>

#include "ex_common.h"

/* dest[i]: the IS-05 destination of leg i (only ip and udp_port are read), or NULL to
   keep it; legs_disabled: the rtp_enabled bits (and every bit when master_enable is
   false); when NULL: activate_immediate. */
int activate(mtl_session_h s, const struct mtl_flow* const dest[MTL_MAX_LEGS],
             uint32_t legs_disabled, const struct mtl_when* when, int64_t* planned_tai_ns,
             uint64_t* seq) {
  struct mtl_session_config sc;
  struct mtl_session_status st;
  MTL_INIT(&sc);
  int ret = mtl_session_get_config(s, &sc); /* the active configuration */
  for (int i = 0; ret >= 0 && i < MTL_MAX_LEGS; i++) {
    if (!dest[i]) continue; /* copy only what IS-05 changed: ssrc, dscp, pt stay */
    memcpy(sc.flows[i].ip, dest[i]->ip, sizeof(dest[i]->ip));
    memcpy(sc.flows[i].source_filter, dest[i]->source_filter,
           sizeof(dest[i]->source_filter));
    sc.flows[i].udp_port = dest[i]->udp_port;
  }
  sc.legs_disabled = legs_disabled;
  /* identical legs are re-applied, as IS-05 asks */
  if (ret >= 0)
    ret = mtl_session_update(s, &sc,
                             MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS | MTL_UPDATE_REAPPLY,
                             when, planned_tai_ns);
  if (ret >= 0) ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret >= 0) *seq = st.update_seq; /* the Node serialises the updates of a session */
  return ret < 0 ? ex_fail("activate", ret) : 0;
}

/* 1: switched at *tai_ns (activation_time); 0: still pending; -MTL_ECANCELED: replaced or
   cancelled; -MTL_EIO: failed (status.update_reason says why). */
int applied_at(mtl_session_h s, uint64_t seq, int64_t* tai_ns) {
  struct mtl_session_status st;
  int ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret < 0) return ret;
  if (st.update_seq != seq || st.update_state == MTL_UPDATE_STATE_REPLACED ||
      st.update_state == MTL_UPDATE_STATE_CANCELLED)
    return -MTL_ECANCELED;
  if (st.update_state == MTL_UPDATE_STATE_FAILED) return -MTL_EIO;
  if (st.update_state != MTL_UPDATE_STATE_APPLIED) return 0;
  *tai_ns = st.update_applied_tai_ns;
  return 1;
}
