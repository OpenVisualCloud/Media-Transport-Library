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
 * region events. Both are read here, behind the object's one wait handle
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
  MTL_EVENT_RX_FORMAT = 7,      /* S; new = changed-property mask; values: stats rx.detected.* */
  MTL_EVENT_RX_TIMEBASE_SUSPECT = 8, /* S; value[0] arrival - media ns; the getter is
                                        MTL_STATUS_TIMEBASE_SUSPECT */
  MTL_EVENT_PACING_CHANGED = 9,      /* S; enum mtl_pacing */
  MTL_EVENT_TIMING_INFEASIBLE = 10,  /* S; value[0] shortfall, value[1] suggested delay */
  MTL_EVENT_BACKPRESSURE = 11,       /* S; enum mtl_blocked_on; NONE -> X begins, X -> NONE
                                        ends */
  MTL_EVENT_TX_UNDERRUN = 12,        /* S; value[0] slots filled by the underrun policy */
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
enum mtl_severity {
  MTL_SEV_INFO = 0,
  MTL_SEV_WARNING = 1,
  MTL_SEV_ERROR = 2,
};
#define MTL_EVF_TIME_VALID 0x1u
/* One event record (ev_size versions it). */
struct mtl_event {
  uint32_t type;      /* enum mtl_event_type */
  uint32_t severity;  /* enum mtl_severity */
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
   bytes apart. A count >= 1, or -MTL_EAGAIN. The typed wrappers pass the size. WT. (MS3) */
MTL_API_WT int mtl_read_events(struct mtl_object o, struct mtl_event* ev, size_t ev_size,
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
/* ---- Shared queues (later) -------------------------------------------------------------- */

/* A queue gathers the TX results, RX readiness and events of many sessions, plus port,
   time and instance events, behind one wait handle; results stay lossless and ordered per
   session. */
typedef struct mtl_queue_h {
  uint64_t id;
} mtl_queue_h;
#define MTL_OBJ_OF_QUEUE(q) mtl_obj(MTL_OBJ_QUEUE, 0, (q).id)

/* Instance-wide event subscriptions (mtl_queue_config.subscribe) */
#define MTL_SUB_PORT 0x1u     /* PORT_LINK, PORT_RESET, PORT_REMOVED, PORT_ADDRESS */
#define MTL_SUB_TIME 0x2u     /* TIME_STATE, TIME_STEP, GRANDMASTER */
#define MTL_SUB_INSTANCE 0x4u /* MANAGER_LOST, SCHED_OVERLOAD, SCHED_STALLED, HEALTH,
                                 REGION_RELEASED */
#define MTL_SUB_SESSIONS 0x8u /* SESSION_STATE of every session */
struct mtl_queue_config {
  uint32_t struct_size;
  uint32_t event_capacity; /* records per producer; 0 = 64 */
  uint64_t subscribe;      /* MTL_SUB_* */
  uint64_t reserved[4];
};
/* CP. (later) */
MTL_API_CP int mtl_queue_create(mtl_instance_h mt, const struct mtl_queue_config* c,
                                mtl_queue_h* out);
/* Unbinds every session (their results and events return to them) and retires the
   queue. 0 for a null handle. CP. */
static inline int mtl_queue_close(mtl_queue_h q) {
  return mtl_close(MTL_OBJ_OF_QUEUE(q), 0);
}

/* mtl_queue_bind() parts */
#define MTL_BIND_RESULTS 0x1u /* TX results go to the queue instead of the session */
#define MTL_BIND_READY 0x2u   /* RX readiness is reported by mtl_queue_ready() */
#define MTL_BIND_EVENTS 0x4u  /* a copy of the session's events */
/* CREATED or STOPPED; parts 0 unbinds. CP. (later) */
MTL_API_CP int mtl_queue_bind(mtl_queue_h q, mtl_session_h s, uint64_t parts);

/* TX results of bound sessions (mtl_tx_result.session says whose), per session in
   submission order. A count >= 1, or -MTL_EAGAIN. WT. */
static inline int mtl_queue_reap(mtl_queue_h q, struct mtl_tx_result* r, uint32_t max,
                                 int64_t timeout_ns) {
  return mtl_reap(MTL_OBJ_OF_QUEUE(q), r, sizeof(*r), max, timeout_ns);
}
/* Up to max bound RX sessions with a unit ready to dequeue. A count >= 1, or -MTL_EAGAIN.
   WT. (later) */
MTL_API_WT int mtl_queue_ready(mtl_queue_h q, mtl_session_h* s, uint32_t max,
                               int64_t timeout_ns);
static inline int mtl_queue_read_events(mtl_queue_h q, struct mtl_event* ev, uint32_t max,
                                        int64_t timeout_ns) {
  return mtl_read_events(MTL_OBJ_OF_QUEUE(q), ev, sizeof(*ev), max, timeout_ns);
}

/* Waiting on a queue: the session rules of mtl.h with MTL_WAIT_RESULTS, MTL_WAIT_DEQUEUE
   (some bound RX session is ready) and MTL_WAIT_EVENTS. */
static inline int mtl_queue_wait(mtl_queue_h q, uint64_t mask, int64_t timeout_ns) {
  return mtl_wait(MTL_OBJ_OF_QUEUE(q), mask, timeout_ns);
}
static inline int mtl_queue_get_wait_handle(mtl_queue_h q, uint64_t mask, intptr_t* native) {
  return mtl_get_wait_handle(MTL_OBJ_OF_QUEUE(q), mask, native);
}
/* Waits on this queue only. AS with on = 1, CP with on = 0. */
static inline int mtl_queue_interrupt(mtl_queue_h q, int on) {
  return mtl_interrupt(MTL_OBJ_OF_QUEUE(q), on ? MTL_INTR_ON : MTL_INTR_OFF);
}

/* fn set: a library thread (never a tasklet) reads q and calls fn for each result and
   event. fn may make any call except close, stop or update of a session bound to q,
   mtl_queue_close(q), mtl_queue_dispatch(q, ...), and the instance's close and shutdown
   (-MTL_EDEADLK there). A slow fn delays only this queue. fn NULL stops the thread and
   waits for a running fn, so `user` may be freed after it returns. CP. (later) */
typedef void (*mtl_queue_fn)(void* user, const struct mtl_tx_result* MTL_NULLABLE result,
                             const struct mtl_event* MTL_NULLABLE event);
MTL_API_CP int mtl_queue_dispatch(mtl_queue_h q, mtl_queue_fn MTL_NULLABLE fn,
                                  void* MTL_NULLABLE user);

MTL_SIZE_CHECK(mtl_queue_h, 8);
MTL_SIZE_CHECK(mtl_queue_config, 48);
#endif

#if defined(MTL_LATER)
/* Reserved: opt-in notification on the completing context, for latency parity with
   today's tasklet callbacks. fn runs under the DP rules (no blocking; only DP calls with
   timeout 0 that trylock, and AS calls) and is disabled with MTL_EVENT_OVERFLOW if it
   overruns. CP. (later) */
typedef void (*mtl_inline_fn)(void* user, mtl_session_h s, uint64_t ready_mask);
MTL_API_CP int mtl_session_set_inline_notify(mtl_session_h s, mtl_inline_fn fn, void* user);
#endif

MTL_SIZE_CHECK(mtl_event, 144);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_EVENTS_H */
