/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_events.h - events of the unified MTL API, revision 0.2.
 *
 * Events say that something changed (a link, the time base, a session's state, a flow);
 * every state they report also has a getter, so a lost event loses nothing. A session
 * keeps its own events; the instance keeps the port, time, scheduler, health, manager and
 * region events. Both are read here, and waited on with mtl_wait or a queue
 * (MTL_WAIT_EVENTS, mtl.h). Events are coalesced and may overflow (MTL_EVENT_OVERFLOW).
 * Reads return a count >= 1, or -MTL_EAGAIN when there is nothing (mtl.h R2).
 */

#ifndef MTL_EXPERIMENTAL_MTL_EVENTS_H
#define MTL_EXPERIMENTAL_MTL_EVENTS_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Events ------------------------------------------------------------------------- */

/* old -> new and value[] meaning per type (contract.md). S: on the session; I: on the
   instance. */
enum mtl_event_type {
  MTL_EVENT_SESSION_STATE = 1,  /* S; state */
  MTL_EVENT_RECOVERY = 3,       /* S; 0 -> 1 begin, 1 -> 0 end; value[0] units dropped */
  MTL_EVENT_LEG_STATE = 4,      /* S; index = leg; oper; value[0] admin */
  MTL_EVENT_FLOW_STATE = 5,     /* S; index = leg; enum mtl_flow_state; value[0] first index */
  MTL_EVENT_RX_SIGNAL = 6,      /* S; 0/1 packets present */
  MTL_EVENT_RX_FORMAT = 7,      /* S; old -> new format_seq published, new 0 while none is
                                   (status format_reason, MTL_STATUS_RX_DETECTING); values:
                                   stats rx.detected.* */
  MTL_EVENT_RX_TIMEBASE_SUSPECT = 8, /* S; value[0] arrival - media ns; the getter is
                                        MTL_STATUS_TIMEBASE_SUSPECT */
  MTL_EVENT_PACING_CHANGED = 9,      /* S; enum mtl_pacing */
  MTL_EVENT_TIMING_INFEASIBLE = 10,  /* S; value[0] shortfall, value[1] suggested delay */
  MTL_EVENT_BACKPRESSURE = 11,       /* S; enum mtl_blocked_on; NONE -> X begins, X -> NONE
                                        ends */
  MTL_EVENT_TX_UNDERRUN = 12,        /* S; value[0] indices filled by the underrun policy */
  MTL_EVENT_TIME_STATE = 13,         /* I; port; enum mtl_time_state; value[0] offset ns */
  MTL_EVENT_TIME_STEP = 14,          /* I; value[0] step ns */
  MTL_EVENT_PORT_LINK = 15,          /* I; port; 0/1 up; value[0] Mb/s */
  MTL_EVENT_PORT_RESET = 16,         /* I; port */
  MTL_EVENT_SCHED_OVERLOAD = 17, /* I; scheduler; value[0] busy % x 100 */
  MTL_EVENT_MANAGER_LOST = 18,   /* I; MtlManager connection state */
  MTL_EVENT_REGION_RELEASED = 19, /* I; a region whose close returned 1 retired */
  MTL_EVENT_EPOCH_TICK = 20,     /* S; opt-in (option session.epoch_tick); value[0] index */
  MTL_EVENT_CAPTURE_DONE = 21,   /* S; a packet capture finished (mtl_observe.h) */
  MTL_EVENT_OVERFLOW = 22,       /* S, I; value[0] events lost: re-read the getters */
  MTL_EVENT_PORT_REMOVED = 23,   /* I; port; the device is gone or its reset failed
                                    (deployment.md) */
  MTL_EVENT_SCHED_STALLED = 24,  /* I; scheduler; value[0] loop age ns; at 1/8, 1/4, 1/2 of
                                    instance.stall_ns, before the health bit */
  MTL_EVENT_HEALTH = 25,         /* I; old -> new MTL_HEALTH_* flags */
  MTL_EVENT_UPDATE = 26,         /* S; an update left PENDING: new = enum mtl_update_state;
                                    value[0] applied TAI, value[1] update seq (nmos-ipmx.md) */
  MTL_EVENT_GRANDMASTER = 27,    /* I; port; grandmaster changed: value[0] new id, [1] old */
  MTL_EVENT_PORT_ADDRESS = 28,   /* I; port; the granted address changed (DHCP) */
#if defined(MTL_LATER)
  MTL_EVENT_RTCP_INFO = 29,      /* S; the Info Block version changed (TX sent, RX received):
                                    new = version; re-render SDP (mtl_ipmx.h, Phase 7) */
  MTL_EVENT_KEY_NEEDED = 30,     /* S; RX: packets with an unknown key version: value[0] it
                                    (Phase 7) */
#endif
};
#define MTL_EVF_TIME_VALID 0x1u
/* One event record (ev_size versions it). */
struct mtl_event {
  uint32_t type;      /* enum mtl_event_type */
  uint32_t severity;  /* enum mtl_severity (mtl.h): INFO, WARNING or ERR */
  uint32_t reason;    /* mtl_reasons.h, 0 if not applicable */
  uint32_t coalesced; /* occurrences this record stands for, >= 1 */
  uint64_t seq;       /* per reader; a gap means overflow */
  int64_t time_tai_ns;
  struct mtl_object origin;
  char origin_name[MTL_NAME_MAX]; /* readable after the origin retired */
  uint32_t flags;                 /* MTL_EVF_* */
  uint32_t index;                 /* leg or port, else 0 */
  uint32_t old_value;
  uint32_t new_value;
  int64_t value[2];
};
/* The events of a session or of an instance (its own, not its sessions'), each ev_size
   bytes apart. A count >= 1, or -MTL_EAGAIN. The typed wrappers pass the size. WT
   (waiting: mtl.h). (MS3) */
MTL_API_WT(3) int mtl_read_events(struct mtl_object o, struct mtl_event* ev, size_t ev_size,
                                  uint32_t max, int64_t timeout_ns);
static inline int mtl_session_read_events(mtl_session_h s, struct mtl_event* ev, uint32_t max,
                                          int64_t timeout_ns) {
  return mtl_read_events(MTL_OBJ_OF_SESSION(s), ev, sizeof(*ev), max, timeout_ns);
}
static inline int mtl_instance_read_events(mtl_instance_h mt, struct mtl_event* ev,
                                           uint32_t max, int64_t timeout_ns) {
  return mtl_read_events(MTL_OBJ_OF_INSTANCE(mt), ev, sizeof(*ev), max, timeout_ns);
}

#if defined(MTL_LATER)
/* Reserved: opt-in notification on the completing context, for latency parity with
   today's tasklet callbacks. fn runs under the DP rules (no blocking; only DP calls with
   timeout 0 that trylock, and AS calls) and is disabled with MTL_EVENT_OVERFLOW if it
   overruns. CP. (later) */
typedef void (*mtl_inline_fn)(void* user, mtl_session_h s, uint64_t ready_mask);
MTL_API_CP(LATER) int mtl_session_set_inline_notify(mtl_session_h s, mtl_inline_fn fn, void* user);
#endif

MTL_SIZE_CHECK(mtl_event, 144);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_EVENTS_H */
