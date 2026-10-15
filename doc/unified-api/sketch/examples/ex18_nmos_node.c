/* ex18 — an NMOS node: its IS-04 interfaces from the ports as granted, one IS-05 PATCH
   as one update (destinations and rtp_enabled per leg switch together on one unit, all
   or nothing), and a TX format change handed to a second session with no pause.
   master_enable stays the Node's own state. Needs: MS3 (a scheduled `when` and a
   changed rtp_enabled while running: MS5; until then the Node's timer activates at t
   with NULL). */
#include <mtl/experimental/mtl_observe.h>
#include <mtl/experimental/mtl_sync.h>
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
   - when: the activation time, NULL for activate_immediate (an instant: MS5);
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

/* A TX format change (IS-11, a new SDP) with no pause: session b, created early with the
   new format and a's flows and min_tx_delay_ns, starts at t, and a sends only the units
   whose media time is before t. Both sessions need capacity until t (-MTL_ENOSPC: stop,
   MTL_UPDATE_MEDIA and start instead). lead_ns covers b's start; *t_tai_ns is the
   activation_time and the producer's switch: units at or after it go to b. */
int hand_over_begin(mtl_session_h a, mtl_session_h b, int64_t lead_ns,
                    int64_t* t_tai_ns) {
  struct mtl_tx_next n;
  struct mtl_when at = {.kind = MTL_AT_TAI};
  int ret = mtl_tx_get_next(a, &n, sizeof(n));
  if (ret >= 0) {
    at.value = n.next_media_tai_ns + lead_ns;
    ret = mtl_session_start(&b, 1, &at, t_tai_ns); /* b's first index >= t */
  }
  return ret < 0 ? ex_fail("hand over", ret) : 0;
}

/* At once after the producer's last unit before t went to a, and before a would pick up
   a repeat for b's first launch (T0 + b's delay - a's pick-up lead): a sends what it
   holds and repeats nothing more (the REPEAT pins drop when DRAIN begins), then goes. */
int hand_over_end(mtl_session_h* a, mtl_session_h b) {
  int ret = mtl_session_stop(a, 1, MTL_STOP_DRAIN, MTL_SEC(1));
  int closed = mtl_session_close(*a, MTL_MS(200)); /* whatever the drain did: a goes */
  if (ret >= 0) ret = closed;
  *a = b; /* the Node's Sender is b now: new flow id, SDP and IS-04 version */
  return ret < 0 ? ex_fail("hand over end", ret) : 0;
}
