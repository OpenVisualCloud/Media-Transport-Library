/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_queue.h - events and shared queues for the unified MTL API, revision 0.2.
 *
 * Events say that something changed (a link, the time base, a session's state, a flow);
 * every state they report also has a getter, so a lost event loses nothing. Each session
 * keeps its own events; read them here. A queue gathers the TX results, RX readiness and
 * events of many sessions, plus port, time and instance events, behind one wait handle:
 * the shape a framework or an operator console needs. Results stay lossless and ordered
 * per session in a queue; events are coalesced and may overflow (MTL_EVENT_OVERFLOW).
 * Reads return a count >= 1, or -MTL_EAGAIN when there is nothing (mtl.h R2).
 */

#ifndef MTL_EXPERIMENTAL_MTL_QUEUE_H
#define MTL_EXPERIMENTAL_MTL_QUEUE_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Events ------------------------------------------------------------------------- */

/* old -> new and value[] meaning per type (contract.md). */
enum mtl_event_type {
  MTL_EVENT_SESSION_STATE = 1,  /* state */
  MTL_EVENT_SESSION_RETIRED = 2, /* a close that returned 1 finished; memory may be freed */
  MTL_EVENT_RECOVERY = 3,       /* 0 -> 1 begin, 1 -> 0 end; value[0] units dropped */
  MTL_EVENT_LEG_STATE = 4,      /* index = leg; oper; value[0] admin */
  MTL_EVENT_FLOW_STATE = 5,     /* index = leg; enum mtl_flow_state; value[0] first index */
  MTL_EVENT_RX_SIGNAL = 6,      /* 0/1 packets present */
  MTL_EVENT_RX_FORMAT = 7,      /* new = changed-property mask; values: stats rx.detected.* */
  MTL_EVENT_RX_TIMEBASE_SUSPECT = 8, /* value[0] arrival - media ns */
  MTL_EVENT_PACING_CHANGED = 9,      /* enum mtl_pacing */
  MTL_EVENT_TIMING_INFEASIBLE = 10,  /* value[0] shortfall, value[1] suggested delay */
  MTL_EVENT_BACKPRESSURE = 11,       /* enum mtl_blocked_on; NONE -> X begins, X -> NONE ends */
  MTL_EVENT_TX_UNDERRUN = 12,        /* value[0] slots filled by the underrun policy */
  MTL_EVENT_TIME_STATE = 13,         /* port; enum mtl_time_state; value[0] offset ns */
  MTL_EVENT_TIME_STEP = 14,          /* value[0] step ns */
  MTL_EVENT_PORT_LINK = 15,          /* port; 0/1 up; value[0] Mb/s */
  MTL_EVENT_PORT_RESET = 16,
  MTL_EVENT_SCHED_OVERLOAD = 17, /* scheduler; value[0] busy % x 100 */
  MTL_EVENT_MANAGER_LOST = 18,   /* MtlManager connection state */
  MTL_EVENT_REGION_RELEASED = 19, /* the region's last reference went */
  MTL_EVENT_EPOCH_TICK = 20,     /* opt-in (option session.epoch_tick); value[0] index */
  MTL_EVENT_CAPTURE_DONE = 21,   /* a packet capture finished (mtl_observe.h) */
  MTL_EVENT_OVERFLOW = 22,       /* value[0] events lost: re-read the getters */
  MTL_EVENT_PORT_REMOVED = 23,   /* port; the device is gone or its reset failed (deployment.md) */
  MTL_EVENT_SCHED_STALLED = 24,  /* scheduler; value[0] loop age ns; at 1/8, 1/4, 1/2 of
                                    instance.stall_ns, before the health bit */
  MTL_EVENT_HEALTH = 25,         /* instance; old -> new MTL_HEALTH_* flags */
  MTL_EVENT_UPDATE = 26,         /* an update left PENDING: new = enum mtl_update_state;
                                    value[0] applied TAI, value[1] update seq (nmos-ipmx.md) */
  MTL_EVENT_GRANDMASTER = 27,    /* port; grandmaster changed: value[0] new id, [1] old */
  MTL_EVENT_PORT_ADDRESS = 28,   /* port; the granted address changed (DHCP) */
  MTL_EVENT_RTCP_INFO = 29,      /* the Info Block version changed (TX sent, RX received):
                                    new = version; re-render SDP (mtl_rtcp.h) */
  MTL_EVENT_KEY_NEEDED = 30,     /* RX: packets with an unknown key version: value[0] it */
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
/* A session's own events. A count >= 1, or -MTL_EAGAIN. WT. */
MTL_API_WT int mtl_session_read_events(mtl_session_h s, struct mtl_event* ev,
                                       size_t ev_size, uint32_t max, int64_t timeout_ns);

/* ---- Shared queues -------------------------------------------------------------------- */

typedef struct mtl_queue_h {
  uint64_t id;
} mtl_queue_h;

/* Instance-wide event subscriptions (mtl_queue_config.subscribe) */
#define MTL_SUB_PORT 0x1u     /* PORT_LINK, PORT_RESET, PORT_REMOVED, PORT_ADDRESS */
#define MTL_SUB_TIME 0x2u     /* TIME_STATE, TIME_STEP, GRANDMASTER */
#define MTL_SUB_INSTANCE 0x4u /* MANAGER_LOST, SCHED_OVERLOAD, SCHED_STALLED, HEALTH,
                                 REGION_RELEASED */
#define MTL_SUB_SESSIONS 0x8u /* SESSION_STATE and SESSION_RETIRED of every session */
struct mtl_queue_config {
  uint32_t struct_size;
  uint32_t event_capacity; /* records per producer; 0 = 64 */
  uint64_t subscribe;      /* MTL_SUB_* */
  uint64_t reserved[4];
};
MTL_API_CP int mtl_queue_create(mtl_instance_h mt, const struct mtl_queue_config* c,
                                mtl_queue_h* out);
/* Unbinds every session (their results and events return to them) and retires the
   queue; always consumes q. 0 for a null handle. CP. */
MTL_API_CP int mtl_queue_close(mtl_queue_h q);

/* mtl_queue_bind() parts */
#define MTL_BIND_RESULTS 0x1u /* TX results go to the queue instead of the session */
#define MTL_BIND_READY 0x2u   /* RX readiness is reported by mtl_queue_ready() */
#define MTL_BIND_EVENTS 0x4u  /* a copy of the session's events */
/* CREATED or STOPPED; parts 0 unbinds. CP. */
MTL_API_CP int mtl_queue_bind(mtl_queue_h q, mtl_session_h s, uint64_t parts);

/* TX results of bound sessions (mtl_tx_result.session says whose), per session in
   submission order. A count >= 1, or -MTL_EAGAIN. WT. */
MTL_API_WT int mtl_queue_reap(mtl_queue_h q, void* rec, size_t rec_size, uint32_t max,
                              int64_t timeout_ns);
/* Up to max bound RX sessions with a unit ready to dequeue. A count >= 1, or -MTL_EAGAIN.
   WT. */
MTL_API_WT int mtl_queue_ready(mtl_queue_h q, mtl_session_h* s, uint32_t max,
                               int64_t timeout_ns);
MTL_API_WT int mtl_queue_read_events(mtl_queue_h q, struct mtl_event* ev, size_t ev_size,
                                     uint32_t max, int64_t timeout_ns);

/* Waiting on a queue: the session rules of mtl.h with MTL_WAIT_RESULTS, MTL_WAIT_DEQUEUE
   (some bound RX session is ready) and MTL_WAIT_EVENTS. */
MTL_API_WT int mtl_queue_wait(mtl_queue_h q, uint64_t mask, int64_t timeout_ns);
MTL_API_CP int mtl_queue_get_wait_handle(mtl_queue_h q, uint64_t mask, intptr_t* native);
MTL_API_AS int mtl_queue_interrupt(mtl_queue_h q, int on);

/* A library thread (never a tasklet, not a busy-loop thread) reads q and calls fn for each
   result and event. fn may make any call except close, stop or update of a session bound
   to q, mtl_queue_close(q), mtl_queue_dispatch_stop(q), and the instance's close and
   shutdown (-MTL_EDEADLK there). A slow fn
   delays only this queue. dispatch_stop waits for a running fn, so `user` may be freed
   after it returns. */
typedef void (*mtl_queue_fn)(void* user, const struct mtl_tx_result* MTL_NULLABLE result,
                             const struct mtl_event* MTL_NULLABLE event);
MTL_API_CP int mtl_queue_dispatch_start(mtl_queue_h q, mtl_queue_fn fn, void* user);
MTL_API_CP int mtl_queue_dispatch_stop(mtl_queue_h q);

#if defined(MTL_LATER)
/* Q-THR-1 (M5): opt-in notification on the completing context, for latency parity with
   today's tasklet callbacks. fn runs under the DP rules (no blocking, no MTL call outside
   the inline-safe subset) and is disabled with MTL_EVENT_OVERFLOW if it overruns. */
typedef void (*mtl_inline_fn)(void* user, mtl_session_h s, uint64_t ready_mask);
MTL_API_CP int mtl_session_set_inline_notify(mtl_session_h s, mtl_inline_fn fn, void* user);
#endif

MTL_SIZE_CHECK(mtl_event, 144);
MTL_SIZE_CHECK(mtl_queue_h, 8);
MTL_SIZE_CHECK(mtl_queue_config, 48);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_QUEUE_H */
