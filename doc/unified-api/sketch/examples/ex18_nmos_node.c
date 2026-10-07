/* ex18 — an NMOS node: its IS-04 interfaces from the ports as granted, and one IS-05
   PATCH as one update: destinations and rtp_enabled per leg switch together at one
   instant, all or nothing. master_enable stays the Node's own state. Needs: MS5. */
#include <mtl/experimental/mtl_observe.h>
#include <string.h>

#include "ex_common.h"

void nmos_interface(uint32_t port, const char* name, const uint8_t mac[6],
                    const uint8_t ip[16]);

/* IS-04: the ports as granted (name, MAC, address, a DHCP lease included). */
int inventory(mtl_instance_h mt) {
  for (uint32_t p = 0;; p++) {
    struct mtl_port_spec spec;
    int ret = mtl_port_get_spec(mt, p, &spec, sizeof(spec));
    if (ret == -MTL_EINVAL) return 0; /* no port p: the end of the list */
    if (ret < 0) return ret;
    nmos_interface(p, spec.name, spec.mac, spec.sip);
  }
}

/* IS-05, the parameters:
   - sc: the Node's copy of the session's config (the update reads the members it names);
   - dest[i]: leg i's destination (ip, source_filter and udp_port are read), NULL keeps
   it;
   - legs_disabled: a bit per leg whose rtp_enabled is false;
   - when: the activation time, NULL for activate_immediate;
   - planned_tai_ns and seq: out, the activation_time and the update's number. */
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
