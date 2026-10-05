/* ex13 — NMOS IS-05 on one session. One PATCH is one update: new destinations and
   rtp_enabled per leg switch together at one instant, all or nothing. The call says which
   instant it switches on (the 202 response of a scheduled activation); the status says
   when it did (the 200 response of an immediate one, or /active of a scheduled one).
   master_enable stays the Node's own state: with every leg disabled the session is muted
   but RUNNING, so a disable can be scheduled too (muting is Phase 7; until then
   master_enable false is a stop). Needs: MS5. */
#include <string.h>

#include "ex_common.h"

/* sc: the Node's copy of the session's configuration (the update reads only the members
   its parts name). dest[i]: the IS-05 destination of leg i (only ip, source_filter and
   udp_port are read), or NULL to keep it; legs_disabled: the rtp_enabled bits (and every
   existing leg's bit when master_enable is false); when NULL: activate_immediate. */
int activate(mtl_session_h s, struct mtl_session_config* sc,
             const struct mtl_flow* const dest[MTL_MAX_LEGS], uint32_t legs_disabled,
             const struct mtl_when* when, int64_t* planned_tai_ns, uint64_t* seq) {
  struct mtl_session_status st;
  for (int i = 0; i < MTL_MAX_LEGS; i++) {
    if (!dest[i]) continue; /* copy only what IS-05 changed: dscp and ttl stay */
    memcpy(sc->flows[i].ip, dest[i]->ip, sizeof(dest[i]->ip));
    memcpy(sc->flows[i].source_filter, dest[i]->source_filter,
           sizeof(dest[i]->source_filter));
    sc->flows[i].udp_port = dest[i]->udp_port;
  }
  sc->legs_disabled = legs_disabled;
  /* -MTL_EBUSY while an earlier activation is pending: the Node answers 423 */
  int ret =
      mtl_session_update(s, sc, MTL_UPDATE_FLOWS | MTL_UPDATE_LEGS, when, planned_tai_ns);
  if (ret >= 0) ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret >= 0) *seq = st.update_seq; /* the Node serialises the updates of a session */
  return ret < 0 ? ex_fail("activate", ret) : 0;
}

/* 1: switched at *tai_ns (activation_time); 0: still pending; -MTL_EIO: failed
   (status.update_reason says why). */
int applied_at(mtl_session_h s, uint64_t seq, int64_t* tai_ns) {
  struct mtl_session_status st;
  int ret = mtl_session_get_status(s, &st, sizeof(st));
  if (ret < 0) return ret;
  if (st.update_seq != seq) return -MTL_EINVAL; /* not the last activation */
  if (st.update_state == MTL_UPDATE_STATE_FAILED) return -MTL_EIO;
  if (st.update_state != MTL_UPDATE_STATE_APPLIED) return 0;
  *tai_ns = st.update_applied_tai_ns;
  return 1;
}
