/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_unified.h - the unified MTL API, experimental revision 0.1 (design sketch).
 *
 * STATUS: design sketch for maintainer review (doc/unified-api/, revision 3). Nothing
 * here is implemented. This file is normative for names, types, layouts and call
 * classes: when the prose in the doc/unified-api/ documents disagrees with it, the
 * header wins and the prose is fixed (doc/unified-api/sketch/README.md).
 *
 * doc/unified-api/sketch/check.sh compiles it, the other headers and every example with
 * gcc -std=c99 -Wall -Wextra -Wpadded -Werror (and -pedantic), g++ -std=c++17 -Wall
 * -Wextra -Werror, and the same with clang when present.
 *
 * Conventions (11 §2)
 *
 * C1  Returns. 0 = success, a negative MTL_E* code = failure. Array reads return a
 *     count >= 0. mtl_*_trywait() returns 1 (ready: do not block), 0 (armed: safe to
 *     block on the wait object) or < 0 (error), never -MTL_EAGAIN.
 *     mtl_session_get_state() and mtl_group_get_state() return the state as the
 *     non-negative result.
 * C2  Timeouts are the last argument, int64_t ns: 0 = do not wait,
 *     MTL_TIMEOUT_INFINITE (-1) = forever. Nothing ready with timeout 0: array reads
 *     return 0, single-object verbs return -MTL_EAGAIN. With a timeout > 0 that expires,
 *     both return -MTL_ETIMEDOUT.
 * C3  struct_size. Every top-level struct that crosses the API starts with
 *     uint32_t struct_size, set by the caller with the exported mtl_<struct>_init() (or,
 *     in C only, the MTL_<STRUCT>_INIT(...) value macro). The library never rewrites it.
 *     Input: the library reads min(struct_size, known); fields beyond the caller's size
 *     take their defaults; 0 or less than the 0.1 size is -MTL_EINVAL (NONZERO_TAIL);
 *     non-zero bytes beyond the library's known size are -MTL_EINVAL (NONZERO_TAIL),
 *     the strict rule of 11 §2.3 with its version-check pattern. Every reserved field
 *     must be 0 on input (-MTL_EINVAL).
 *     Output: the library writes min(struct_size, known) bytes.
 *     Sizes are sizeof on the target ABI; this header supports 64-bit targets only.
 * C4  Array reads take the caller's record size (the stride) as an argument and write
 *     min(record_size, native) bytes per record at that pitch. In each written CQ record
 *     hdr.size is an OUTPUT: the bytes written. For EQ records it is mtl_event.size.
 * C5  Zero-default rule: a zero-filled input struct (struct_size set) is the default
 *     configuration; no field has a non-zero default. Fields where 0 means something
 *     other than the number zero are listed in 09's defaults table, checked against this
 *     header in CI. NUMA fields use MTL_NUMA(n); optional filters use flag bits.
 * C6  Enumerated fields are uint32_t; enum types only name the constants. Flag fields
 *     and flag arguments are uint64_t (compact output records use uint32_t). Flags are
 *     plain integer literals. Unknown flag bits are -MTL_EINVAL (UNKNOWN_BITS).
 * C7  Handles are 64-bit values of distinct struct types. id 0 is the null handle of
 *     every type and every call rejects it with -MTL_EBADF. Object ids are
 *     | type:8 | reserved:8 | index:16 | generation:32 |; leases are
 *     | session index:16 | slot:16 | generation:32 |. Generations are seeded randomly per
 *     table (per session for leases) and skip 0, so no live id is 0 and a foreign lease
 *     fails on the session index, deterministically. Handle tables are grow-only chunked
 *     arrays; nothing is sized per instance.
 * C8  Times. Arguments and getters use struct mtl_time (value, clock, flags, accuracy).
 *     Inside CQ records every time is int64_t TAI ns, valid only if its bit is set in the
 *     record's time_valid mask: "every time in a CQ record is TAI or invalid".
 * C9  Versions. "since 0.N" gives the experimental revision that introduced an item. A
 *     section banner, struct or enum carries it; functions and fields inherit the marker
 *     of the nearest enclosing struct or section, and anything added later carries its
 *     own. MTL_UNIFIED_API_VERSION is what this header describes, mtl_version_num() what
 *     the running library implements; both use the MTL_VERSION_NUM(a, b, c) encoding.
 * C10 Call classes (04 §3), enforced in debug builds by a thread-local class set at entry
 *     (allocation, mutexes, sleeps and INFO/WARN logging assert that the class is not DP,
 *     DPC or AS):
 *       MTL_API_CP   control plane: may allocate, lock and block;
 *       MTL_API_DP   data plane: O(1), no allocation, mutex, syscall or logging;
 *       MTL_API_DPC  data plane with caller-context work (conversion, fill, copy);
 *       MTL_API_WT   waits: DP (or DPC) when timeout == 0, enforced dynamically;
 *       MTL_API_AS   async-signal-safe.
 *     From a library busy-loop thread (user tasklet, RX packet lcore) only the
 *     inline-safe subset is legal: mtl_tx_acquire, mtl_rx_dequeue and mtl_tx_reap with
 *     timeout 0, mtl_tx_submit, mtl_tx_release, mtl_rx_release and the DP getters. They
 *     only trylock (-MTL_EAGAIN when contended) and never drain an eventfd. AS calls
 *     also work there; anything else returns -MTL_EDEADLK.
 * C11 MTL_NULLABLE marks a pointer argument that may be NULL. Other pointer arguments
 *     must not be NULL (-MTL_EINVAL).
 */

#ifndef MTL_EXPERIMENTAL_MTL_UNIFIED_H
#define MTL_EXPERIMENTAL_MTL_UNIFIED_H

#include <stddef.h>
#include <stdint.h>

#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFu
#error "mtl_unified.h supports 64-bit targets only (LP64 and LLP64)"
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- 0. Export, call-class and annotation macros -------------------------------- */

/* Binding generators see plain prototypes and every inline helper as an exported twin */
#if defined(SWIG) || defined(__bindgen)
#define MTL_API
#ifndef MTL_UNIFIED_NO_INLINE
#define MTL_UNIFIED_NO_INLINE 1
#endif
#elif defined(_WIN32)
#if defined(MTL_UNIFIED_BUILD)
#define MTL_API __declspec(dllexport)
#else
#define MTL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define MTL_API __attribute__((visibility("default")))
#else
#define MTL_API
#endif

#define MTL_API_CP MTL_API  /* control plane */
#define MTL_API_DP MTL_API  /* data plane, O(1) */
#define MTL_API_DPC MTL_API /* data plane with caller-context work */
#define MTL_API_WT MTL_API  /* wait; DP when timeout == 0 */
#define MTL_API_AS MTL_API  /* async-signal-safe */

#define MTL_NULLABLE /* the pointer may be NULL (C11) */

/* View addresses: a pointer in C and C++, an integer in bindings (11 §3). */
#if defined(SWIG) || defined(__bindgen)
#define MTL_ADDR(T) uintptr_t
#else
#define MTL_ADDR(T) T*
#endif

/* C99 size check (A1): typedef char mtl_sz_<name>[(sizeof(...) == n) ? 1 : -1]; */
#define MTL_SIZE_CHECK(name, n) \
  typedef char mtl_sz_##name[(sizeof(struct name) == (n)) ? 1 : -1]
#define MTL_USIZE_CHECK(name, n) \
  typedef char mtl_sz_##name[(sizeof(union name) == (n)) ? 1 : -1]

/* ---- 1. Versions and limits (since 0.1) ----------------------------------------- */

#ifndef MTL_VERSION_NUM /* identical to include/mtl_api.h */
#define MTL_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
#endif
#define MTL_UNIFIED_API_VERSION MTL_VERSION_NUM(0, 1, 0)

#define MTL_TIMEOUT_INFINITE ((int64_t)-1)
#define MTL_US(x) ((int64_t)(x) * 1000)
#define MTL_MS(x) ((int64_t)(x) * 1000000)
#define MTL_SEC(x) ((int64_t)(x) * 1000000000)

#define MTL_MAX_LEGS 2           /* ST 2022-7 legs in configs; CQ records carry a count */
#define MTL_MAX_PLANES 4         /* planes per buffer */
#define MTL_INSTANCE_MAX_PORTS 8 /* ports in mtl_instance_params */
#define MTL_NAME_MAX 64          /* session, timeline and port names, NUL included */
#define MTL_LCORES_MAX 128       /* lcore list string, NUL included */
#define MTL_ERROR_DETAIL_MAX 128 /* mtl_error_info.detail, NUL included */
#define MTL_CQ_ENTRY_SIZE 256    /* frozen */
#define MTL_CQ_RECORD_MAX 240    /* every CQ record kind; >= 16 B of growth left */
#define MTL_RX_MISSING_RANGES 4  /* missing[] in mtl_rx_unit */
#define MTL_RX_MISSING_EXT 23    /* ranges per MTL_CQE_RX_MISSING follow-on record */
#define MTL_TX_REASON_SLOTS 32   /* frozen slot count of per-reason TX counters */
#define MTL_RX_REJECT_SLOTS 32   /* frozen slot count of per-cause RX rejects */
#define MTL_HIST_BUCKETS 16      /* histogram buckets (log2 unless MTL_OPT_HIST_LINEAR) */
#define MTL_EVENT_PAYLOAD 64     /* bytes of typed payload in mtl_event */

/* NUMA placement: 0 = auto (the socket of the ports), MTL_NUMA(n) = node n (C5). */
#define MTL_NUMA(n) ((uint32_t)(n) + 1u)

/* ---- 2. Error codes (07 §5; since 0.1) ----------------------------------------- */

/* Values equal Linux errno, so on Linux -MTL_EINVAL == -EINVAL. Compare with MTL_E*,
   never with <errno.h>: Windows UCRT has no ESHUTDOWN or ESTALE and numbers EDEADLK,
   ETIMEDOUT, ECANCELED and ENOTSUP differently. Values >= 1000 are reserved for future
   MTL-only codes. Each code has one meaning (07 §5): */
#define MTL_EIO 5         /* the session is in ERROR; reason in get_status() */
#define MTL_EBADF 9       /* null, foreign or destroyed handle */
#define MTL_EAGAIN 11     /* nothing now, and timeout == 0 */
#define MTL_ENOMEM 12     /* allocation failed */
#define MTL_EBUSY 16      /* in use, or not allowed in this state */
#define MTL_EEXIST 17     /* name in use, or named object with another config */
#define MTL_ENODEV 19     /* device gone or reset; resources not re-reservable */
#define MTL_EINVAL 22     /* invalid argument; reason in mtl_last_error() */
#define MTL_ENOSPC 28     /* capacity (queues, lcores, regions, payload size) */
#define MTL_ERANGE 34     /* timing outside the accepted window */
#define MTL_EDEADLK 35    /* not allowed from a library busy-loop thread */
#define MTL_ENOTSUP 95    /* capability absent */
#define MTL_ESHUTDOWN 108 /* app-initiated only: DRAINING, FLUSHING, DESTROYING */
#define MTL_ETIMEDOUT 110 /* the timeout expired */
#define MTL_ESTALE 116    /* a lease of this session whose generation moved on */
#define MTL_ECANCELED 125 /* waiters interrupted; sticky until *_uninterrupt */

/* ---- 3. Handles (03 §2; since 0.1) --------------------------------------------- */

typedef struct mtl_instance_h {
  uint64_t id;
} mtl_instance_h;
typedef struct mtl_session_h {
  uint64_t id;
} mtl_session_h;
/* A buffer's identity (base handle). */
typedef struct mtl_buffer_h {
  uint64_t id;
} mtl_buffer_h;
/* Access to one pool slot, from acquire/dequeue; the only handle access-moving verbs
   (submit, publish, release, transfer) accept. */
typedef struct mtl_lease_h {
  uint64_t id;
} mtl_lease_h;
typedef struct mtl_region_h {
  uint64_t id;
} mtl_region_h;
typedef struct mtl_cq_h {
  uint64_t id;
} mtl_cq_h;
typedef struct mtl_eq_h {
  uint64_t id;
} mtl_eq_h;
typedef struct mtl_timeline_h {
  uint64_t id;
} mtl_timeline_h;
typedef struct mtl_group_h {
  uint64_t id;
} mtl_group_h;

/* Null handles are values in C and C++. In a config struct a null handle means "the
   documented default"; it is never a live object. */
#if !defined(__cplusplus)
#define MTL_INSTANCE_NULL ((mtl_instance_h){0})
#define MTL_SESSION_NULL ((mtl_session_h){0})
#define MTL_BUFFER_NULL ((mtl_buffer_h){0})
#define MTL_LEASE_NULL ((mtl_lease_h){0})
#define MTL_REGION_NULL ((mtl_region_h){0})
#define MTL_CQ_NULL ((mtl_cq_h){0})
#define MTL_EQ_NULL ((mtl_eq_h){0})
#define MTL_TIMELINE_NULL ((mtl_timeline_h){0})
#define MTL_GROUP_NULL ((mtl_group_h){0})
#else
#define MTL_INSTANCE_NULL (mtl_instance_h{0})
#define MTL_SESSION_NULL (mtl_session_h{0})
#define MTL_BUFFER_NULL (mtl_buffer_h{0})
#define MTL_LEASE_NULL (mtl_lease_h{0})
#define MTL_REGION_NULL (mtl_region_h{0})
#define MTL_CQ_NULL (mtl_cq_h{0})
#define MTL_EQ_NULL (mtl_eq_h{0})
#define MTL_TIMELINE_NULL (mtl_timeline_h{0})
#define MTL_GROUP_NULL (mtl_group_h{0})
#endif

/* is_null / eq: static inline; with MTL_UNIFIED_NO_INLINE (bindings, the library's own
   build) the same names are exported functions instead. */
#if defined(MTL_UNIFIED_NO_INLINE)
MTL_API_AS int mtl_instance_is_null(mtl_instance_h h);
MTL_API_AS int mtl_instance_eq(mtl_instance_h a, mtl_instance_h b);
MTL_API_AS int mtl_session_is_null(mtl_session_h h);
MTL_API_AS int mtl_session_eq(mtl_session_h a, mtl_session_h b);
MTL_API_AS int mtl_buffer_is_null(mtl_buffer_h h);
MTL_API_AS int mtl_buffer_eq(mtl_buffer_h a, mtl_buffer_h b);
MTL_API_AS int mtl_lease_is_null(mtl_lease_h h);
MTL_API_AS int mtl_lease_eq(mtl_lease_h a, mtl_lease_h b);
MTL_API_AS int mtl_region_is_null(mtl_region_h h);
MTL_API_AS int mtl_region_eq(mtl_region_h a, mtl_region_h b);
MTL_API_AS int mtl_cq_is_null(mtl_cq_h h);
MTL_API_AS int mtl_cq_eq(mtl_cq_h a, mtl_cq_h b);
MTL_API_AS int mtl_eq_is_null(mtl_eq_h h);
MTL_API_AS int mtl_eq_eq(mtl_eq_h a, mtl_eq_h b);
MTL_API_AS int mtl_timeline_is_null(mtl_timeline_h h);
MTL_API_AS int mtl_timeline_eq(mtl_timeline_h a, mtl_timeline_h b);
MTL_API_AS int mtl_group_is_null(mtl_group_h h);
MTL_API_AS int mtl_group_eq(mtl_group_h a, mtl_group_h b);
#else
static inline int mtl_instance_is_null(mtl_instance_h h) {
  return h.id == 0;
}
static inline int mtl_instance_eq(mtl_instance_h a, mtl_instance_h b) {
  return a.id == b.id;
}
static inline int mtl_session_is_null(mtl_session_h h) {
  return h.id == 0;
}
static inline int mtl_session_eq(mtl_session_h a, mtl_session_h b) {
  return a.id == b.id;
}
static inline int mtl_buffer_is_null(mtl_buffer_h h) {
  return h.id == 0;
}
static inline int mtl_buffer_eq(mtl_buffer_h a, mtl_buffer_h b) {
  return a.id == b.id;
}
static inline int mtl_lease_is_null(mtl_lease_h h) {
  return h.id == 0;
}
static inline int mtl_lease_eq(mtl_lease_h a, mtl_lease_h b) {
  return a.id == b.id;
}
static inline int mtl_region_is_null(mtl_region_h h) {
  return h.id == 0;
}
static inline int mtl_region_eq(mtl_region_h a, mtl_region_h b) {
  return a.id == b.id;
}
static inline int mtl_cq_is_null(mtl_cq_h h) {
  return h.id == 0;
}
static inline int mtl_cq_eq(mtl_cq_h a, mtl_cq_h b) {
  return a.id == b.id;
}
static inline int mtl_eq_is_null(mtl_eq_h h) {
  return h.id == 0;
}
static inline int mtl_eq_eq(mtl_eq_h a, mtl_eq_h b) {
  return a.id == b.id;
}
static inline int mtl_timeline_is_null(mtl_timeline_h h) {
  return h.id == 0;
}
static inline int mtl_timeline_eq(mtl_timeline_h a, mtl_timeline_h b) {
  return a.id == b.id;
}
static inline int mtl_group_is_null(mtl_group_h h) {
  return h.id == 0;
}
static inline int mtl_group_eq(mtl_group_h a, mtl_group_h b) {
  return a.id == b.id;
}
#endif

/* The base buffer behind a lease. Also works for a stale lease of a live session (it
   resolves the slot), so a TX result's hdr.lease maps back to the app's surface. */
MTL_API_DP mtl_buffer_h mtl_lease_buffer(mtl_lease_h lease);
/* Stable slot index of a base buffer for app-side tables; UINT32_MAX if invalid. */
MTL_API_DP uint32_t mtl_buffer_index(mtl_buffer_h b);
/* The pool slot a lease refers to (its slot field); UINT32_MAX for a null lease. */
MTL_API_DP uint32_t mtl_lease_slot(mtl_lease_h lease);

/* A typed reference to an instance, a port, a session or a group, for EQ subscriptions
   and fault injection. Value type. */
enum mtl_object_kind {
  MTL_OBJECT_UNSET = 0,
  MTL_OBJECT_INSTANCE = 1,
  MTL_OBJECT_PORT = 2, /* index = port index, id = the instance */
  MTL_OBJECT_SESSION = 3,
  MTL_OBJECT_GROUP = 4,
};
struct mtl_object {
  uint32_t kind; /* enum mtl_object_kind */
  uint32_t index;
  uint64_t id;
};
#if defined(MTL_UNIFIED_NO_INLINE)
MTL_API_AS struct mtl_object mtl_object_instance(mtl_instance_h mt);
MTL_API_AS struct mtl_object mtl_object_port(mtl_instance_h mt, uint32_t port);
MTL_API_AS struct mtl_object mtl_object_session(mtl_session_h s);
MTL_API_AS struct mtl_object mtl_object_group(mtl_group_h g);
#else
static inline struct mtl_object mtl_object_of_(uint32_t kind, uint32_t index,
                                               uint64_t id) {
  struct mtl_object o;
  o.kind = kind;
  o.index = index;
  o.id = id;
  return o;
}
static inline struct mtl_object mtl_object_instance(mtl_instance_h mt) {
  return mtl_object_of_(MTL_OBJECT_INSTANCE, 0, mt.id);
}
static inline struct mtl_object mtl_object_port(mtl_instance_h mt, uint32_t port) {
  return mtl_object_of_(MTL_OBJECT_PORT, port, mt.id);
}
static inline struct mtl_object mtl_object_session(mtl_session_h s) {
  return mtl_object_of_(MTL_OBJECT_SESSION, 0, s.id);
}
static inline struct mtl_object mtl_object_group(mtl_group_h g) {
  return mtl_object_of_(MTL_OBJECT_GROUP, 0, g.id);
}
#endif

/* ---- 4. Common enumerations (since 0.1) ----------------------------------------- */

enum mtl_dir {
  MTL_DIR_TX = 1,
  MTL_DIR_RX = 2,
};

enum mtl_essence {
  MTL_ESSENCE_UNSET = 0,
  MTL_ESSENCE_VIDEO = 1,    /* ST 2110-20 */
  MTL_ESSENCE_CVIDEO = 2,   /* ST 2110-22 compressed video */
  MTL_ESSENCE_AUDIO = 3,    /* ST 2110-30/31 */
  MTL_ESSENCE_ANC = 4,      /* ST 2110-40 */
  MTL_ESSENCE_FASTMETA = 5, /* ST 2110-41 */
};

/* 03 §3.1 */
enum mtl_session_state {
  MTL_STATE_UNSET = 0, /* never returned; a zeroed record reads UNSET */
  MTL_STATE_CREATED = 1,
  MTL_STATE_ARMED = 2, /* start instant ahead; RX: joined, discarding early units */
  MTL_STATE_RUNNING = 3,
  MTL_STATE_DRAINING = 4, /* stop(MTL_STOP_DRAIN) */
  MTL_STATE_FLUSHING = 5, /* stop(MTL_STOP_FLUSH), or a drain timeout */
  MTL_STATE_STOPPED = 6,
  MTL_STATE_ERROR = 7,      /* only stop/destroy; start re-reserves resources */
  MTL_STATE_DESTROYING = 8, /* deferred destroy in progress */
  MTL_STATE_RETIRED = 9,    /* tombstoned handle until its slot is reused */
};

enum mtl_stop_mode {
  MTL_STOP_DRAIN = 0, /* send every queued unit at its slot */
  MTL_STOP_FLUSH = 1, /* queued units become FLUSHED/STOP_FLUSH */
};
#define MTL_DESTROY_FORCE 0x1u /* mtl_session_destroy(): do not defer */

enum mtl_unit_kind {
  MTL_UNIT_FRAME = 0,        /* frame or field */
  MTL_UNIT_ROWS = 1,         /* progressive rows (06 §9) */
  MTL_UNIT_PACKET_CHUNK = 2, /* reserved (NG2): -MTL_ENOTSUP in 0.1 */
};

enum mtl_scan {
  MTL_SCAN_PROGRESSIVE = 0,
  MTL_SCAN_INTERLACED = 1, /* unit = field; layout rows = height / 2 */
  MTL_SCAN_PSF = 2,        /* paced as interlaced; one RTP and one index per frame */
};

/* 07 §2.3 */
enum mtl_blocked_on {
  MTL_BLOCKED_NONE = 0,
  MTL_BLOCKED_BUFFERS = 1,    /* every slot queued or in flight */
  MTL_BLOCKED_RESULTS = 2,    /* unread-results ring full: read the CQ */
  MTL_BLOCKED_RING = 3,       /* submission hand-off full */
  MTL_BLOCKED_APP_LEASES = 4, /* every slot is held by the app (leaked leases) */
  MTL_BLOCKED_APP_HOLDS = 5,  /* the free slots are excluded by mtl_buffer_hold() */
};

/* Every reason carried by events (mtl_event.reason), mtl_session_get_status().reason and
   .last_error_reason, and mtl_error_info.reason: the table of 07 §5.4. Grouped by
   hundreds so each group can grow; values are frozen once published. */
enum mtl_state_reason {
  MTL_REASON_NONE = 0,
  /* 1-99: lifecycle */
  MTL_REASON_APP_REQUEST = 1,
  MTL_REASON_START_INSTANT = 2,
  MTL_REASON_DRAIN_COMPLETE = 3,
  MTL_REASON_DRAIN_TIMEOUT = 4,
  MTL_REASON_CMD_TIMEOUT = 5, /* ERROR: a command was not acked in cmd_ack_timeout_ns */
  MTL_REASON_FORCED = 6,      /* ERROR: mtl_debug_inject(MTL_FAULT_FORCE_ERROR) */
  MTL_REASON_WRONG_STATE = 7,
  MTL_REASON_GROUP_MEMBER = 8,
  MTL_REASON_DESTROY_IN_PROGRESS = 9,
  /* 100-199: device, port, network */
  MTL_REASON_TX_QUEUE_FATAL = 100,
  MTL_REASON_TX_QUEUE_HANG = 101,
  MTL_REASON_LINK_DOWN = 102,
  MTL_REASON_PORT_RESET = 103,
  MTL_REASON_DEVICE_GONE = 104,
  MTL_REASON_BACKEND_FAILED = 105,
  MTL_REASON_PORT_NOT_OPEN = 106, /* -MTL_ENODEV on a second acquire (A11) */
  MTL_REASON_WAITING_NEIGHBOUR = 107,
  MTL_REASON_JOIN_FAILED = 108,
  MTL_REASON_LEG_DISABLED = 109,
  MTL_REASON_MANAGER_LOST = 110, /* create: -MTL_EAGAIN unless MANAGER_OPTIONAL */
  MTL_REASON_SCHED_OVERLOAD = 111,
  /* 200-299: configuration and arguments */
  MTL_REASON_INVALID_ARGUMENT = 200, /* detail names the field (CP) */
  MTL_REASON_UNKNOWN_BITS = 201,
  MTL_REASON_NONZERO_TAIL = 202, /* bytes beyond the known size; struct_size 0 or small */
  MTL_REASON_WRONG_DIRECTION = 203,
  MTL_REASON_INSTANCE_PARAM_MISMATCH = 204,
  MTL_REASON_COOKIE_WITHOUT_RESULTS = 205,
  MTL_REASON_MEDIA_TIME_BACKWARDS = 206,
  MTL_REASON_PASSTHROUGH_AUTO = 207, /* RTP passthrough with media mode AUTO */
  MTL_REASON_LAYOUT_MISMATCH = 208,
  MTL_REASON_ACCESS_MISMATCH = 209, /* RX needs MTL_MEM_WRITE, TX MTL_MEM_READ */
  MTL_REASON_UNALIGNED = 210,
  MTL_REASON_POOL_INCOMPLETE = 211,
  MTL_REASON_POOL_TOO_SMALL = 212,
  MTL_REASON_RECONFIGURE_INCOMPATIBLE = 213,
  MTL_REASON_NAME_EXISTS = 214,
  MTL_REASON_TIMELINE_CONFIG_MISMATCH = 215,
  MTL_REASON_BUSY_LOOP_THREAD = 216, /* -MTL_EDEADLK */
  MTL_REASON_RX_TIMELINE_LAZY = 217, /* RX on an AT_START or NEXT_GRID timeline */
  MTL_REASON_FIELD_REQUIRED = 218,   /* a required field is zero; detail names it */
  MTL_REASON_STRIDE_MISMATCH =
      219, /* attached buffers of one pool with different strides */
  MTL_REASON_HOLD_MISMATCH = 220, /* a TX plane outside the held RX buffer's planes */
  MTL_REASON_SPAN = 221, /* a buffer outside one library-pool slot or its region */
  MTL_REASON_REGION_NOT_MAPPED =
      222, /* dynamic acquire on a region not mapped to the device */
  MTL_REASON_MIXED_BACKING = 223, /* an import spanning VMAs with different backings */
  MTL_REASON_PORT_CHANGE_NEEDS_RECONFIGURE = 224, /* flow.port changes only in STOPPED */
  MTL_REASON_GRID_MISMATCH =
      225, /* a reconfigured unit period no longer fits the group grid */
  /* 300-399: capacity and memory */
  MTL_REASON_CAPACITY_TX_QUEUES = 300,
  MTL_REASON_CAPACITY_RX_QUEUES = 301,
  MTL_REASON_CAPACITY_RL_QUEUES = 302,
  MTL_REASON_CAPACITY_SCHED_QUOTA = 303,
  MTL_REASON_CAPACITY_LCORES = 304,
  MTL_REASON_CAPACITY_SESSIONS = 305,
  MTL_REASON_REGION_BUDGET = 306,
  MTL_REASON_CODESTREAM_OVERSIZE = 307, /* -MTL_ENOSPC at submit */
  MTL_REASON_HUGEPAGES = 308,
  MTL_REASON_NUMA_MISMATCH = 309,
  MTL_REASON_DIRECT_IMPOSSIBLE = 310,
  MTL_REASON_PACING_UNAVAILABLE = 311,
  MTL_REASON_POOL_COUNT_MAX = 312, /* pool.count above max_count (8 until E11) */
  /* 400-499: timing */
  MTL_REASON_BEYOND_HORIZON = 400,
  MTL_REASON_START_IN_PAST = 401,
  MTL_REASON_LAUNCH_IN_PAST = 402,
  MTL_REASON_TIMING_SHORTFALL = 403,
  MTL_REASON_LINK_OFFSET_BUDGET = 404,
  MTL_REASON_TIME_ESTIMATED = 405,
  MTL_REASON_TIME_STEP = 406,
};

/* ---- 5. Errors and wait objects (07 §5, 04 §5; since 0.1) --------------------- */

struct mtl_error_info {
  uint32_t struct_size;
  int32_t code;    /* negative MTL_E*; 0 = no failure recorded on this thread */
  uint32_t reason; /* enum mtl_state_reason */
  uint32_t reserved0;
  uint64_t call_seq; /* per-thread call count of the failing call */
  /* CP failures only; empty for DP failures */
  char detail[MTL_ERROR_DETAIL_MAX];
};
/* Copies the calling thread's last failure. Every failing call sets it; a successful
   call does not clear it: compare call_seq with mtl_call_seq() taken before the call. */
MTL_API_DP int mtl_last_error(struct mtl_error_info* out);
MTL_API_DP uint64_t mtl_call_seq(void);
/* Static names ("MTL_EINVAL", "UNALIGNED") for logs and tests. */
MTL_API_AS const char* mtl_error_name(int code);
MTL_API_AS const char* mtl_reason_name(uint32_t reason);

enum mtl_wait_kind {
  MTL_WAIT_FD = 1,         /* Linux eventfd: poll for POLLIN; the library drains it */
  MTL_WAIT_WIN_HANDLE = 2, /* Windows auto-reset event HANDLE */
};
/* One ABI on every OS: the native object travels as an intptr_t. */
struct mtl_wait_object {
  intptr_t native; /* int fd or HANDLE */
  uint32_t kind;   /* enum mtl_wait_kind */
  uint32_t reserved;
};

/* Session wait targets (A10): one armed word per target, counting its waiters (04 §5.1).
 */
#define MTL_WAIT_ACQUIRE 0x1u /* TX: a slot can be acquired */
#define MTL_WAIT_DEQUEUE 0x2u /* RX: a unit is READY */
#define MTL_WAIT_RESULTS 0x4u /* TX: a result can be reaped */
#define MTL_WAIT_EVENTS 0x8u  /* the session EQ has events */

/* ---- 6. Time (06 §2; since 0.1) ------------------------------------------------ */

struct mtl_rational {
  uint64_t num;
  uint64_t den;
};

/* MTL_CLOCK_TSC is not public (06 §2). */
enum mtl_clock {
  MTL_CLOCK_UNSET = 0,
  MTL_CLOCK_TAI = 1,
  MTL_CLOCK_MONOTONIC = 2,
  MTL_CLOCK_REALTIME = 3,
  MTL_CLOCK_PHC_RAW = 4,
};
#define MTL_TIME_VALID 0x1u
#define MTL_TIME_ESTIMATED 0x2u /* e.g. SYSTEM_TAI without a daemon-set offset */
#define MTL_TIME_HW 0x4u
#define MTL_TIME_SW 0x8u
#define MTL_TIME_HOLDOVER 0x10u
#define MTL_TIME_ACCURACY_VALID 0x20u

/* Value type for arguments and getters. TAI: ns since 1970-01-01 00:00:00 TAI. */
struct mtl_time {
  int64_t ns;
  uint16_t clock;       /* enum mtl_clock */
  uint16_t flags;       /* MTL_TIME_* */
  uint32_t accuracy_ns; /* with MTL_TIME_ACCURACY_VALID */
};

/* mtl_instance_params.time_source */
enum mtl_time_source {
  MTL_TIME_SOURCE_AUTO = 0, /* builtin PTP if enabled, else PHC, else SYSTEM_TAI */
  MTL_TIME_SOURCE_PTP_BUILTIN = 1,
  MTL_TIME_SOURCE_PHC_EXTERNAL = 2, /* ptp4l/phc2sys discipline the NIC PHC */
  MTL_TIME_SOURCE_CLOCK_TAI = 3,    /* rejected if the kernel TAI offset is 0 */
  MTL_TIME_SOURCE_SYSTEM_TAI = 4,   /* REALTIME + UTC offset, labelled ESTIMATED */
  MTL_TIME_SOURCE_USER = 5,         /* the app feeds mtl_time_user_update() */
  MTL_TIME_SOURCE_TEST = 6,         /* mtl_debug.h mtl_time_test_source() */
};
enum mtl_time_state {
  MTL_TIME_STATE_UNSET = 0,
  MTL_TIME_STATE_FREERUN = 1,
  MTL_TIME_STATE_ACQUIRING = 2,
  MTL_TIME_STATE_LOCKED = 3,
  MTL_TIME_STATE_HOLDOVER = 4,
  MTL_TIME_STATE_LOST = 5,
};
struct mtl_time_status {
  uint32_t struct_size;
  uint32_t source; /* enum mtl_time_source, granted */
  uint32_t state;  /* enum mtl_time_state */
  uint32_t ptp_domain;
  int64_t offset_ns;
  int64_t path_delay_ns;
  int64_t last_sync_age_ns;
  uint8_t grandmaster_id[8]; /* SDP ts-refclk */
  int32_t utc_offset_s;
  uint32_t reserved0;
  uint64_t step_count;
  uint64_t reserved[4];
};

/* DP and wait-free: TAI now from the published time base (not signal-safe). */
MTL_API_DP int mtl_time_now(mtl_instance_h mt, struct mtl_time* now);
MTL_API_DP int mtl_time_get_status(mtl_instance_h mt, uint32_t port,
                                   struct mtl_time_status* st);
MTL_API_DP int mtl_time_convert(mtl_instance_h mt, const struct mtl_time* in,
                                uint32_t to_clock, struct mtl_time* out);
/* One consistent sample of the time base's cross-timestamps, so frameworks can map
   their own clocks (GStreamer pipeline clock, FFmpeg wallclock) to TAI. */
MTL_API_DP int mtl_time_cross_timestamp(mtl_instance_h mt, int64_t* MTL_NULLABLE tai_ns,
                                        int64_t* MTL_NULLABLE monotonic_ns,
                                        int64_t* MTL_NULLABLE realtime_ns);
/* User time source: one (TAI, CLOCK_MONOTONIC) pair from a normal thread. */
MTL_API_CP int mtl_time_user_update(mtl_instance_h mt, int64_t tai_ns,
                                    int64_t monotonic_ns);
/* a - b for two CQ-record times: 0 and *out, or -MTL_EINVAL if a bit of `required` is
   missing from `time_valid`, so an unset field never compares. */
MTL_API_AS int mtl_time_diff_ns(int64_t a, int64_t b, uint32_t time_valid,
                                uint32_t required, int64_t* out);

/* Named frame rates (C5 §2.20): use these, not {60, 1} for 59.94. */
enum mtl_fps {
  MTL_FPS_UNSET = 0,
  MTL_FPS_23_98 = 1, /* 24000/1001 */
  MTL_FPS_24 = 2,
  MTL_FPS_25 = 3,
  MTL_FPS_29_97 = 4, /* 30000/1001 */
  MTL_FPS_30 = 5,
  MTL_FPS_47_95 = 6, /* 48000/1001 */
  MTL_FPS_48 = 7,
  MTL_FPS_50 = 8,
  MTL_FPS_59_94 = 9, /* 60000/1001 */
  MTL_FPS_60 = 10,
  MTL_FPS_100 = 11,
  MTL_FPS_119_88 = 12, /* 120000/1001 */
  MTL_FPS_120 = 13,
};
/* {0, 0} if unknown. */
MTL_API_AS struct mtl_rational mtl_fps_rational(uint32_t fps);
/* "59.94", "60000/1001", "p50", ... to the exact rational; -MTL_EINVAL if unknown. */
MTL_API_AS int mtl_fps_parse(const char* s, struct mtl_rational* out);
#if !defined(__cplusplus)
#define MTL_RATIONAL(n, d) ((struct mtl_rational){(n), (d)})
#endif

/* L4 timeline arithmetic, exact: k = floor((tai_ns - T0) / period).
   mtl_timeline_index_at: T0 from the timeline, the unit period from session `s` (a null
   session uses the timeline's common grid G); -MTL_EAGAIN while T0 is unresolved.
   mtl_rational_index_at: no instance, T0 = the SMPTE epoch, unit_rate e.g. 60000/1001 -
   the cross-process answer of 06 §10 (EPOCH timeline + INDEX mode, k = units since the
   epoch). */
MTL_API_DP int mtl_timeline_index_at(mtl_timeline_h tl, mtl_session_h s, int64_t tai_ns,
                                     int64_t* k);
MTL_API_AS int mtl_rational_index_at(int64_t tai_ns, struct mtl_rational unit_rate,
                                     int64_t* k);

/* ---- 7. Struct kinds, known sizes, versions (11 §2.3; since 0.1) --------------- */

/* One kind per struct_size-bearing struct, plus the CQ and EQ record kinds. */
enum mtl_struct_kind {
  MTL_STRUCT_UNSET = 0,
  MTL_STRUCT_INSTANCE_PARAMS = 1,
  MTL_STRUCT_PORT_PARAMS = 2,
  MTL_STRUCT_SESSION_CONFIG = 3,
  MTL_STRUCT_VIDEO_CONFIG = 4,
  MTL_STRUCT_CVIDEO_CONFIG = 5,
  MTL_STRUCT_AUDIO_CONFIG = 6,
  MTL_STRUCT_ANC_CONFIG = 7,
  MTL_STRUCT_FASTMETA_CONFIG = 8,
  MTL_STRUCT_MEM_DESC = 9,
  MTL_STRUCT_BUFFER_DESC = 10,
  MTL_STRUCT_TIMELINE_CONFIG = 11,
  MTL_STRUCT_GROUP_CONFIG = 12,
  MTL_STRUCT_START_PARAMS = 13,
  MTL_STRUCT_DISCARD_PARAMS = 14,
  MTL_STRUCT_ACTIVATION = 15,
  MTL_STRUCT_TX_SUBMISSION = 16,
  MTL_STRUCT_CQ_CONFIG = 17,
  MTL_STRUCT_EQ_CONFIG = 18,
  MTL_STRUCT_BUFFER_VIEW = 19,
  MTL_STRUCT_SLOT_HINT = 20,
  MTL_STRUCT_BUFFER_REQUIREMENTS = 21,
  MTL_STRUCT_SESSION_INFO = 22,
  MTL_STRUCT_SESSION_STATUS = 23,
  MTL_STRUCT_SESSION_STATS = 24,
  MTL_STRUCT_INSTANCE_INFO = 25,
  MTL_STRUCT_INSTANCE_STATUS = 26,
  MTL_STRUCT_PORT_CAPS = 27,
  MTL_STRUCT_PORT_STATUS = 28,
  MTL_STRUCT_PORT_CAPACITY = 29,
  MTL_STRUCT_SCHED_STATUS = 30,
  MTL_STRUCT_TIME_STATUS = 31,
  MTL_STRUCT_MEM_INFO = 32,
  MTL_STRUCT_MEM_STATUS = 33,
  MTL_STRUCT_ERROR_INFO = 34,
  MTL_STRUCT_EVENT = 35,
  MTL_STRUCT_CQ_TX_RESULT = 36,
  MTL_STRUCT_CQ_RX_UNIT = 37,
  MTL_STRUCT_CQ_RX_PROGRESS = 38,
  MTL_STRUCT_CQ_TX_SOURCE_RELEASED = 39, /* hdr-only record */
  MTL_STRUCT_CQ_RX_MISSING = 40,
  MTL_STRUCT_FAULT_PARAMS = 41, /* mtl_debug.h */
  MTL_STRUCT_TIMELINE_INFO = 42,
  MTL_STRUCT_RECONFIGURE_PARAMS = 43,
};
/* The size of `kind` the running library understands (0 = unknown kind). A newer app on
   an older library leaves fields beyond it at zero (11 §2.3). */
MTL_API_AS uint32_t mtl_struct_known_size(uint32_t kind);
/* The unified API version the running library implements. */
MTL_API_AS uint32_t mtl_version_num(void);

/* ---- 8. Instance and ports (03 §7, A11; since 0.1) ------------------------------- */

/* mtl_port_spec.name: a PCI BDF ("0000:af:01.0"), "kernel:<ifname>",
   "native_af_xdp:<ifname>", or "null:<n>" - the null backend (A12), which completes
   units at their scheduled time from the instance clock and needs no NIC, root or
   hugepages. Fixed size; versions with mtl_instance_params. */
struct mtl_port_spec {
  char name[MTL_NAME_MAX];
  uint8_t ip_family;  /* 0 = IPv4 */
  uint8_t prefix_len; /* 0 = 24 */
  uint8_t reserved0[2];
  uint8_t sip[16];     /* source IP; all zero = DHCP on kernel backends */
  uint8_t gateway[16]; /* all zero = none */
  uint32_t tx_queues;  /* 0 = auto; the merge rules of 03 §7 apply */
  uint32_t rx_queues;  /* 0 = auto */
  uint32_t numa;       /* 0 = the device's socket, or MTL_NUMA(n) */
  uint32_t port_flags; /* reserved, must be 0 */
  uint32_t reserved[8];
};

#define MTL_INSTANCE_MANAGER_OPTIONAL 0x1u /* MtlManager loss: fall back to shm */
#define MTL_INSTANCE_TASKLET_THREAD 0x2u   /* schedulers are pthreads (containers) */
#define MTL_INSTANCE_TASKLET_SLEEP 0x4u
#define MTL_INSTANCE_PTP_BUILTIN 0x8u /* run the built-in PTP client */
#define MTL_INSTANCE_HW_TIMESTAMP 0x10u
#define MTL_INSTANCE_BIND_NUMA 0x20u
#define MTL_INSTANCE_RX_SEPARATE_VIDEO_LCORE 0x40u
#define MTL_INSTANCE_TX_VIDEO_MIGRATE 0x80u
#define MTL_INSTANCE_RX_VIDEO_MIGRATE 0x100u
#define MTL_INSTANCE_TASKLET_TIME_MEASURE 0x200u

/* Log levels for mtl_instance_params.log_level; 0 means the default, INFO. The legacy
   enum mtl_log_level numbers DEBUG as 0, so the unified encoding is its own. */
enum mtl_unified_log_level {
  MTL_ULOG_DEFAULT = 0,
  MTL_ULOG_DEBUG = 1,
  MTL_ULOG_INFO = 2,
  MTL_ULOG_NOTICE = 3,
  MTL_ULOG_WARNING = 4,
  MTL_ULOG_ERR = 5,
  MTL_ULOG_CRIT = 6,
};

/* Versioned instance parameters (A11); replaces struct mtl_init_params in every new
   prototype. A second acquire of the default instance is merged by the published merge
   table (03 §7): each field is invariant (a mismatch is -MTL_EINVAL,
   INSTANCE_PARAM_MISMATCH), mergeable, or ignored (reported in
   mtl_instance_info.ignored_fields); zero fields match anything, and set flag bits are
   requests. */
struct mtl_instance_params {
  uint32_t struct_size;
  uint32_t api_version; /* MTL_UNIFIED_API_VERSION written for; 0 = the header's */
  uint32_t port_count;  /* 1..MTL_INSTANCE_MAX_PORTS */
  uint32_t time_source; /* enum mtl_time_source */
  struct mtl_port_spec ports[MTL_INSTANCE_MAX_PORTS];
  char lcores[MTL_LCORES_MAX]; /* "2-5,8"; empty = MtlManager or auto */
  uint64_t flags;              /* MTL_INSTANCE_* */
  uint32_t sched_max;          /* 0 = auto (at most 18) */
  uint32_t sched_quota_mbs;    /* per-scheduler data quota; 0 = default */
  uint32_t ptp_domain;
  uint32_t log_level;  /* enum mtl_unified_log_level; 0 = default (INFO) */
  uint32_t max_queues; /* per port, the default instance's cap; 0 = HW maximum */
  uint32_t reserved0;
  int64_t cmd_ack_timeout_ns; /* 0 = 100 ms (04 §3); expiry: ERROR/CMD_TIMEOUT */
  uint64_t reserved[16];
};
MTL_API_DP void mtl_instance_params_init(struct mtl_instance_params* p);

struct mtl_instance_info {
  uint32_t struct_size;
  uint32_t api_version; /* granted */
  uint32_t lib_version; /* mtl_version_num() */
  uint32_t port_count;
  uint32_t sched_count;
  uint32_t time_source;    /* granted */
  uint64_t flags;          /* effective MTL_INSTANCE_* */
  uint64_t ignored_fields; /* bit per params field ignored on a second acquire */
  uint64_t reserved[4];
};
enum mtl_manager_state {
  MTL_MANAGER_UNSET = 0,
  MTL_MANAGER_NOT_CONFIGURED = 1, /* no MtlManager in use: nothing is logged */
  MTL_MANAGER_CONNECTED = 2,
  MTL_MANAGER_LOST = 3,
  MTL_MANAGER_RECONNECTING = 4,
};
struct mtl_instance_status {
  uint32_t struct_size;
  uint32_t manager;             /* enum mtl_manager_state (cached) */
  uint32_t refcount;            /* mtl_instance_acquire_default() references */
  uint32_t sessions;            /* live unified sessions */
  uint32_t sessions_destroying; /* deferred destroys not yet retired */
  uint32_t regions;
  uint32_t debug_api; /* 1 = built with -Denable_debug_api=true (15 §6) */
  uint32_t reserved0;
  uint64_t reserved[3];
};
/* Per NUMA node. */
struct mtl_mem_status {
  uint32_t struct_size;
  uint32_t numa; /* plain node number */
  uint64_t hugepage_size;
  uint64_t hugepages_total;
  uint64_t hugepages_free;
  uint64_t library_bytes;        /* hugepage bytes MTL holds on this node */
  uint64_t internal_bytes;       /* of which transport-internal frames (CONVERT paths) */
  uint64_t imported_bytes;       /* regions imported on this node */
  uint64_t largest_free_segment; /* largest free contiguous hugepage segment */
  uint32_t regions_used;         /* instance-wide region budget (05 §3.4) */
  uint32_t regions_free;
  uint64_t reserved[4];
};
MTL_API_DP void mtl_instance_info_init(struct mtl_instance_info* p);
MTL_API_DP void mtl_instance_status_init(struct mtl_instance_status* p);
MTL_API_DP void mtl_mem_status_init(struct mtl_mem_status* p);

/* A private instance. */
MTL_API_CP int mtl_instance_open(const struct mtl_instance_params* p,
                                 mtl_instance_h* out);
/* The refcounted process-wide default instance (merge table above). */
MTL_API_CP int mtl_instance_acquire_default(const struct mtl_instance_params* p,
                                            mtl_instance_h* out);
/* Drops one reference. The instance lives until the last DESTROYING session retires;
   process exit without release is supported. */
MTL_API_CP int mtl_instance_release(mtl_instance_h mt);
/* Bridges to and from the legacy mtl_handle (struct mtl_main_impl*, include/mtl_api.h),
   so legacy and unified sessions share one instance (11 §5). */
struct mtl_main_impl;
MTL_API_CP int mtl_instance_from_legacy(struct mtl_main_impl* legacy,
                                        mtl_instance_h* out);
MTL_API_CP int mtl_instance_to_legacy(mtl_instance_h mt, struct mtl_main_impl** out);
/* L4: "0000:af:01.0=192.168.1.10[,0000:af:01.1=192.168.2.10]" or "null:1". */
MTL_API_CP int mtl_instance_open_simple(const char* port_spec, mtl_instance_h* out);
MTL_API_CP int mtl_instance_get_info(mtl_instance_h mt, struct mtl_instance_info* info);
MTL_API_DP int mtl_instance_get_status(mtl_instance_h mt, struct mtl_instance_status* st);
MTL_API_CP int mtl_instance_get_mem_status(mtl_instance_h mt, uint32_t numa,
                                           struct mtl_mem_status* st);
/* Every live session handle, DESTROYING included; *n = total, at most cap written. */
MTL_API_CP int mtl_instance_list_sessions(mtl_instance_h mt, mtl_session_h* handles,
                                          uint32_t cap, uint32_t* n);
/* AS: every waiter of every session, CQ and EQ returns -MTL_ECANCELED (sticky); one
   eventfd write to the waker, which fans out. For SIGINT handlers. */
MTL_API_AS int mtl_instance_interrupt_all(mtl_instance_h mt);
MTL_API_CP int mtl_instance_uninterrupt_all(mtl_instance_h mt);

enum mtl_backend {
  MTL_BACKEND_UNSET = 0,
  MTL_BACKEND_DPDK_PMD = 1,
  MTL_BACKEND_AF_XDP = 2,
  MTL_BACKEND_KERNEL_SOCKET = 3,
  MTL_BACKEND_NULL = 4, /* "null:<n>" */
  MTL_BACKEND_DPDK_AF_XDP = 5,
  MTL_BACKEND_DPDK_AF_PACKET = 6,
};
/* 06 §6 */
enum mtl_pacing_class {
  MTL_PACING_ANY = 0,
  MTL_PACING_HW_RATE = 1,   /* RL shaper */
  MTL_PACING_HW_LAUNCH = 2, /* TSN launch time (E830) */
  MTL_PACING_SW = 3,        /* TSC; TSC_NARROW and PTP ways are SW profiles */
  MTL_PACING_BEST_EFFORT = 4,
};
/* mtl_port_caps.pacing_classes: bit (1 << class) */
#define MTL_PACING_MASK_HW_RATE 0x2u
#define MTL_PACING_MASK_HW_LAUNCH 0x4u
#define MTL_PACING_MASK_SW 0x8u
#define MTL_PACING_MASK_BEST_EFFORT 0x10u
enum mtl_link_source {
  MTL_LINK_SOURCE_UNSET = 0,
  MTL_LINK_SOURCE_LSC = 1,     /* link-state-change interrupt */
  MTL_LINK_SOURCE_POLL = 2,    /* admin thread polls every 100 ms */
  MTL_LINK_SOURCE_NETLINK = 3, /* kernel-socket and AF_XDP backends (09 §7.2) */
};

struct mtl_port_caps {
  uint32_t struct_size;
  uint32_t backend;        /* enum mtl_backend */
  uint32_t pacing_classes; /* MTL_PACING_MASK_* */
  uint32_t numa;           /* plain node number */
  uint32_t tx_multi_seg;   /* direct TX possible */
  uint32_t hw_rx_timestamp;
  uint32_t hw_tx_timestamp;
  uint32_t launch_time_offload;
  uint32_t dma_engines;
  uint32_t header_split;
  uint32_t max_rl_queues;
  uint32_t iova_va; /* IOVA-as-VA mode */
  uint32_t link_speed_mbps;
  uint32_t backend_syscalls_on_tasklet;
  uint32_t max_regions;     /* region budget (DPDK memseg lists), 05 */
  uint32_t link_source;     /* enum mtl_link_source */
  uint64_t page_size;       /* system page size: import alignment of anonymous memory */
  uint64_t hugepage_sizes;  /* bit n: 2^n-byte hugepages are enabled */
  uint32_t max_sessions[8]; /* indexed by enum mtl_essence */
  uint64_t reserved[8];
};
#define MTL_PORT_TV_COUNTERS 0x1u
struct mtl_port_status {
  uint32_t struct_size;
  uint32_t link_up;
  uint32_t speed_mbps;
  uint32_t rl_queues_in_use;
  uint32_t resets;
  uint32_t time_valid; /* MTL_PORT_TV_* */
  uint64_t rx_pkts;    /* cached NIC counters, polled off the tasklet */
  uint64_t tx_pkts;
  uint64_t rx_errors;
  uint64_t tx_errors;
  uint64_t rx_missed;
  int64_t counters_tai_ns; /* when the cached counters were read */
  uint64_t reserved[8];
};
/* Free resources now (C5 §8.11); a reservation object follows in Phase 6. */
struct mtl_port_capacity {
  uint32_t struct_size;
  uint32_t free_tx_queues;
  uint32_t free_rx_queues;
  uint32_t free_rl_queues;
  uint32_t free_lcores;
  uint32_t free_sessions_per_sched;  /* of the least-loaded scheduler */
  uint32_t quota_free_1080p_x100;    /* remaining quota, 1080p-equivalents x 100 */
  uint32_t quota_max_one_sched_x100; /* the largest remaining quota on one scheduler */
  uint64_t reserved[4];
};
struct mtl_sched_status {
  uint32_t struct_size;
  uint32_t index;
  uint32_t lcore;
  uint32_t sessions;
  uint32_t busy_pct_x100;
  uint32_t sleep_ratio_x100;
  uint32_t waker_cpu_pct_x100;
  uint32_t quota_used_1080p_x100;
  int64_t loop_avg_ns;
  int64_t loop_max_ns;
  int64_t tasklet_p9999_ns;
  int64_t tasklet_max_ns;
  uint64_t reserved[4];
};
/* Phase 6 (A11): runtime port open; -MTL_ENOTSUP in 0.1. Until then a second acquire
   whose ports are not open gets -MTL_ENODEV with reason PORT_NOT_OPEN. */
struct mtl_port_params {
  uint32_t struct_size;
  uint32_t reserved0;
  struct mtl_port_spec spec;
  uint32_t reserved1;
  uint64_t reserved[4];
};
MTL_API_DP void mtl_port_caps_init(struct mtl_port_caps* p);
MTL_API_DP void mtl_port_status_init(struct mtl_port_status* p);
MTL_API_DP void mtl_port_capacity_init(struct mtl_port_capacity* p);
MTL_API_DP void mtl_sched_status_init(struct mtl_sched_status* p);
MTL_API_DP void mtl_time_status_init(struct mtl_time_status* p);
MTL_API_DP void mtl_port_params_init(struct mtl_port_params* p);
MTL_API_DP void mtl_error_info_init(struct mtl_error_info* p);

MTL_API_CP int mtl_port_count(mtl_instance_h mt, uint32_t* n);
MTL_API_CP int mtl_port_find(mtl_instance_h mt, const char* name_bdf_or_ip,
                             uint32_t* port);
MTL_API_CP int mtl_port_open(mtl_instance_h mt, const struct mtl_port_params* p,
                             uint32_t* port);
MTL_API_CP int mtl_port_get_caps(mtl_instance_h mt, uint32_t port,
                                 struct mtl_port_caps* caps);
MTL_API_DP int mtl_port_get_status(mtl_instance_h mt, uint32_t port,
                                   struct mtl_port_status* st);
MTL_API_CP int mtl_port_get_capacity(mtl_instance_h mt, uint32_t port,
                                     struct mtl_port_capacity* cap);
MTL_API_DP int mtl_sched_get_status(mtl_instance_h mt, uint32_t sched,
                                    struct mtl_sched_status* st);

/* ---- 9. Memory, regions and buffers (05; since 0.1) ------------------------------ */

enum mtl_mem_domain {
  MTL_MEM_HOST = 0, /* default; GPU pinned host memory included */
  MTL_MEM_HOST_HUGEPAGE = 1,
  MTL_MEM_DMABUF = 2,        /* later */
  MTL_MEM_DEVICE = 3,        /* later */
  MTL_MEM_DEVICE_SHARED = 4, /* later; COPY only */
};
/* mtl_mem_desc.access: 0 = READ | WRITE; attach enforces it (RX writes, TX reads) */
#define MTL_MEM_READ 0x1u
#define MTL_MEM_WRITE 0x2u
/* mtl_mem_desc.flags */
#define MTL_MEM_MAP_REQUIRED 0x1u  /* fail if a listed device cannot map it */
#define MTL_MEM_NUMA_REQUIRED 0x2u /* fail if the pages are not on `numa` */

/* Import rules (05 §3): va and length page aligned (hugepage aligned for hugetlbfs),
   else -MTL_EINVAL/UNALIGNED; device_mask 0 maps lazily on the first attach to an
   unmapped device (a CP step); imported memory stays mapped until mtl_mem_destroy()
   returns 0. */
struct mtl_mem_desc {
  uint32_t struct_size;
  uint32_t domain; /* enum mtl_mem_domain */
  void* va;        /* import: start; alloc: must be NULL */
  uint64_t length;
  uint32_t numa;        /* alloc: placement; import: check (MTL_MEM_NUMA_REQUIRED) */
  uint32_t access;      /* MTL_MEM_READ | MTL_MEM_WRITE; 0 = both */
  uint64_t device_mask; /* bit per port; 0 = lazy */
  uint64_t flags;       /* MTL_MEM_MAP_REQUIRED | MTL_MEM_NUMA_REQUIRED */
  uint64_t user_cookie;
  uint64_t reserved[4];
};
enum mtl_backing {
  MTL_BACKING_UNSET = 0,
  MTL_BACKING_EAL_HUGEPAGE = 1, /* mtl_mem_alloc(), library pools */
  MTL_BACKING_HUGETLBFS = 2,    /* DIRECT */
  MTL_BACKING_ANON = 3,         /* anonymous or pinned host memory: DIRECT */
  MTL_BACKING_SHMEM = 4,        /* tmpfs, memfd: DIRECT */
  MTL_BACKING_FILE = 5,         /* regular-file mapping (page cache): COPY only */
  MTL_BACKING_DEVICE = 6,       /* later */
};
struct mtl_mem_info {
  uint32_t struct_size;
  uint32_t backing; /* enum mtl_backing */
  uint64_t length;
  uint32_t page_size;      /* of the backing */
  uint32_t numa;           /* MTL_NUMA(n) of the pages; 0 = mixed or unknown */
  uint32_t numa_mismatch;  /* bit p: port p is on another socket */
  uint32_t direct_capable; /* 0 = COPY only */
  uint64_t mapped_devices; /* bit p: port p; bit 32 + d: DMA engine d */
  uint32_t buffers;        /* buffers that reference the region */
  uint32_t refs;           /* in-flight references */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_mem_desc_init(struct mtl_mem_desc* p);
MTL_API_DP void mtl_mem_info_init(struct mtl_mem_info* p);

MTL_API_CP int mtl_mem_alloc(mtl_instance_h mt, const struct mtl_mem_desc* d,
                             mtl_region_h* out);
MTL_API_CP int mtl_mem_import(mtl_instance_h mt, const struct mtl_mem_desc* d,
                              mtl_region_h* out);
/* -MTL_EBUSY while a buffer, attach, in-flight unit or pending DMA references it. */
MTL_API_CP int mtl_mem_destroy(mtl_region_h r);
MTL_API_CP int mtl_mem_get_info(mtl_region_h r, struct mtl_mem_info* info);
/* Explicit mapping; the DMA engines of that port's sessions follow. */
MTL_API_CP int mtl_mem_map_device(mtl_region_h r, uint32_t port);

/* Fixed size; versions with mtl_buffer_desc. */
struct mtl_plane_desc {
  mtl_region_h region;
  uint64_t offset;    /* from the region start; byte granular */
  uint64_t span;      /* bytes MTL may touch; 0 = stride * (rows - 1) + row_bytes */
  uint32_t row_bytes; /* valid bytes per row */
  uint32_t stride;    /* 0 = row_bytes; any stride >= row_bytes is DIRECT-capable */
  uint32_t rows;
  uint32_t reserved[3];
};
struct mtl_buffer_desc {
  uint32_t struct_size;
  uint32_t plane_count; /* 1..MTL_MAX_PLANES */
  struct mtl_plane_desc plane[MTL_MAX_PLANES];
  mtl_region_h meta_region; /* null = the library meta area of the slot */
  uint64_t meta_offset;
  uint32_t meta_capacity; /* 0 with a null meta_region */
  uint32_t reserved0;
  uint64_t user_cookie; /* stable per-buffer identity */
  uint64_t reserved[2];
};
/* Fixed size. */
struct mtl_plane_view {
  MTL_ADDR(void) addr; /* NULL when the domain has no CPU mapping */
  uint32_t stride;
  uint32_t row_bytes;
  uint32_t rows;
  uint32_t reserved;
};
/* Layouts are immutable: a view is needed once per slot. Cache it by
   mtl_buffer_index(mtl_lease_buffer(lease)) and pass NULL to acquire/dequeue. */
struct mtl_buffer_view {
  uint32_t struct_size;
  uint32_t plane_count;
  struct mtl_plane_view plane[MTL_MAX_PLANES];
  uint32_t domain;
  uint32_t meta_bytes; /* RX: bytes valid in the meta area */
  MTL_ADDR(void) meta;
  uint64_t device_handle; /* when addr is NULL: dma-buf fd, device pointer */
  uint64_t reserved[2];
};
MTL_API_DP void mtl_buffer_desc_init(struct mtl_buffer_desc* p);
MTL_API_DP void mtl_buffer_view_init(struct mtl_buffer_view* p);

MTL_API_CP int mtl_buffer_create(const struct mtl_buffer_desc* d, mtl_buffer_h* out);
/* -MTL_EBUSY while leased, held, or attached to a session that has not retired. */
MTL_API_CP int mtl_buffer_destroy(mtl_buffer_h b);
MTL_API_DP int mtl_buffer_get_view(mtl_buffer_h b, struct mtl_buffer_view* v);
/* The layout a buffer was created with (library pool buffers: over the pool region). */
MTL_API_CP int mtl_buffer_get_desc(mtl_buffer_h b, struct mtl_buffer_desc* d);
/* Exclude a buffer from any-free acquire and pool reclaim until unhold, so the app can
   keep reading it after submit in completion NONE (05 §4.5, C5 §6.7). Holds count
   against the pool (blocked_on = MTL_BLOCKED_APP_HOLDS). acquire_buffer still leases
   it. */
MTL_API_DP int mtl_buffer_hold(mtl_buffer_h b);
MTL_API_DP int mtl_buffer_unhold(mtl_buffer_h b);
/* Copy into or out of a leased buffer (bindings, the Python buffer protocol). */
MTL_API_DPC int mtl_lease_copy_in(mtl_lease_h lease, uint32_t plane, uint64_t offset,
                                  const void* src, size_t len);
MTL_API_DPC int mtl_lease_copy_out(mtl_lease_h lease, uint32_t plane, uint64_t offset,
                                   void* dst, size_t len);

enum mtl_pool_source {
  MTL_POOL_LIBRARY = 0, /* MTL allocates pool.count buffers */
  MTL_POOL_ATTACHED =
      1, /* imported buffers attached before start; ALL (forward: NONE ok) */
  MTL_POOL_DYNAMIC = 2, /* Phase 4: a layout per acquire, in a region; ALL forced */
};
enum mtl_path_request {
  MTL_PATH_ALLOW_COPY = 0,
  MTL_PATH_PREFER_DIRECT = 1,
  MTL_PATH_REQUIRE_DIRECT = 2, /* with a conversion: -MTL_ENOTSUP at query/create */
};
/* The granted or used data path. */
enum mtl_path_kind {
  MTL_PATH_KIND_UNSET = 0,
  MTL_PATH_KIND_DIRECT = 1,
  MTL_PATH_KIND_CONVERT = 2,
  MTL_PATH_KIND_COPY = 3,
  MTL_PATH_KIND_COPY_AND_CONVERT = 4, /* TX: copy of a converted frame */
  MTL_PATH_KIND_DIRECT_CPU = 5,       /* RX: payloads placed by CPU copy */
  MTL_PATH_KIND_DIRECT_DMA =
      6, /* RX: placed by the DMA engine, CPU fallback per packet */
  MTL_PATH_KIND_DIRECT_HDS = 7, /* RX: header split (later, -MTL_ENOTSUP in 0.1) */
};
/* Where conversion runs (05 §6.4). */
enum mtl_convert_context {
  MTL_CONVERT_NONE = 0,
  MTL_CONVERT_CALLER = 1, /* inside mtl_tx_submit / mtl_rx_dequeue (DPC) */
  MTL_CONVERT_PLUGIN = 2, /* a converter or codec plugin thread */
  MTL_CONVERT_WORKER = 3, /* a library worker */
};
enum mtl_rx_overflow {
  MTL_RX_DROP_NEW = 0,
  MTL_RX_RECLAIM_OLDEST_READY = 1, /* latest wins; with BY_INDEX: the target slot */
};
enum mtl_rx_slot_select {
  MTL_RX_SLOT_ANY_FREE = 0,
  MTL_RX_SLOT_BY_INDEX = 1, /* slot = media_index mod count (MXL grains) */
};
enum mtl_rx_fill {
  MTL_RX_FILL_DEFAULT = 0, /* ZERO_MISSING for library pools, NONE for attached */
  MTL_RX_FILL_ZERO_MISSING = 1,
  MTL_RX_FILL_NONE = 2,
};
/* Fixed size; versions with mtl_session_config. */
struct mtl_pool_config {
  uint32_t source;         /* enum mtl_pool_source */
  uint32_t count;          /* 0: video max(min_count_direct, 3), others 4 (09 §9) */
  uint32_t data_path;      /* enum mtl_path_request */
  uint32_t rx_overflow;    /* enum mtl_rx_overflow */
  uint32_t rx_slot_select; /* enum mtl_rx_slot_select */
  uint32_t rx_fill;        /* enum mtl_rx_fill */
  uint32_t reserved[6];
};

/* Fixed size. */
struct mtl_plane_requirements {
  uint64_t min_span;     /* at the natural stride */
  uint32_t row_bytes;    /* bytes per row the session reads or writes */
  uint32_t rows;         /* per unit: height, or height / 2 for a field */
  uint32_t stride;       /* the natural stride, used by library pools */
  uint32_t min_stride;   /* any stride >= min_stride (== row_bytes) is accepted */
  uint32_t stride_align; /* 1 = byte granular */
  uint32_t offset_align; /* 1 = byte-granular plane offsets */
};
struct mtl_buffer_requirements {
  uint32_t struct_size;
  uint32_t plane_count;
  struct mtl_plane_requirements plane[MTL_MAX_PLANES];
  uint32_t direct_possible;
  uint32_t min_count;
  uint32_t min_count_direct;      /* 0 = not applicable (non-video) */
  uint32_t max_count;             /* 8 for video and cvideo until engine change E11 */
  int64_t completion_latency_ns;  /* expected last-packet-to-result gap (DIRECT) */
  uint32_t domains;               /* accepted: bit per enum mtl_mem_domain */
  uint32_t meta_capacity;         /* RX meta area per unit */
  uint32_t max_user_meta_bytes;   /* TX user meta per unit, at most 1332 B */
  uint32_t internal_buffer_count; /* frames MTL allocates besides the pool */
  uint64_t internal_bytes;
  uint64_t reserved[4];
};
MTL_API_DP void mtl_buffer_requirements_init(struct mtl_buffer_requirements* p);

/* ---- 10. Media configs and formats (09 §1; since 0.1) ---------------------------- */

/* RFC 4175 transport formats */
enum mtl_video_format {
  MTL_VIDEO_FORMAT_UNSET = 0,
  MTL_VIDEO_YUV422_8BIT = 1,
  MTL_VIDEO_YUV422_10BIT = 2,
  MTL_VIDEO_YUV422_12BIT = 3,
  MTL_VIDEO_YUV422_16BIT = 4,
  MTL_VIDEO_YUV420_8BIT = 5,
  MTL_VIDEO_YUV420_10BIT = 6,
  MTL_VIDEO_YUV444_10BIT = 7,
  MTL_VIDEO_YUV444_12BIT = 8,
  MTL_VIDEO_RGB_8BIT = 9,
  MTL_VIDEO_RGB_10BIT = 10,
  MTL_VIDEO_RGB_12BIT = 11,
};
/* App-side layouts; 0 = the transport format itself (no conversion). */
enum mtl_app_format {
  MTL_APP_SAME_AS_TRANSPORT = 0,
  MTL_APP_YUV422P10LE = 1,
  MTL_APP_V210 = 2,
  MTL_APP_Y210 = 3,
  MTL_APP_UYVY = 4,
  MTL_APP_YUV422P = 5,
  MTL_APP_I420 = 6,
  MTL_APP_NV12 = 7,
  MTL_APP_RGBA = 8,
  MTL_APP_BGRA = 9,
  MTL_APP_RGB8 = 10,
  MTL_APP_YUV444P10LE = 11,
  MTL_APP_GBRP10LE = 12,
};
enum mtl_packing {
  MTL_PACKING_GPM_SL = 0,
  MTL_PACKING_BPM = 1,
  MTL_PACKING_GPM = 2,
};
enum mtl_codec {
  MTL_CODEC_UNSET = 0,
  MTL_CODEC_JPEGXS = 1,
  MTL_CODEC_H264 = 2,
  MTL_CODEC_H265 = 3,
};
/* ST 2110-22 rate model. CBR (the default) sends constant bytes and packets per unit as
   ST 2110-22 requires: short codestreams are padded, an oversize one fails at submit with
   -MTL_ENOSPC (CODESTREAM_OVERSIZE). VBR_MAX is an opt-in, non-compliant legacy mode
   (MTL_INFO_NON_COMPLIANT_2110_22): TRS from the ceiling, packets follow the codestream.
 */
enum mtl_cvideo_rate_mode {
  MTL_CVIDEO_RATE_CBR = 0,
  MTL_CVIDEO_RATE_VBR_MAX = 1,
};
enum mtl_cvideo_pack {
  MTL_CVIDEO_PACK_CODESTREAM = 0,
  MTL_CVIDEO_PACK_SLICE = 1, /* -MTL_ENOTSUP in 0.1 */
};
enum mtl_plugin_device {
  MTL_PLUGIN_DEVICE_AUTO = 0,
  MTL_PLUGIN_DEVICE_CPU = 1,
  MTL_PLUGIN_DEVICE_GPU = 2,
  MTL_PLUGIN_DEVICE_FPGA = 3,
};
/* ST 2110-21 sender types (video and cvideo) */
enum mtl_sender_type {
  MTL_SENDER_N = 0,
  MTL_SENDER_NL = 1,
  MTL_SENDER_W = 2,
};
/* Progressive TX rows that miss their deadline (06 §9) */
enum mtl_progressive_late {
  MTL_PROG_TRUNCATE = 0,
  MTL_PROG_PAD = 1,
  MTL_PROG_STALL = 2,
};
enum mtl_audio_format {
  MTL_AUDIO_FORMAT_UNSET = 0,
  MTL_AUDIO_PCM8 = 1,
  MTL_AUDIO_PCM16 = 2,
  MTL_AUDIO_PCM24 = 3,
  MTL_AUDIO_AM824 = 4, /* ST 2110-31 */
};
enum mtl_anc_timing_model {
  MTL_ANC_CTM = 0,
  MTL_ANC_LLTM = 1,
};
/* Which frame's ANC window a unit uses (06 §5.2) */
enum mtl_anc_window {
  MTL_ANC_WINDOW_AUTO = 0, /* the video member's transmit frame, else own frame + own L */
  MTL_ANC_WINDOW_MEDIA = 1, /* strict: the frame of the media time (L = 0) */
};
enum mtl_detect {
  MTL_DETECT_DEFAULT = 0, /* AUTO: ANC interlace detection, today's default */
  MTL_DETECT_OFF = 1,     /* ST40P_RX_FLAG_DISABLE_AUTO_DETECT */
  MTL_DETECT_ON = 2,
};

/* ST 2110-20 */
struct mtl_video_config {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t interlaced;       /* enum mtl_scan */
  struct mtl_rational fps;   /* the FRAME rate, also when interlaced; MTL_FPS_* */
  uint32_t transport_format; /* enum mtl_video_format */
  uint32_t app_format;       /* enum mtl_app_format */
  uint32_t packing;          /* enum mtl_packing */
  uint32_t sender_type;      /* enum mtl_sender_type; moved from timing */
  uint32_t progressive_late; /* enum mtl_progressive_late; moved from timing */
  uint32_t reserved0;
  int64_t troffset_ns; /* 0 = TRODEFAULT; moved here from timing (A8) */
  uint64_t reserved[8];
};
/* ST 2110-22 */
struct mtl_cvideo_config {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t interlaced; /* CBR is per field when interlaced */
  struct mtl_rational fps;
  uint32_t codec; /* enum mtl_codec */
  /* required: CBR bytes (or the VBR_MAX ceiling) per unit, box header included; the
     grant rounds up to whole packets and reports the headroom in mtl_session_info */
  uint32_t codestream_bytes;
  uint32_t rate_mode;     /* enum mtl_cvideo_rate_mode; 0 = CBR */
  uint32_t app_format;    /* raw format to/from the codec; 0 = app gives codestream */
  uint32_t plugin_device; /* enum mtl_plugin_device */
  uint32_t codec_threads; /* 0 = default */
  uint32_t quality;       /* 0 = default */
  uint32_t pack_type;     /* enum mtl_cvideo_pack */
  uint32_t sender_type;   /* enum mtl_sender_type; moved from timing */
  uint32_t reserved0;
  int64_t troffset_ns; /* 0 = TRODEFAULT; moved here from timing (A8) */
  uint64_t reserved[8];
};
/* mtl_audio_config.audio_flags */
#define MTL_AUDIO_NO_ABSORB 0x1u /* strict contiguity: audio_absorb_samples forced 0 */
/* ST 2110-30/31 */
struct mtl_audio_config {
  uint32_t struct_size;
  uint32_t format;      /* enum mtl_audio_format */
  uint32_t sample_rate; /* 48000 | 96000 | 44100 */
  uint32_t channels;
  uint32_t samples_per_packet;    /* S; ptime = S / rate; 0 = 1 ms */
  uint32_t rx_unit_samples;       /* RX unit; 0 = 10 ms in whole packets */
  uint32_t buffer_capacity_bytes; /* library-pool buffer; 0 = 10 ms */
  uint32_t audio_absorb_samples;  /* 0 = by source kind and media mode (06 §8) */
  uint32_t audio_flags;           /* MTL_AUDIO_* */
  uint32_t audio_build_pacing;    /* 1 = pace in the builder too; from options (A8) */
  uint32_t audio_fifo_ms;         /* builder-to-pacer FIFO; 0 = 10 ms; from options */
  uint32_t reserved0;
  int64_t audio_launch_offset_ns; /* D_a; 0 = by source kind; from timing (A8) */
  uint64_t reserved[8];
};
/* ST 2110-40 */
struct mtl_anc_config {
  uint32_t struct_size;
  uint32_t interlaced;     /* enum mtl_scan of the associated video */
  struct mtl_rational fps; /* the associated video frame rate */
  uint32_t total_lines;    /* TX: SDI raster (TEPO); 0 = the group video's, else 1125 */
  uint32_t detect;         /* RX: enum mtl_detect */
  uint32_t anc_split_by_packet; /* 1 = one ANC packet per RTP packet; from options */
  uint32_t anc_timing_model;    /* enum mtl_anc_timing_model; from timing (A8) */
  uint32_t anc_window_anchor;   /* enum mtl_anc_window */
  uint32_t max_udw_bytes;       /* per unit; 0 = 64 KiB */
  uint32_t max_packets;         /* ANC packets per unit; 0 = 255 */
  uint32_t reserved0;
  int64_t anc_target_delay_ns; /* 0 = the model's default; from timing (A8) */
  uint64_t reserved[8];
};
#define MTL_FASTMETA_FREE_RUNNING 0x1u /* fastmeta_flags: own rate, not the video's */
/* ST 2110-41; RTP clock 90 kHz */
struct mtl_fastmeta_config {
  uint32_t struct_size;
  uint32_t interlaced;      /* enum mtl_scan; field parity from the index */
  struct mtl_rational rate; /* 0/0 = the group video's rate; FREE_RUNNING: required */
  uint32_t data_item_type;  /* TX: DIT on the wire */
  uint32_t k_bit;           /* TX */
  uint32_t fastmeta_flags;  /* MTL_FASTMETA_* */
  uint32_t buffer_capacity_bytes;   /* library-pool buffer; 0 = 64 KiB */
  int64_t fastmeta_target_delay_ns; /* D_fmd (06 §5.2); 0 = the 06 default */
  uint64_t reserved[8];
};
/* ANC packet table entry (TX submission; RX meta area). One UDW byte run per unit with
   per-packet offsets; anc_count <= 255 and udw_count <= 255 (checked); line 0 means
   unspecified. Value type. */
struct mtl_anc_packet {
  uint16_t did;
  uint16_t sdid;
  uint16_t line;
  uint16_t hoffset;
  uint8_t c; /* C bit */
  uint8_t stream;
  uint16_t udw_count;
  uint32_t udw_offset; /* into the unit's UDW byte run */
};
/* Output value type of mtl_video_format_enum(). */
struct mtl_format_pair {
  uint32_t transport; /* enum mtl_video_format */
  uint32_t app;       /* enum mtl_app_format */
  uint32_t path;      /* enum mtl_path_kind */
  uint32_t reserved;
};
MTL_API_DP void mtl_video_config_init(struct mtl_video_config* p);
MTL_API_DP void mtl_cvideo_config_init(struct mtl_cvideo_config* p);
MTL_API_DP void mtl_audio_config_init(struct mtl_audio_config* p);
MTL_API_DP void mtl_anc_config_init(struct mtl_anc_config* p);
MTL_API_DP void mtl_fastmeta_config_init(struct mtl_fastmeta_config* p);

/* Pure functions: no instance. */
MTL_API_CP int mtl_video_layout_query(const struct mtl_video_config* v,
                                      struct mtl_buffer_requirements* out);
MTL_API_CP int mtl_video_format_enum(uint32_t i, struct mtl_format_pair* pair);
/* L4: published FourCC, GStreamer and FFmpeg names of a format (static strings). */
MTL_API_AS int mtl_video_format_names(uint32_t format, uint32_t* MTL_NULLABLE fourcc,
                                      const char** MTL_NULLABLE gst_format,
                                      const char** MTL_NULLABLE av_pix_fmt);

/* ---- 11. Session configuration (09 §1; since 0.1) -------------------------------- */

/* mtl_flow.port: 0 = leg i uses instance port i, MTL_FLOW_PORT(n) = port n (C5). */
#define MTL_FLOW_PORT(n) ((uint32_t)(n) + 1u)
/* mtl_flow.flow_flags */
#define MTL_FLOW_USER_MAC 0x1u      /* use dst_mac, no ARP */
#define MTL_FLOW_MATCH_FMD_DIT 0x2u /* RX ST 2110-41: filter on fmd_dit */
#define MTL_FLOW_MATCH_FMD_K 0x4u   /* RX ST 2110-41: filter on fmd_k */

/* Fixed size. */
struct mtl_flow {
  uint32_t port;             /* 0 = the leg's own port, MTL_FLOW_PORT(n) */
  uint8_t ip_family;         /* 0 = IPv4 */
  uint8_t payload_type;      /* TX: 0 = essence default; RX: 0 = no check */
  uint8_t dscp;              /* the DSCP value; 0 = CS0 (today's TOS 0) */
  uint8_t ttl;               /* 0 = 64 */
  uint8_t ip[16];            /* TX destination; RX group or unicast; required */
  uint8_t source_filter[16]; /* RX SSM source; all zero = none */
  uint16_t udp_port;         /* required: 0 is -MTL_EINVAL */
  uint16_t udp_src_port;     /* TX: 0 = udp_port; RX: 0 = any */
  uint32_t ssrc;             /* TX: 0 = random per session; RX: 0 = no check */
  uint16_t vlan;             /* reserved, must be 0 */
  uint8_t dst_mac[6];        /* only with MTL_FLOW_USER_MAC, else zero */
  uint32_t fmd_dit;          /* RX ST 2110-41, with MTL_FLOW_MATCH_FMD_DIT */
  uint8_t fmd_k;             /* RX ST 2110-41, with MTL_FLOW_MATCH_FMD_K */
  uint8_t reserved0[3];
  uint64_t flow_flags; /* MTL_FLOW_* */
  uint64_t reserved[4];
};

/* 06 §5.1 */
enum mtl_source_kind {
  MTL_SOURCE_PLAYBACK = 0,
  MTL_SOURCE_CAPTURE = 1,
  MTL_SOURCE_GATEWAY = 2,
};
/* 06 §4.1 */
enum mtl_media_mode {
  MTL_MEDIA_AUTO = 0,
  MTL_MEDIA_INDEX = 1,
  MTL_MEDIA_TAI = 2,
};
/* 06 §7.2 */
enum mtl_late_policy {
  MTL_LATE_DEFAULT = 0, /* by media mode: AUTO RESLOT, INDEX and TAI DROP */
  MTL_LATE_DROP = 1,
  MTL_LATE_SEND_LATE = 2, /* bounded */
  MTL_LATE_RESLOT = 3,
};
/* 06 §7.3 */
enum mtl_underrun_policy {
  MTL_UNDERRUN_DEFAULT = 0, /* ST20/22/30 SKIP, ST40 EMPTY_ANC, ST41 KEEPALIVE */
  MTL_UNDERRUN_SKIP = 1,
  MTL_UNDERRUN_EMPTY_ANC = 2,
  MTL_UNDERRUN_KEEPALIVE = 3,
  MTL_UNDERRUN_SILENCE = 4,
  MTL_UNDERRUN_REPEAT_LAST = 5, /* later: -MTL_ENOTSUP in 0.1 */
};
/* TAI media mode snapping, 06 §4.3 */
enum mtl_snap_mode {
  MTL_SNAP_NEAREST = 0,      /* nearest slot; a collision is DROPPED/DUPLICATE_SLOT */
  MTL_SNAP_LOCKED_PHASE = 1, /* opt-in for genlocked sources */
};
enum mtl_tsmode {
  MTL_TSMODE_DEFAULT = 0, /* derived from source kind and media mode (06) */
  MTL_TSMODE_SAMP = 1,
  MTL_TSMODE_NEW = 2,
  MTL_TSMODE_PRES = 3,
};
enum mtl_rtp_mode {
  MTL_RTP_DERIVED = 0,
  MTL_RTP_PASSTHROUGH = 1, /* rtp_override per submission; with AUTO: PASSTHROUGH_AUTO */
};
enum mtl_rx_incomplete {
  MTL_RX_DELIVER = 0,
  MTL_RX_DISCARD = 1,
};
enum mtl_mediaclk {
  MTL_MEDIACLK_DIRECT = 0,
  MTL_MEDIACLK_SENDER = 1,
};
/* LOCKED_PHASE, a unit outside snap_tolerance (06 §4.3) */
enum mtl_off_grid {
  MTL_OFF_GRID_RELOCK = 0, /* relock after 3 consecutive OFF_GRID units */
  MTL_OFF_GRID_REANCHOR = 1,
  MTL_OFF_GRID_DROP = 2,
};

/* Fixed size; versions with mtl_session_config. Cross-essence fields only (A8). */
struct mtl_timing_config {
  mtl_timeline_h timeline;  /* null = the epoch timeline (mtl_timeline_epoch) */
  uint32_t source_kind;     /* enum mtl_source_kind */
  uint32_t media_mode;      /* enum mtl_media_mode */
  uint32_t late_policy;     /* enum mtl_late_policy */
  uint32_t underrun_policy; /* enum mtl_underrun_policy */
  uint32_t snap_mode;       /* enum mtl_snap_mode */
  uint32_t tsmode;          /* enum mtl_tsmode */
  uint32_t rtp_mode;        /* enum mtl_rtp_mode */
  uint32_t rx_incomplete;   /* enum mtl_rx_incomplete */
  uint32_t mediaclk_mode;   /* RX: enum mtl_mediaclk */
  uint32_t off_grid_policy; /* enum mtl_off_grid */
  uint32_t rx_rtp_offset;   /* RX: SDP mediaclk:direct=<offset> */
  uint32_t reserved0;
  int64_t min_tx_delay_ns; /* 0 = by source kind (09 §9) */
  /* TAI mode: shift every unit's media time by the declared latency; RTP moves with
     it. Replaces rtp_timestamp_delta_us. */
  int64_t media_time_offset_ns;
  int64_t late_tolerance_ns;  /* SEND_LATE; 0 = min(TROFFSET, TFRAME - RACTIVE*TFRAME) */
  int64_t snap_tolerance_ns;  /* 0 = by snap mode (09 §9) */
  int64_t horizon_ns;         /* 0 = 1 s from the resolved start instant */
  int64_t media_index_offset; /* whole-unit lip-sync trim */
  int64_t link_offset_ns;     /* RX: presentation = media + link_offset */
  /* TX: declared downstream budget; derives the maximum slot delay; exceeding it is a
     timing warning (LINK_OFFSET_BUDGET), never a rejection. 0 = none. */
  int64_t link_offset_budget_ns;
  int64_t rx_flush_offset_ns;   /* RX: 0 = derived */
  int64_t rx_skew_budget_ns;    /* RX 2022-7: 0 = derived */
  int64_t rx_signal_timeout_ns; /* RX: 0 = derived */
  uint64_t reserved[12];        /* >= 96 B spare (A8) */
};

/* 07 §2.2 */
enum mtl_complete_mode {
  MTL_COMPLETE_DEFAULT = 0, /* library pool NONE; attached, dynamic, exported ALL */
  MTL_COMPLETE_NONE = 1,    /* a non-zero user_cookie is COOKIE_WITHOUT_RESULTS */
  MTL_COMPLETE_EXCEPTIONS = 2,
  MTL_COMPLETE_ALL = 3,
};
/* mtl_completion_config.optional_kinds */
#define MTL_CQE_ENABLE_SOURCE_RELEASED 0x1u
#define MTL_CQE_ENABLE_RX_MISSING 0x2u
/* Fixed size. */
struct mtl_completion_config {
  uint32_t mode;           /* enum mtl_complete_mode */
  uint32_t optional_kinds; /* MTL_CQE_ENABLE_* */
  uint32_t reserved[6];
};

/* 09 §6.2 */
enum mtl_req {
  MTL_REQ_ANY = 0,
  MTL_REQ_PREFER = 1,
  MTL_REQ_REQUIRE = 2,
  MTL_REQ_OFF = 3,
};
/* Fixed size. */
struct mtl_capability_request {
  uint32_t hw_pacing;     /* enum mtl_req */
  uint32_t pacing_class;  /* enum mtl_pacing_class */
  uint32_t dma_offload;   /* enum mtl_req */
  uint32_t hw_timestamps; /* enum mtl_req */
  uint32_t reserved[4];
};

enum mtl_tx_queue {
  MTL_TXQ_AUTO = 0, /* by essence (09 §9.1); video never silently shared */
  MTL_TXQ_DEDICATED = 1,
  MTL_TXQ_SHARED = 2,
};
enum mtl_src_port_mode {
  MTL_SRC_PORT_FIXED = 0,
  MTL_SRC_PORT_RANDOM = 1,
  MTL_SRC_PORT_MULTI = 2,
};
/* Fixed size; cross-essence only (A8). */
struct mtl_session_options {
  uint32_t tx_queue;      /* enum mtl_tx_queue */
  uint32_t numa;          /* lease table and library pool: 0 = ports', MTL_NUMA(n) */
  uint32_t src_port_mode; /* enum mtl_src_port_mode */
  uint32_t rx_burst_size; /* 0 = 128 */
  uint32_t rx_threads;    /* 0 = auto */
  uint32_t reserved[11];
};

/* mtl_session_config.flags */
#define MTL_SESSION_MT_SUBMIT 0x1u     /* MP-safe submit/publish */
#define MTL_SESSION_WAKE_DIRECT 0x2u   /* force W2: the completing context wakes */
#define MTL_SESSION_SINGLE_READER 0x4u /* one reader: the reaper lock is elided */
#define MTL_SESSION_EXPORT_POOL                                                         \
  0x8u                               /* slots exported to a framework pool: forces ALL, \
                                        implies MT_SUBMIT (03 §4.4) */
#define MTL_SESSION_WAKE_WAKER 0x10u /* force W3: the waker thread wakes */

struct mtl_session_config {
  uint32_t struct_size;
  uint32_t direction; /* enum mtl_dir; required: 0 is INVALID_ARGUMENT */
  const void* next;   /* extension chain; must be NULL in 0.1 */
  /* copied at create; unique per instance (-MTL_EEXIST); "" = generated */
  char name[MTL_NAME_MAX];
  uint32_t unit;      /* enum mtl_unit_kind */
  uint32_t leg_count; /* 0 = derive from the populated flows[] */
  struct mtl_flow flows[MTL_MAX_LEGS];
  struct mtl_pool_config pool;
  struct mtl_timing_config timing;
  struct mtl_completion_config completion;
  struct mtl_capability_request caps;
  struct mtl_session_options options;
  uint64_t flags; /* MTL_SESSION_* */
  uint64_t user_cookie;
  uint64_t reserved[8];
};
MTL_API_DP void mtl_session_config_init(struct mtl_session_config* c);
/* L4: "239.168.85.20:20000@0000:af:01.0,pt=112,ssrc=0x1234"; fields the spec does not
   name keep their defaults. */
MTL_API_CP int mtl_flow_parse(const char* spec, struct mtl_flow* f);

/* ---- 12. Session info, status and stats (08; since 0.1) -------------------------- */

/* Fixed size. */
struct mtl_leg_info {
  uint32_t port;
  uint32_t ssrc;         /* granted */
  uint16_t udp_src_port; /* granted */
  uint16_t udp_dst_port;
  uint8_t payload_type;
  uint8_t dscp;
  uint8_t ttl;
  uint8_t reserved0;
  uint8_t src_mac[6];
  uint8_t dst_mac[6]; /* resolved or user MAC; zero while WAITING_NEIGHBOUR */
};
/* SDP a=ts-refclk (08 §4). Fixed size. */
struct mtl_ts_refclk {
  uint8_t gm_identity[8];
  uint32_t domain;
  uint32_t kind; /* 0 unknown, 1 PTP (IEEE 1588-2008), 2 local mac */
};
/* mtl_session_info.flags */
#define MTL_INFO_RTP_OFF_GRID 0x1u
#define MTL_INFO_JTNM_DEFAULT_WINDOW_EXCEEDED 0x2u
#define MTL_INFO_NON_COMPLIANT_2110_22 0x4u
enum mtl_waker_mode {
  MTL_WAKER_UNSET = 0,
  MTL_WAKER_W0_POLL = 1,
  MTL_WAKER_W2_DIRECT = 2,
  MTL_WAKER_W3_THREAD = 3,
};
/* The granted configuration. */
struct mtl_session_info {
  uint32_t struct_size;
  uint32_t direction;
  uint32_t essence; /* enum mtl_essence */
  uint32_t unit;
  char name[MTL_NAME_MAX];
  struct mtl_time created;
  uint32_t flags;       /* MTL_INFO_* */
  uint32_t sched_index; /* was st20p_tx_get_sch_idx() */
  uint32_t leg_count;
  uint32_t pacing_class;   /* enum mtl_pacing_class, granted */
  uint32_t pacing_profile; /* accuracy profile of the granted class */
  uint32_t sender_type;
  uint32_t tsmode;
  uint32_t source_kind;
  uint32_t media_mode;
  uint32_t completion_mode; /* granted enum mtl_complete_mode */
  uint32_t path;            /* enum mtl_path_kind, granted */
  uint32_t tx_queue_kind;   /* granted enum mtl_tx_queue */
  uint32_t waker_mode;      /* enum mtl_waker_mode */
  uint32_t backend_syscalls_on_tasklet;
  uint32_t numa;          /* node of the lease table (plain number) */
  uint32_t numa_mismatch; /* pool pages on another socket than the ports */
  uint32_t pool_count;
  uint32_t min_count_direct;
  uint32_t max_count;
  uint32_t internal_buffer_count;
  uint32_t pkts_per_unit;
  uint32_t unit_samples;          /* audio: granted samples per unit */
  uint32_t buffer_capacity_bytes; /* granted */
  uint32_t max_udw_bytes;         /* anc, granted */
  uint32_t max_user_meta_bytes;
  uint32_t meta_max_bytes;   /* RX meta area per unit */
  uint32_t codestream_bytes; /* cvideo, granted in whole packets */
  uint32_t box_hdr_bytes;    /* cvideo */
  uint32_t cvideo_rate_mode;
  uint32_t anc_tm;                  /* SDP TM= of the ANC timing model */
  uint32_t anc_window_frame_offset; /* 06 §5.2 */
  uint32_t max_slot_delay;          /* L bound derived from link_offset_budget_ns */
  uint32_t slot_delay;              /* L, in units */
  uint32_t vrx_full;
  uint32_t cmax;
  uint32_t seq_restarted; /* the RTP sequence was re-randomised at the last start */
  struct mtl_leg_info leg[MTL_MAX_LEGS];
  struct mtl_ts_refclk ts_refclk;
  uint64_t unit_bytes;      /* one unit in the app layout (was st20p_tx_frame_size) */
  uint64_t pool_slot_pitch; /* library pools: bytes between slots in the pool region */
  uint64_t internal_bytes;
  int64_t troffset_ns;
  int64_t trs_ps;             /* packet spacing, picoseconds */
  int64_t tsdelay_ns;         /* SDP TSDELAY */
  int64_t pickup_lead_ns;     /* deadline to scheduled-first gap of the engine */
  int64_t min_submit_lead_ns; /* the framework's latency (06 §5.4) */
  int64_t media_time_offset_ns;
  int64_t completion_latency_ns;
  int64_t expected_wake_latency_ns;
  int64_t latency_min_ns; /* LATENCY query range */
  int64_t latency_max_ns;
  int64_t tolerated_skew_ns; /* RX 2022-7 path differential */
  int64_t horizon_ns;
  int64_t rx_flush_offset_ns;
  int64_t cbr_headroom_bytes; /* cvideo */
  uint32_t convert_context;   /* granted enum mtl_convert_context */
  uint32_t converter;         /* 0 = none, 1 = internal SIMD, 2 = plugin (name via
                                 mtl_session_stat_get) */
  uint32_t total_lines;       /* anc TX: the SDI raster used for TEPO (06 §5.2) */
  uint32_t reserved0;
  uint64_t reserved[6];
};

enum mtl_leg_admin {
  MTL_LEG_ADMIN_UNSET = 0,
  MTL_LEG_ENABLED = 1,
  MTL_LEG_DISABLED = 2,
};
enum mtl_leg_oper {
  MTL_LEG_OPER_UNSET = 0,
  MTL_LEG_UP = 1,
  MTL_LEG_DOWN = 2,
};
/* Per leg; the FLOW_STATE event. */
enum mtl_flow_state {
  MTL_FLOW_STATE_UNSET = 0,
  MTL_FLOW_WAITING_NEIGHBOUR = 1, /* TX: ARP unresolved; DROPPED/NO_NEIGHBOUR */
  MTL_FLOW_RESOLVED = 2,
  MTL_FLOW_JOINING = 3, /* RX: IGMP join sent */
  MTL_FLOW_JOINED = 4,
  MTL_FLOW_JOIN_FAILED = 5,
};
#define MTL_LEG_TV_LAST_PACKET 0x1u
/* Fixed size. */
struct mtl_leg_status {
  uint32_t admin;      /* enum mtl_leg_admin */
  uint32_t oper;       /* enum mtl_leg_oper (link monitor, Phase 2) */
  uint32_t flow_state; /* enum mtl_flow_state */
  uint32_t receiving;  /* RX: packets within rx_signal_timeout */
  uint32_t time_valid; /* MTL_LEG_TV_* */
  uint32_t igmp_reports;
  int64_t last_packet_tai_ns;
};
/* The latest timing warning; sticky until the next start. Fixed size. */
struct mtl_timing_warning {
  uint32_t reason; /* TIMING_SHORTFALL, LINK_OFFSET_BUDGET, TIME_ESTIMATED or NONE */
  uint32_t reserved;
  int64_t shortfall_ns;
  int64_t suggested_min_tx_delay_ns;
};
struct mtl_session_status {
  uint32_t struct_size;
  uint32_t state;             /* enum mtl_session_state */
  uint32_t reason;            /* enum mtl_state_reason of the last transition */
  uint32_t last_error_reason; /* why the session entered ERROR, else NONE */
  uint32_t blocked_on;        /* enum mtl_blocked_on */
  uint32_t rx_signal;         /* RX: 1 = present */
  uint32_t format_changed;    /* RX: detected-property bitmask against the config */
  uint32_t pacing_class;      /* current, after a runtime downgrade */
  uint32_t path;              /* current enum mtl_path_kind */
  uint32_t inline_hook;       /* reserved (04 §6) */
  uint32_t leg_count;
  int32_t error; /* in ERROR: the code data calls return (-MTL_EIO) */
  struct mtl_timing_warning timing_warning;
  struct mtl_leg_status leg[MTL_MAX_LEGS];
  uint64_t reserved[8];
};

/* Fixed size. log2 buckets unless the MTL_OPT_HIST_LINEAR option is set. */
struct mtl_histogram {
  uint64_t count;
  int64_t sum;
  int64_t min;
  int64_t max;
  uint64_t bucket[MTL_HIST_BUCKETS];
};
/* Fixed size: windowed maxima. */
struct mtl_window_max {
  int64_t last_1s;
  int64_t last_60s;
};
/* Fixed size; cumulative counters only (no epochs, no reset). */
struct mtl_tx_stats {
  uint64_t units_submitted;
  uint64_t units_on_time;
  uint64_t units_late;
  uint64_t units_flushed;
  uint64_t units_withdrawn;
  uint64_t units_before_start;
  uint64_t units_failed;
  uint64_t units_suppressed; /* results suppressed by NONE and EXCEPTIONS */
  uint64_t slots_empty;
  uint64_t units_repeated;
  uint64_t units_padded; /* keep-alive, silence, empty ANC */
  uint64_t acquire_blocked_on_results;
  uint64_t acquire_blocked_on_buffers;
  uint64_t acquire_blocked_on_app_leases;
  uint64_t build_overrun;
  uint64_t recoveries_ok; /* the count of record */
  uint64_t recoveries_failed;
  uint64_t cmd_timeouts;
  uint64_t bytes;
  uint64_t pkts;
  uint64_t pkts_copied_partial;
  uint64_t tpr_late_pkts;
  uint64_t reserved0;
  uint64_t units_dropped[MTL_TX_REASON_SLOTS]; /* by enum mtl_tx_reason */
  struct mtl_window_max vrx_max;
  struct mtl_window_max cinst_max;
  struct mtl_window_max margin_min;
  struct mtl_histogram margin_hist;
  struct mtl_histogram launch_error_hist;
  uint64_t caller_work_ns; /* DPC work done in the caller (conversion) */
  uint64_t reserved[16];
  struct mtl_histogram vrx_hist;   /* TX self-check (06 §12): VRX level per packet */
  struct mtl_histogram cinst_hist; /* CINST level per packet */
};
/* Fixed size. */
struct mtl_rx_stats {
  uint64_t units_delivered;
  uint64_t units_complete;
  uint64_t units_incomplete_delivered;
  uint64_t units_incomplete_discarded;
  uint64_t units_used_redundancy;
  uint64_t units_missed_pool_full;
  uint64_t units_reclaimed;
  uint64_t units_flushed;
  uint64_t units_before_start; /* ARMED: media time before the start */
  uint64_t units_stale;
  uint64_t units_dropped_notify; /* the engine refused delivery (SF-45) */
  uint64_t pkts_received;
  uint64_t pkts_redundant;
  uint64_t pkts_stale;
  uint64_t pkts_dma;
  uint64_t pkts_cpu;
  uint64_t pkts_lost_est;
  uint64_t lost_is_exact;
  uint64_t tp_narrow; /* timing parser (opt-in) */
  uint64_t tp_wide;
  uint64_t tp_fail;
  uint64_t tp_untrusted_pkts;
  uint64_t cmd_timeouts;
  uint64_t pkts_rejected[MTL_RX_REJECT_SLOTS]; /* by enum mtl_rx_reject */
  struct mtl_window_max latency_max;
  struct mtl_histogram latency_hist;
  struct mtl_histogram delivery_hist;
  uint64_t caller_work_ns; /* DPC work done in the caller (conversion, zero-fill) */
  uint64_t format_changes; /* RX_FORMAT changes detected */
  struct mtl_window_max tp_vrx_max; /* timing parser summary (opt-in, 08 §2.7) */
  struct mtl_window_max tp_cinst_max;
  struct mtl_window_max tp_fpt_max; /* first packet time */
  struct mtl_window_max tp_latency_max;
  uint64_t reserved[5];
};
union mtl_dir_stats {
  struct mtl_tx_stats tx;
  struct mtl_rx_stats rx;
};
/* Fixed size. */
struct mtl_leg_stats {
  uint64_t pkts;
  uint64_t bytes;
  uint64_t pkts_lost;
  uint64_t pkts_reordered;
  uint64_t pkts_duplicate_same_leg;
  uint64_t pkts_skipped; /* TX: units not sent on this leg (down, disabled) */
  uint64_t igmp_reports;
  uint32_t link_state; /* gauge: enum mtl_leg_oper */
  uint32_t time_valid; /* MTL_LEG_TV_* */
  int64_t last_packet_tai_ns;
  int64_t observed_skew_ns; /* TX 2022-7 gauge */
  uint64_t reserved[4];
};
/* Lease states, the index of mtl_queue_gauges.entries[] and .exits[]. */
enum mtl_lease_state {
  MTL_LEASE_FREE = 0,
  MTL_LEASE_APP_WRITABLE = 1,
  MTL_LEASE_QUEUED = 2,
  MTL_LEASE_IN_FLIGHT = 3,
  MTL_LEASE_DONE = 4,
  MTL_LEASE_RECEIVING = 5,
  MTL_LEASE_READY = 6,
  MTL_LEASE_APP_READING = 7,
  MTL_LEASE_HELD_BY_TX = 8, /* RX slot released by the app, held by TX submissions */
  MTL_LEASE_STATES = 9,
};
/* One pass over the slot state words; entries - exits = gauge per state (G-43). */
struct mtl_queue_gauges {
  uint32_t gauge[12]; /* by enum mtl_lease_state; the rest reserved */
  uint32_t unread_results;
  uint32_t blocked_on;
  uint32_t held_by_app; /* mtl_buffer_hold() holds */
  uint32_t reserved0;
  int64_t queued_media_ns;
  uint64_t entries[12]; /* cumulative, by enum mtl_lease_state */
  uint64_t exits[12];
  uint64_t reserved[4];
};
/* Fixed-size per-essence tails (C5 §2.20). */
struct mtl_video_stats {
  uint64_t pkts_dma;
  uint64_t pkts_copied_partial;
  uint64_t reserved[14];
};
struct mtl_cvideo_stats {
  uint64_t padding_bytes;
  uint64_t oversize_rejected;
  uint64_t reserved[14];
};
struct mtl_audio_stats {
  uint64_t samples_padded;
  uint64_t samples_dropped;
  uint64_t samples_inserted;
  int64_t audio_drift_samples; /* gauge: accumulated absorbed offset */
  uint64_t rephase_count;      /* explicit DISCONTINUITY re-phases (06 §8) */
  uint64_t reserved[11];
};
struct mtl_anc_stats {
  uint64_t anc_packets;
  uint64_t udw_bytes;
  uint64_t reserved[14];
};
struct mtl_fastmeta_stats {
  uint64_t items;
  uint64_t keepalives;
  uint64_t reserved[14];
};
union mtl_media_stats {
  struct mtl_video_stats video;
  struct mtl_cvideo_stats cvideo;
  struct mtl_audio_stats audio;
  struct mtl_anc_stats anc;
  struct mtl_fastmeta_stats fastmeta;
  uint64_t raw[16];
};
struct mtl_session_stats {
  uint32_t struct_size;
  uint32_t state;
  uint32_t essence;
  uint32_t legs;
  char name[MTL_NAME_MAX];
  struct mtl_time created;
  struct mtl_time snapshot;
  uint64_t unsupported_mask; /* counters the backend cannot produce */
  union mtl_dir_stats dir;
  struct mtl_leg_stats leg[MTL_MAX_LEGS];
  struct mtl_queue_gauges queue;
  union mtl_media_stats media;
  uint64_t reserved[8];
};
MTL_API_DP void mtl_session_info_init(struct mtl_session_info* p);
MTL_API_DP void mtl_session_status_init(struct mtl_session_status* p);
MTL_API_DP void mtl_session_stats_init(struct mtl_session_stats* p);

enum mtl_stat_object {
  MTL_STAT_SESSION = 1,
  MTL_STAT_PORT = 2,
  MTL_STAT_INSTANCE = 3,
};
enum mtl_stat_kind {
  MTL_STAT_COUNTER = 1,
  MTL_STAT_GAUGE = 2,
  MTL_STAT_HIST_BUCKET = 3,
};
/* Output value type. */
struct mtl_stat_desc {
  uint32_t key;
  uint32_t kind;        /* enum mtl_stat_kind */
  uint32_t unit;        /* 0 count, 1 ns, 2 bytes, 3 packets */
  uint32_t param_count; /* valid `param` values; 0 = none */
  char name[48];        /* "nic.rx_missed", "rl.shaper_drops", ... */
};
/* Key-addressed and typed per object (A4): backend-specific or rare values. */
MTL_API_DP int mtl_session_stat_get(mtl_session_h s, uint32_t key, uint32_t param,
                                    uint64_t* value);
MTL_API_DP int mtl_port_stat_get(mtl_instance_h mt, uint32_t port, uint32_t key,
                                 uint32_t param, uint64_t* value);
MTL_API_DP int mtl_instance_stat_get(mtl_instance_h mt, uint32_t key, uint32_t param,
                                     uint64_t* value);
MTL_API_CP int mtl_stat_list(uint32_t object_kind, struct mtl_stat_desc* descs,
                             uint32_t cap, uint32_t* n);

/* Backend-specific and pacing-tuning options (A8): enumerable by
   mtl_session_option_list (names such as "hist.margin.linear"); CREATED/STOPPED only. */
enum mtl_option_key {
  MTL_OPT_HIST_LINEAR = 1,      /* deprecated alias of MTL_OPT_HIST_LINEAR_MARGIN */
  MTL_OPT_RX_TIMING_PARSER = 2, /* 1 = run the RX ST 2110-21 timing parser */
  MTL_OPT_VIDEO_DISABLE_BULK = 3,
  MTL_OPT_VIDEO_STATIC_PAD_P = 4,
  MTL_OPT_VIDEO_START_VRX = 5,
  MTL_OPT_VIDEO_PAD_INTERVAL = 6,
  MTL_OPT_AUDIO_RL_WARMUP = 7,
  /* Linear histogram layout, one key per histogram (08 §2.6): the value is the bucket
     width in the histogram's unit (ns for margin/launch/latency/delivery, packets for
     VRX/CINST); 0 = log2 (the default). CREATED or STOPPED only. */
  MTL_OPT_HIST_LINEAR_MARGIN = 8,
  MTL_OPT_HIST_LINEAR_LAUNCH_ERROR = 9,
  MTL_OPT_HIST_LINEAR_LATENCY = 10,
  MTL_OPT_HIST_LINEAR_DELIVERY = 11,
  MTL_OPT_HIST_LINEAR_VRX = 12,
  MTL_OPT_HIST_LINEAR_CINST = 13,
};
/* Output value type. */
struct mtl_option_desc {
  uint32_t key;
  uint32_t reserved;
  uint64_t min;
  uint64_t max;
  uint64_t def;
  char name[40];
};
MTL_API_CP int mtl_session_set_option(mtl_session_h s, uint32_t key, uint64_t value);
MTL_API_CP int mtl_session_get_option(mtl_session_h s, uint32_t key, uint64_t* value);
MTL_API_CP int mtl_session_option_list(mtl_session_h s, struct mtl_option_desc* descs,
                                       uint32_t cap, uint32_t* n);

/* ---- 13. Timelines and groups (06 §3, §10; 03 §5; since 0.1) --------------------- */

enum mtl_anchor_mode {
  MTL_ANCHOR_AT_START = 0,  /* lazy: T0 on the common grid at the first start */
  MTL_ANCHOR_NEXT_GRID = 1, /* lazy, never before tai_ns */
  MTL_ANCHOR_AT_TAI = 2,    /* T0 = tai_ns snapped up to the grid G (06 §3.1) */
  MTL_ANCHOR_EPOCH = 3,     /* T0 = the SMPTE epoch */
};
enum mtl_step_policy {
  MTL_STEP_DEFAULT = 0, /* EPOCH: KEEP; the others: REANCHOR */
  MTL_STEP_REANCHOR = 1,
  MTL_STEP_KEEP = 2,
};
#define MTL_TIMELINE_GRID_EXPLICIT 0x1u /* use `grid`, not the derived common grid */
struct mtl_timeline_config {
  uint32_t struct_size;
  uint32_t anchor_mode;     /* enum mtl_anchor_mode */
  int64_t tai_ns;           /* AT_TAI anchor; NEXT_GRID lower bound */
  struct mtl_rational grid; /* with MTL_TIMELINE_GRID_EXPLICIT */
  uint64_t flags;
  uint32_t step_policy; /* enum mtl_step_policy */
  uint32_t reserved0;
  uint64_t reserved[4];
};
/* mtl_timeline_info.flags */
#define MTL_TIMELINE_RESOLVED 0x1u /* T0 is resolved */
struct mtl_timeline_info {
  uint32_t struct_size;
  uint32_t anchor_mode;
  uint32_t flags; /* MTL_TIMELINE_RESOLVED */
  uint32_t members;
  int64_t t0_tai_ns;
  struct mtl_rational t0_s; /* exact T0 in seconds */
  struct mtl_rational grid; /* the common grid G */
  int64_t frame_offset;     /* media_index_offset kept by the timeline (06 §10.6) */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_timeline_config_init(struct mtl_timeline_config* p);
MTL_API_DP void mtl_timeline_info_init(struct mtl_timeline_info* p);
MTL_API_CP int mtl_timeline_create(mtl_instance_h mt, const struct mtl_timeline_config* c,
                                   mtl_timeline_h* out);
/* Named per instance, in a namespace separate from session names. A second open with a
   different config is -MTL_EEXIST (TIMELINE_CONFIG_MISMATCH). */
MTL_API_CP int mtl_timeline_open(mtl_instance_h mt, const char* name,
                                 const struct mtl_timeline_config* c,
                                 mtl_timeline_h* out);
/* Opened timelines: the last close destroys. */
MTL_API_CP int mtl_timeline_close(mtl_timeline_h tl);
/* Created timelines; -MTL_EBUSY while a session or group uses it. */
MTL_API_CP int mtl_timeline_destroy(mtl_timeline_h tl);
/* The epoch timeline as a real handle (null only for an invalid instance). */
MTL_API_DP mtl_timeline_h mtl_timeline_epoch(mtl_instance_h mt);
/* -MTL_EAGAIN until a lazy anchor is resolved. */
MTL_API_DP int mtl_timeline_get_anchor(mtl_timeline_h tl,
                                       struct mtl_rational* MTL_NULLABLE t0_s,
                                       struct mtl_time* MTL_NULLABLE t0);
MTL_API_DP int mtl_timeline_get_info(mtl_timeline_h tl, struct mtl_timeline_info* info);

struct mtl_start_params; /* section 14 */
enum mtl_group_state {
  MTL_GROUP_UNSET = 0,
  MTL_GROUP_CREATED = 1,
  MTL_GROUP_ARMED = 2,
  MTL_GROUP_RUNNING = 3,
  MTL_GROUP_STOPPED = 4,
  MTL_GROUP_PARTIAL_FAILURE = 5,
};
struct mtl_group_config {
  uint32_t struct_size;
  uint32_t direction;     /* enum mtl_dir; required */
  int64_t link_offset_ns; /* RX groups: one presentation offset for every member */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_group_config_init(struct mtl_group_config* p);
/* cfg NULL = a TX group without link offset. RX groups need an EPOCH or AT_TAI
   timeline (RX_TIMELINE_LAZY otherwise). */
MTL_API_CP int mtl_group_create(mtl_instance_h mt, mtl_timeline_h tl,
                                const struct mtl_group_config* MTL_NULLABLE cfg,
                                mtl_group_h* out);
/* A CREATED or STOPPED session. Legal on a RUNNING group: the session joins at the next
   feasible index with the timeline's offset. TX members need media_mode INDEX or TAI. */
MTL_API_CP int mtl_group_add(mtl_group_h g, mtl_session_h s);
/* A STOPPED member. */
MTL_API_CP int mtl_group_remove(mtl_group_h g, mtl_session_h s);
MTL_API_CP int mtl_group_start(mtl_group_h g,
                               const struct mtl_start_params* MTL_NULLABLE p,
                               struct mtl_time* MTL_NULLABLE chosen);
MTL_API_CP int mtl_group_stop(mtl_group_h g, uint32_t mode, int64_t timeout_ns);
/* enum mtl_group_state, or < 0. */
MTL_API_DP int mtl_group_get_state(mtl_group_h g);
MTL_API_CP int mtl_group_destroy(mtl_group_h g);

/* ---- 14. Sessions: create, query, lifecycle (03; since 0.1) ---------------------- */

#define MTL_QUERY_CHECK_CAPACITY 0x1u /* the dry run also checks free capacity */

/* One create and one dry-run query per essence; the query allocates nothing. With
   MTL_QUERY_CHECK_CAPACITY it returns -MTL_ENOSPC with the limiting resource as the
   reason. */
MTL_API_CP int mtl_video_session_create(mtl_instance_h mt,
                                        const struct mtl_session_config* sc,
                                        const struct mtl_video_config* mc,
                                        mtl_session_h* out);
MTL_API_CP int mtl_cvideo_session_create(mtl_instance_h mt,
                                         const struct mtl_session_config* sc,
                                         const struct mtl_cvideo_config* mc,
                                         mtl_session_h* out);
MTL_API_CP int mtl_audio_session_create(mtl_instance_h mt,
                                        const struct mtl_session_config* sc,
                                        const struct mtl_audio_config* mc,
                                        mtl_session_h* out);
MTL_API_CP int mtl_anc_session_create(mtl_instance_h mt,
                                      const struct mtl_session_config* sc,
                                      const struct mtl_anc_config* mc,
                                      mtl_session_h* out);
MTL_API_CP int mtl_fastmeta_session_create(mtl_instance_h mt,
                                           const struct mtl_session_config* sc,
                                           const struct mtl_fastmeta_config* mc,
                                           mtl_session_h* out);
MTL_API_CP int mtl_video_session_query(mtl_instance_h mt,
                                       const struct mtl_session_config* sc,
                                       const struct mtl_video_config* mc, uint64_t flags,
                                       struct mtl_session_info* MTL_NULLABLE info,
                                       struct mtl_buffer_requirements* MTL_NULLABLE req);
MTL_API_CP int mtl_cvideo_session_query(mtl_instance_h mt,
                                        const struct mtl_session_config* sc,
                                        const struct mtl_cvideo_config* mc,
                                        uint64_t flags,
                                        struct mtl_session_info* MTL_NULLABLE info,
                                        struct mtl_buffer_requirements* MTL_NULLABLE req);
MTL_API_CP int mtl_audio_session_query(mtl_instance_h mt,
                                       const struct mtl_session_config* sc,
                                       const struct mtl_audio_config* mc, uint64_t flags,
                                       struct mtl_session_info* MTL_NULLABLE info,
                                       struct mtl_buffer_requirements* MTL_NULLABLE req);
MTL_API_CP int mtl_anc_session_query(mtl_instance_h mt,
                                     const struct mtl_session_config* sc,
                                     const struct mtl_anc_config* mc, uint64_t flags,
                                     struct mtl_session_info* MTL_NULLABLE info,
                                     struct mtl_buffer_requirements* MTL_NULLABLE req);
MTL_API_CP int mtl_fastmeta_session_query(
    mtl_instance_h mt, const struct mtl_session_config* sc,
    const struct mtl_fastmeta_config* mc, uint64_t flags,
    struct mtl_session_info* MTL_NULLABLE info,
    struct mtl_buffer_requirements* MTL_NULLABLE req);

MTL_API_CP int mtl_session_get_buffer_requirements(mtl_session_h s,
                                                   struct mtl_buffer_requirements* r);
/* CREATED or STOPPED. Access is enforced here: RX needs MTL_MEM_WRITE, TX needs
   MTL_MEM_READ (ACCESS_MISMATCH). */
MTL_API_CP int mtl_session_attach_buffers(mtl_session_h s, const mtl_buffer_h* b,
                                          uint32_t n);
MTL_API_CP int mtl_session_detach_buffers(mtl_session_h s);
/* A library pool as a region, so other sessions can build buffers over it (RX to TX);
   slot j starts at j * mtl_session_info.pool_slot_pitch. */
MTL_API_CP int mtl_session_get_pool_region(mtl_session_h s, mtl_region_h* out);
/* The session's base buffers (library or attached) in slot order; *n = total. */
MTL_API_CP int mtl_session_get_buffers(mtl_session_h s, mtl_buffer_h* out, uint32_t cap,
                                       uint32_t* n);
/* CREATED or STOPPED. */
MTL_API_CP int mtl_session_bind_cq(mtl_session_h s, mtl_cq_h cq);
MTL_API_CP int mtl_session_bind_eq(mtl_session_h s, mtl_eq_h eq);
/* The private CQ. */
MTL_API_DP int mtl_session_get_cq(mtl_session_h s, mtl_cq_h* cq);
/* The private EQ; takes an app reference, dropped by mtl_eq_destroy(), so it can be read
   after the session retires. Legal in DESTROYING. */
MTL_API_CP int mtl_session_get_eq(mtl_session_h s, mtl_eq_h* eq);

enum mtl_start_mode {
  MTL_START_NOW = 0,
  MTL_START_AT_TAI = 1,
  MTL_START_AT_MEDIA_INDEX = 2,
};
struct mtl_start_params {
  uint32_t struct_size;
  uint32_t mode;       /* enum mtl_start_mode */
  int64_t media_index; /* AT_MEDIA_INDEX: unit k on the session's (group's) timeline */
  int64_t tai_ns;      /* AT_TAI */
  int64_t preroll_ns;  /* extra lead added to the T0 resolution (06 §3.1) */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_start_params_init(struct mtl_start_params* p);
/* p may be NULL (NOW). A queued ARMED unit beyond the horizon from the resolved start
   fails the whole start with -MTL_ERANGE (BEYOND_HORIZON), and nothing starts. */
MTL_API_CP int mtl_session_start(mtl_session_h s,
                                 const struct mtl_start_params* MTL_NULLABLE p);
/* mode: enum mtl_stop_mode. */
MTL_API_CP int mtl_session_stop(mtl_session_h s, uint32_t mode, int64_t timeout_ns);

#define MTL_DISCARD_REBASE 0x1u /* map first_index to the next feasible slot */
struct mtl_discard_params {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t flags;      /* MTL_DISCARD_* */
  int64_t first_index; /* with MTL_DISCARD_REBASE */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_discard_params_init(struct mtl_discard_params* p);
/* Stays RUNNING (seek, FLUSH_STOP): queued units become FLUSHED/DISCARD and the next
   submission is an implicit DISCONTINUITY; REBASE re-sets media_index_offset. On RX it
   force-completes the unit being assembled. */
MTL_API_CP int mtl_session_discard_queued(
    mtl_session_h s, const struct mtl_discard_params* MTL_NULLABLE p);
/* flags 0: deferred while app leases are out. SESSION_RETIRED is posted in both cases
   (session EQ and every EQ subscribed with MTL_EQ_SUB_SESSION). MTL_DESTROY_FORCE also
   escalates a deferred destroy that is already in progress. */
MTL_API_CP int mtl_session_destroy(mtl_session_h s, uint64_t flags);

struct mtl_reconfigure_params {
  uint32_t struct_size;
  uint32_t pool_count;         /* 0 = keep */
  const void* media_config;    /* NULL = keep; else the session's own essence config */
  const struct mtl_flow* legs; /* NULL = keep; the only way to change flow.port */
  uint32_t leg_count;          /* with legs: the session's leg count */
  uint32_t reserved0;
  uint64_t reserved[4];
};
MTL_API_DP void mtl_reconfigure_params_init(struct mtl_reconfigure_params* p);
/* CREATED or STOPPED. Keeps handle, name, SSRC, stats, timeline, group, CQ/EQ, RX rules
   and joins; re-derives the requirements (library pools re-created, attached pools
   re-validated: RECONFIGURE_INCOMPATIBLE). */
MTL_API_CP int mtl_session_reconfigure(mtl_session_h s,
                                       const struct mtl_reconfigure_params* p);

enum mtl_activation_kind {
  MTL_ACTIVATE_NOW = 0,
  MTL_ACTIVATE_AT_TAI = 1,
  MTL_ACTIVATE_AT_MEDIA_INDEX = 2, /* TX only */
};
struct mtl_activation {
  uint32_t struct_size;
  uint32_t kind; /* enum mtl_activation_kind */
  int64_t tai_ns;
  int64_t media_index;
  uint64_t reserved[2];
};
MTL_API_DP void mtl_activation_init(struct mtl_activation* p);
/* All-or-nothing across legs (IS-05): neighbours resolved and rules and queues reserved
   before commit; on failure nothing changes. act may be NULL (NOW). */
MTL_API_CP int mtl_session_update_flows(mtl_session_h s, const struct mtl_flow* legs,
                                        uint32_t n,
                                        const struct mtl_activation* MTL_NULLABLE act);
/* Legal in every state up to STOPPED; in ARMED, RUNNING, DRAINING and FLUSHING a CP
   boundary command applied at the next unit boundary (04 §3.2). Every configured leg
   stays reserved. */
MTL_API_CP int mtl_session_set_leg_enabled(mtl_session_h s, uint32_t leg, int enabled);

/* Waiters (A10): one armed word per target. interrupt is sticky until uninterrupt and a
   no-op on a DESTROYING session (destroy wins). */
MTL_API_AS int mtl_session_interrupt(mtl_session_h s);
MTL_API_CP int mtl_session_uninterrupt(mtl_session_h s);
MTL_API_CP int mtl_session_get_wait_object(mtl_session_h s, uint64_t mask,
                                           struct mtl_wait_object* out);
/* 1 = something in mask is ready; 0 = armed, block on the wait object; < 0 = error. */
MTL_API_DP int mtl_session_trywait(mtl_session_h s, uint64_t mask);
/* Multi-target wait. > 0: the ready subset of mask; or -MTL_ETIMEDOUT, -MTL_ECANCELED. */
MTL_API_WT int mtl_session_wait(mtl_session_h s, uint64_t mask, int64_t timeout_ns);
/* Cheap DP getter: enum mtl_session_state as the result, or < 0. A retired handle
   returns MTL_STATE_RETIRED until its table slot is reused. */
MTL_API_DP int mtl_session_get_state(mtl_session_h s);
MTL_API_CP int mtl_session_get_info(mtl_session_h s, struct mtl_session_info* info);
/* The full status copy (CP, 04 §3.3). */
MTL_API_CP int mtl_session_get_status(mtl_session_h s, struct mtl_session_status* st);
MTL_API_DP int mtl_session_get_stats(mtl_session_h s, struct mtl_session_stats* st);

/* ---- 15. Completion records (07 §1; since 0.1) ----------------------------------- */

enum mtl_cqe_kind {
  MTL_CQE_UNSET = 0,
  MTL_CQE_TX_RESULT = 1,
  MTL_CQE_RX_UNIT = 2,
  MTL_CQE_RX_PROGRESS = 3,
  MTL_CQE_TX_SOURCE_RELEASED = 4, /* optional: MTL_CQE_ENABLE_SOURCE_RELEASED */
  MTL_CQE_RX_MISSING = 5,         /* optional follow-on: MTL_CQE_ENABLE_RX_MISSING */
};
/* mtl_cq_entry_hdr.flags */
#define MTL_CQE_MORE 0x1u                 /* another entry for this unit follows */
#define MTL_CQE_FINAL 0x2u                /* the last progress entry of a unit */
#define MTL_RX_F_USED_REDUNDANCY 0x10000u /* RX: complete thanks to the other leg */

/* hdr.status of MTL_CQE_TX_RESULT */
enum mtl_tx_status {
  MTL_TX_STATUS_UNSET = 0,
  MTL_TX_ON_TIME = 1,
  MTL_TX_LATE = 2,
  MTL_TX_DROPPED = 3,
  MTL_TX_FLUSHED = 4,
  MTL_TX_FAILED = 5,
};
/* mtl_tx_result.reason and .leg_reason[] (07 §1.1.1); every value < MTL_TX_REASON_SLOTS
 */
enum mtl_tx_reason {
  MTL_TX_REASON_NONE = 0,
  MTL_TX_REASON_TOO_LATE = 1,
  MTL_TX_REASON_WOULD_OVERLAP = 2,
  MTL_TX_REASON_DUPLICATE_SLOT = 3,  /* TAI: two units snapped to one slot */
  MTL_TX_REASON_DUPLICATE_INDEX = 4, /* INDEX: an index equal to the last one */
  MTL_TX_REASON_OFF_GRID = 5,
  MTL_TX_REASON_LINK_DOWN = 6,
  MTL_TX_REASON_NO_NEIGHBOUR = 7,
  MTL_TX_REASON_LEG_DISABLED = 8,
  MTL_TX_REASON_RECOVERY = 9,
  MTL_TX_REASON_INVALID_PAYLOAD = 10,
  MTL_TX_REASON_ENCODER_FAILED = 11,
  MTL_TX_REASON_REJECTED_AT_PICKUP = 12,
  MTL_TX_REASON_STOP_FLUSH = 13,
  MTL_TX_REASON_STOP_TIMEOUT = 14,
  MTL_TX_REASON_DISCARD = 15,   /* mtl_session_discard_queued */
  MTL_TX_REASON_WITHDRAWN = 16, /* mtl_tx_withdraw */
  MTL_TX_REASON_DESTROY = 17,
  MTL_TX_REASON_BEFORE_START = 18,
  MTL_TX_REASON_SESSION_ERROR = 19,
  MTL_TX_REASON_TX_QUEUE_FATAL = 20,
  MTL_TX_REASON_DEVICE_GONE = 21,
  MTL_TX_REASON_BEHIND = 22, /* INDEX: an index smaller than the last one (06 §4.4) */
};
/* hdr.status of MTL_CQE_RX_UNIT */
enum mtl_rx_status {
  MTL_RX_STATUS_UNSET = 0,
  MTL_RX_COMPLETE = 1,
  MTL_RX_INCOMPLETE = 2,
  MTL_RX_DISCARDED = 3, /* later (dynamic provide) */
};
/* Index into mtl_rx_stats.pkts_rejected[]; every value < MTL_RX_REJECT_SLOTS */
enum mtl_rx_reject {
  MTL_RX_REJECT_PT = 0,
  MTL_RX_REJECT_SSRC = 1,
  MTL_RX_REJECT_LEN = 2,
  MTL_RX_REJECT_INTERLACE = 3,
  MTL_RX_REJECT_SEQ_OLD = 4,
  MTL_RX_REJECT_RTP_OUT_OF_RANGE = 5,
  MTL_RX_REJECT_NO_SLOT = 6,
  MTL_RX_REJECT_DMA_BUSY = 7,
  MTL_RX_REJECT_WRONG_PORT = 8,
  MTL_RX_REJECT_FMD_FILTER = 9,
  MTL_RX_REJECT_BEFORE_START = 10,
  MTL_RX_REJECT_OFFSET = 11,
  MTL_RX_REJECT_STALE = 12,
};

/* hdr.time_valid of MTL_CQE_TX_RESULT (06 §7.5) */
#define MTL_TT_MEDIA 0x1u
#define MTL_TT_SUBMITTED 0x2u
#define MTL_TT_DEADLINE 0x4u
#define MTL_TT_SCHEDULED 0x8u
#define MTL_TT_ENQUEUED 0x10u /* SW: TSC at the burst of packet 0, not wire time */
#define MTL_TT_OBSERVED_FIRST_LEG0 0x20u /* HW TX timestamp */
#define MTL_TT_OBSERVED_FIRST_LEG1 0x40u
#define MTL_TT_OBSERVED_FIRST(leg) (0x20u << (leg))
#define MTL_TT_OBSERVED_LAST 0x80u
#define MTL_TT_MARGIN 0x100u
#define MTL_TT_PICKUP_SLACK 0x200u
#define MTL_TT_SNAP_ERROR 0x400u
#define MTL_TT_MAX_PKT_LATENESS 0x800u
#define MTL_TV_ESTIMATED 0x40000000u /* TX and RX: the time base was ESTIMATED */
/* mtl_tx_timing.flags */
#define MTL_TX_TIMING_NON_COMPLIANT 0x1u /* EXACT launch */
#define MTL_TX_TIMING_SNAPPED 0x2u
#define MTL_TX_TIMING_RELOCKED 0x4u
#define MTL_TX_TIMING_DISCONTINUITY 0x8u
#define MTL_TX_TIMING_REPEATED 0x10u
#define MTL_TX_TIMING_RESLOTTED 0x20u

/* Fixed size. */
struct mtl_cq_entry_hdr {
  uint16_t kind;       /* enum mtl_cqe_kind */
  uint16_t size;       /* OUTPUT: the bytes the library wrote for this record (C4) */
  uint32_t status;     /* enum mtl_tx_status or mtl_rx_status; 0 = UNSET */
  uint32_t flags;      /* MTL_CQE_*, MTL_RX_F_* */
  uint32_t time_valid; /* one bit per int64 time field of this kind (C8) */
  mtl_session_h session;
  /* RX: the lease the app now holds. TX: the submitted lease, stale by now;
     mtl_lease_buffer() still gives its buffer. */
  mtl_lease_h lease;
  uint64_t user_cookie; /* TX: the submission's if set, else the buffer's */
  uint64_t seq;         /* per session, assigned at submit or delivery */
};
/* Fixed size; inside mtl_tx_result (06 §7.5). */
struct mtl_tx_timing {
  int64_t media_tai_ns; /* M after offsets and snapping */
  int64_t media_index;  /* on the session's timeline */
  int64_t submitted_tai_ns;
  int64_t deadline_tai_ns;        /* the latest pick-up that meets the slot */
  int64_t scheduled_first_tai_ns; /* TVD - VRX0 * TRS, or the ANC/audio target */
  int64_t enqueued_first_tai_ns;
  int64_t observed_first_tai_ns[MTL_MAX_LEGS];
  int64_t observed_last_tai_ns;
  int64_t margin_ns;              /* deadline - submitted */
  int64_t pickup_slack_ns;        /* deadline - pickup */
  int32_t snap_error_ns;          /* TAI mode */
  int32_t max_packet_lateness_ns; /* TX self-check against TPRj */
  uint32_t rtp;                   /* on the wire, first packet */
  uint32_t flags;                 /* MTL_TX_TIMING_* */
  uint32_t slots_skipped_before;
  uint32_t samples_padded;   /* audio: silence from carry or gap fill */
  uint32_t samples_dropped;  /* audio: overlap removed */
  uint32_t samples_inserted; /* audio: silence a re-phase added (06 §8) */
};
/* CQ record; at most MTL_CQ_RECORD_MAX. */
struct mtl_tx_result {
  struct mtl_cq_entry_hdr hdr;
  uint32_t reason; /* enum mtl_tx_reason */
  int32_t error;   /* MTL_E* for FAILED */
  struct mtl_tx_timing timing;
  uint32_t leg_count;
  uint32_t pkts_sent[MTL_MAX_LEGS];
  uint8_t leg_reason[MTL_MAX_LEGS]; /* per leg: NONE, LINK_DOWN, NO_NEIGHBOUR, ... */
  uint16_t reserved0;
  uint32_t path; /* enum mtl_path_kind used for this unit */
  uint32_t pkts_dma;
  uint32_t pkts_copied_partial;
  uint32_t reserved1;
};

/* hdr.time_valid of MTL_CQE_RX_UNIT (06 §11) */
#define MTL_RT_MEDIA 0x1u /* never for MTL_MEDIACLK_SENDER streams */
#define MTL_RT_INDEX 0x2u
#define MTL_RT_ARRIVAL_FIRST_LEG0 0x4u
#define MTL_RT_ARRIVAL_FIRST_LEG1 0x8u
#define MTL_RT_ARRIVAL_FIRST(leg) (0x4u << (leg))
#define MTL_RT_ARRIVAL_LAST_LEG0 0x10u
#define MTL_RT_ARRIVAL_LAST_LEG1 0x20u
#define MTL_RT_PRESENTATION 0x40u
#define MTL_RT_DELIVERED 0x80u
/* mtl_rx_timing.flags */
#define MTL_RXT_HW_ARRIVAL 0x1u /* the arrival times are HW timestamps */
#define MTL_RXT_DISCONTINUITY 0x2u
#define MTL_RXT_LATE_FOR_PRESENTATION 0x4u
#define MTL_RXT_INCOMPLETE_BY_DUE 0x8u

/* Fixed size; inside mtl_rx_unit (06 §11). */
struct mtl_rx_timing {
  int64_t media_tai_ns;
  int64_t media_index; /* exact inverse of the TX RTP rule (06 §11.2); EPOCH or AT_TAI */
  int64_t arrival_first_tai_ns[MTL_MAX_LEGS];
  int64_t arrival_last_tai_ns[MTL_MAX_LEGS];
  int64_t presentation_tai_ns;   /* media + link_offset */
  int64_t delivered_tai_ns;      /* when the unit became READY */
  uint32_t rtp;                  /* raw RTP */
  uint32_t flags;                /* MTL_RXT_* */
  int32_t media_phase_ticks;     /* 06 §11.2 */
  uint32_t units_missing_before; /* stream gap by index difference (06 §11.5) */
};
/* Fixed size. */
struct mtl_rx_missing_range {
  uint32_t first_pkt;
  uint32_t pkt_count;
};
/* CQ record; at most MTL_CQ_RECORD_MAX. */
struct mtl_rx_unit {
  struct mtl_cq_entry_hdr hdr;
  struct mtl_rx_timing timing;
  uint32_t leg_count;
  uint32_t pkts_expected;
  uint32_t pkts_received[MTL_MAX_LEGS];
  uint32_t pkts_recovered;      /* filled by the other leg */
  uint32_t units_missed_before; /* pool could not receive since the previous delivery */
  uint32_t units_before_start;  /* discarded while ARMED since the previous delivery */
  uint32_t valid_bytes;         /* audio, ANC, fastmeta, cvideo */
  uint32_t sample_count;        /* audio */
  uint32_t ready_rows;          /* progressive */
  uint32_t format_changed;      /* detected-property bitmask */
  uint32_t meta_bytes;          /* sender user meta or ANC packet table */
  uint32_t anc_count;
  uint32_t missing_count; /* above MTL_RX_MISSING_RANGES: RX_MISSING follow-ons */
  uint32_t path;          /* enum mtl_path_kind */
  uint32_t pkts_dma;
  uint32_t pkts_copied_partial;
  uint32_t user_meta_type; /* MTL_META_UNTAGGED or the sender's tag */
  uint32_t user_meta_version;
  uint32_t reserved0;
  struct mtl_rx_missing_range missing[MTL_RX_MISSING_RANGES];
};
/* CQ record: hdr.lease = the unit's lease; FINAL on the last one; hdr.time_valid 0x1 =
   ready_tai_ns. */
struct mtl_rx_progress {
  struct mtl_cq_entry_hdr hdr;
  uint32_t ready_rows; /* rows [0, ready_rows) are complete */
  uint32_t reserved0;
  int64_t ready_tai_ns;
};
/* CQ record: hdr.flags has MTL_CQE_MORE, the TX_RESULT follows; hdr.time_valid 0x1 =
   released_tai_ns. */
struct mtl_tx_source_released {
  struct mtl_cq_entry_hdr hdr;
  int64_t released_tai_ns;
};
/* CQ follow-on record: the same seq as its RX_UNIT; MORE if another one follows. */
struct mtl_rx_missing {
  struct mtl_cq_entry_hdr hdr;
  uint32_t first_range; /* index of ranges[0] in the unit's missing list */
  uint32_t range_count;
  struct mtl_rx_missing_range ranges[MTL_RX_MISSING_EXT];
};
/* Exactly MTL_CQ_ENTRY_SIZE. */
union mtl_cq_entry {
  struct mtl_cq_entry_hdr hdr;
  struct mtl_tx_result tx;
  struct mtl_rx_unit rx;
  struct mtl_rx_progress progress;
  struct mtl_tx_source_released released;
  struct mtl_rx_missing missing;
  uint8_t raw[MTL_CQ_ENTRY_SIZE];
};

/* ---- 16. TX data path (03 §4.1, 06; since 0.1) ----------------------------------- */

enum mtl_launch_mode {
  MTL_LAUNCH_DERIVED = 0,
  MTL_LAUNCH_NOT_BEFORE = 1,
  MTL_LAUNCH_EXACT = 2, /* non-compliant: MTL_TX_TIMING_NON_COMPLIANT */
};
/* Value type. */
struct mtl_launch {
  uint32_t mode; /* enum mtl_launch_mode */
  uint32_t reserved;
  int64_t tai_ns;
};
#define MTL_SUB_RTP 0x1u              /* submission.valid: rtp_override is set */
#define MTL_SUBMIT_DISCONTINUITY 0x1u /* submission.flags */
#define MTL_META_UNTAGGED 0u          /* user_meta_type: no tag */

/* NULL means all defaults. */
struct mtl_tx_submission {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t valid;       /* MTL_SUB_* for the optional plain values */
  int64_t media_index;  /* media_mode INDEX */
  int64_t media_tai_ns; /* media_mode TAI: absolute TAI on any timeline */
  struct mtl_launch launch;
  uint32_t rtp_override;   /* MTL_SUB_RTP; PASSTHROUGH sessions only */
  uint32_t sample_count;   /* audio: authoritative */
  uint32_t valid_bytes;    /* ANC, fastmeta, cvideo */
  uint32_t ready_rows;     /* progressive */
  uint64_t flags;          /* MTL_SUBMIT_* */
  uint64_t user_cookie;    /* non-zero with MTL_COMPLETE_NONE: -MTL_EINVAL */
  mtl_lease_h hold;        /* an RX lease held until this unit's result (1 to N) */
  const void* user_meta;   /* copied at submit */
  uint32_t user_meta_size; /* <= max_user_meta_bytes */
  uint32_t user_meta_type; /* MTL_META_UNTAGGED or an app tag */
  uint32_t user_meta_version;
  uint32_t anc_count;               /* <= 255 */
  const struct mtl_anc_packet* anc; /* ANC packet table, copied at submit */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_tx_submission_init(struct mtl_tx_submission* p);

/* Output of acquire: where the next unit lands (06 §7.6). */
struct mtl_slot_hint {
  uint32_t struct_size;
  uint32_t queued; /* units queued ahead */
  /* the admission test: the smallest k > last submitted whose deadline is ahead */
  int64_t next_media_index;
  int64_t next_media_tai_ns;      /* M(next_media_index) */
  int64_t submit_deadline_tai_ns; /* submit before this */
};
MTL_API_DP void mtl_slot_hint_init(struct mtl_slot_hint* p);

/* view and hint may be NULL (cache views by buffer index). 0, or -MTL_EAGAIN,
   -MTL_ETIMEDOUT, -MTL_ECANCELED, -MTL_ESHUTDOWN, -MTL_EIO, ... */
MTL_API_WT int mtl_tx_acquire(mtl_session_h s, mtl_lease_h* lease,
                              struct mtl_buffer_view* MTL_NULLABLE view,
                              struct mtl_slot_hint* MTL_NULLABLE hint,
                              int64_t timeout_ns);
/* That named attached buffer; -MTL_EBUSY if it is not FREE. */
MTL_API_WT int mtl_tx_acquire_buffer(mtl_session_h s, mtl_buffer_h base,
                                     mtl_lease_h* lease,
                                     struct mtl_buffer_view* MTL_NULLABLE view,
                                     struct mtl_slot_hint* MTL_NULLABLE hint,
                                     int64_t timeout_ns);
/* MTL_POOL_DYNAMIC (Phase 4): a free slot bound to a caller layout in an imported
   region. */
MTL_API_WT int mtl_tx_acquire_dynamic(mtl_session_h s,
                                      const struct mtl_buffer_desc* layout,
                                      mtl_lease_h* lease,
                                      struct mtl_buffer_view* MTL_NULLABLE view,
                                      int64_t timeout_ns);
/* On success access moves to MTL and exactly one terminal outcome follows. On failure
   the lease stays the app's: release it, or fix and resubmit. For MTL_UNIT_ROWS the
   lease stays current for mtl_tx_publish() until the FINAL publish. */
MTL_API_DPC int mtl_tx_submit(mtl_session_h s, mtl_lease_h lease,
                              const struct mtl_tx_submission* MTL_NULLABLE sub);
/* ready_rows == rows is FINAL. */
MTL_API_DP int mtl_tx_publish(mtl_session_h s, mtl_lease_h lease, uint32_t ready_rows);
/* Return an acquired, unsubmitted lease; no result is produced. */
MTL_API_DP int mtl_tx_release(mtl_session_h s, mtl_lease_h lease);
/* Pull buffer b's queued, not yet picked-up unit: FLUSHED/WITHDRAWN; -MTL_EBUSY if
   already picked up. */
MTL_API_DP int mtl_tx_withdraw(mtl_session_h s, mtl_buffer_h b);
/* Array read of this session's TX results (C4). */
MTL_API_WT int mtl_tx_reap(mtl_session_h s, void* res, size_t res_size, uint32_t max,
                           int64_t timeout_ns);
/* ST30/40/41 copy path: bytes are cut into packets from the submission's media time. */
MTL_API_WT int mtl_tx_write(mtl_session_h s, const void* data, size_t bytes,
                            const struct mtl_tx_submission* MTL_NULLABLE sub,
                            int64_t timeout_ns);
/* L4: the latest submit time of `row` for the slot in `hint` (progressive producers). */
MTL_API_DP int mtl_session_row_deadline(mtl_session_h s, const struct mtl_slot_hint* hint,
                                        uint32_t row, struct mtl_time* t);

/* ---- 17. RX data path (03 §4.2; since 0.1) --------------------------------------- */

/* view and unit may be NULL; unit_size may be sizeof(struct mtl_cq_entry_hdr) for a
   header-only fill. In CREATED, ARMED or STOPPED with nothing READY: -MTL_EAGAIN or
   -MTL_ETIMEDOUT, never -MTL_ESHUTDOWN (which is app-initiated shutdown only). */
MTL_API_WT int mtl_rx_dequeue(mtl_session_h s, mtl_lease_h* lease,
                              struct mtl_buffer_view* MTL_NULLABLE view,
                              struct mtl_rx_unit* MTL_NULLABLE unit, size_t unit_size,
                              int64_t timeout_ns);
/* Any thread, any order. The slot is FREE once released AND its hold count is 0. */
MTL_API_DP int mtl_rx_release(mtl_session_h s, mtl_lease_h lease);
MTL_API_WT int mtl_rx_wait_progress(mtl_session_h s, mtl_lease_h lease,
                                    uint32_t* ready_rows, int64_t timeout_ns);
/* Sugar for 1 -> 1 forwarding: leases the TX session's attached buffer whose planes equal
   the RX buffer's, with an implicit hold on the RX lease (05 §5.4); -MTL_EINVAL,
   MTL_REASON_HOLD_MISMATCH, if there is none. */
MTL_API_DP int mtl_rx_transfer(mtl_session_h rx, mtl_lease_h lease, mtl_session_h tx,
                               mtl_lease_h* tx_lease);
/* L4, receiver-side A/V alignment (06 §11.6): the offset of the sample contemporaneous
   with the video unit into the audio unit. 0 = exact (both indices valid, phase 0),
   1 = approximate (from media_tai_ns, +-1 sample), -MTL_EINVAL for SENDER streams. */
MTL_API_DP int mtl_rx_align(const struct mtl_rx_unit* video,
                            const struct mtl_rx_unit* audio, int64_t* sample_offset);

/* ---- 18. Completion queues (07 §2; since 0.1) ------------------------------------ */

struct mtl_cq_config {
  uint32_t struct_size;
  uint32_t reserved0;
  uint64_t flags; /* reserved, must be 0 */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_cq_config_init(struct mtl_cq_config* p);
MTL_API_CP int mtl_cq_create(mtl_instance_h mt, const struct mtl_cq_config* c,
                             mtl_cq_h* out);
/* -MTL_EBUSY while sessions are bound. */
MTL_API_CP int mtl_cq_destroy(mtl_cq_h cq);
/* Array read of any record kind (C4): count >= 0; 0 = empty, with timeout 0 only. A read
   with a timeout is the wait; there is no separate cq wait. */
MTL_API_WT int mtl_cq_read(mtl_cq_h cq, void* buf, size_t record_size, uint32_t max,
                           int64_t timeout_ns);
MTL_API_CP int mtl_cq_get_wait_object(mtl_cq_h cq, struct mtl_wait_object* out);
/* 1 / 0 / < 0 (C1). */
MTL_API_DP int mtl_cq_trywait(mtl_cq_h cq);
MTL_API_AS int mtl_cq_interrupt(mtl_cq_h cq);
MTL_API_CP int mtl_cq_uninterrupt(mtl_cq_h cq);
/* L4 dispatcher (Q-CMP-2): a library thread reads the CQ and calls fn per entry. */
typedef void (*mtl_cq_dispatch_fn)(void* user, const union mtl_cq_entry* e);
MTL_API_CP int mtl_cq_dispatch_start(mtl_cq_h cq, mtl_cq_dispatch_fn fn, void* user,
                                     void** handle);
MTL_API_CP int mtl_cq_dispatch_stop(void* handle);

/* ---- 19. Event queues (07 §3; since 0.1) ----------------------------------------- */

enum mtl_event_type {
  MTL_EVENT_UNSET = 0,
  MTL_EVENT_TIME_STATE = 1,
  MTL_EVENT_TIME_STEP = 2,
  MTL_EVENT_PORT_LINK = 3,
  MTL_EVENT_PORT_RESET = 4,
  MTL_EVENT_SESSION_STATE = 5,
  MTL_EVENT_SESSION_RETIRED = 6, /* session EQ + every EQ with MTL_EQ_SUB_SESSION */
  MTL_EVENT_SESSION_RECOVERY = 7,
  MTL_EVENT_LEG_STATE = 8, /* admin x oper */
  MTL_EVENT_FLOW_STATE = 9,
  MTL_EVENT_RX_SIGNAL = 10,
  MTL_EVENT_RX_FORMAT = 11,
  MTL_EVENT_RX_TIMEBASE_SUSPECT = 12,
  MTL_EVENT_RX_DISCONTINUITY = 13, /* relock: old and new RTP */
  MTL_EVENT_PACING_CHANGED = 14,
  MTL_EVENT_TIMING_INFEASIBLE = 15, /* getter: get_status().timing_warning */
  MTL_EVENT_BACKPRESSURE = 16,
  MTL_EVENT_TX_UNDERRUN = 17,
  MTL_EVENT_SCHED_OVERLOAD = 18,
  MTL_EVENT_SESSION_MIGRATED = 19,
  MTL_EVENT_INLINE_HOOK_DISABLED = 20,
  MTL_EVENT_MANAGER_LOST = 21,
  MTL_EVENT_GROUP_MEMBER_FAILED = 22,
  MTL_EVENT_REGION_RELEASED = 23,
  MTL_EVENT_EPOCH_TICK = 24, /* lossy, opt-in MTL_EQ_SUB_EPOCH_TICK; was VSYNC */
  MTL_EVENT_OVERFLOW = 25,
  MTL_EVENT_USER = 26, /* mtl_eq_post; own ring, never evicts library notices */
};
enum mtl_severity {
  MTL_SEV_INFO = 0,
  MTL_SEV_WARNING = 1,
  MTL_SEV_ERROR = 2,
  MTL_SEV_FATAL = 3,
};
enum mtl_origin_kind {
  MTL_ORIGIN_UNSET = 0,
  MTL_ORIGIN_INSTANCE = 1,
  MTL_ORIGIN_PORT = 2,
  MTL_ORIGIN_SESSION = 3,
  MTL_ORIGIN_GROUP = 4,
  MTL_ORIGIN_REGION = 5,
  MTL_ORIGIN_TIMELINE = 6,
  MTL_ORIGIN_EQ = 7,
  MTL_ORIGIN_APP = 8,
};
/* OVERFLOW payload (07 §3.3) */
enum mtl_producer_class {
  MTL_PRODUCER_UNSET = 0,
  MTL_PRODUCER_TASKLET = 1,
  MTL_PRODUCER_LIBRARY = 2,
  MTL_PRODUCER_APP = 3,
  MTL_PRODUCER_USER = 4,
};
/* EQ subscription mask (mtl_eq_config.mask, mtl_eq_subscribe) */
#define MTL_EQ_SUB_PORT 0x1u        /* PORT_LINK, PORT_RESET */
#define MTL_EQ_SUB_TIME 0x2u        /* TIME_STATE, TIME_STEP */
#define MTL_EQ_SUB_INSTANCE 0x4u    /* MANAGER_LOST, SCHED_OVERLOAD, REGION_RELEASED */
#define MTL_EQ_SUB_SESSION 0x8u     /* every session event, SESSION_RETIRED included */
#define MTL_EQ_SUB_EPOCH_TICK 0x10u /* EPOCH_TICK of sessions that enable it */
#define MTL_EQ_SUB_USER 0x20u       /* USER posts to this EQ */

/* Typed payloads. */
struct mtl_event_state {
  uint32_t old_state;
  uint32_t new_state;
  uint32_t leg;
  uint32_t reserved;
};
struct mtl_event_port {
  uint32_t port;
  uint32_t up;
  uint32_t speed_mbps;
  uint32_t reserved;
};
struct mtl_event_time_step {
  int64_t step_ns;
  uint32_t policy;
  uint32_t reserved;
};
struct mtl_event_flow {
  uint32_t leg;
  uint32_t state; /* FLOW_STATE: enum mtl_flow_state; LEG_STATE: enum mtl_leg_oper */
  uint32_t admin; /* enum mtl_leg_admin */
  uint32_t reserved;
  int64_t
      first_index; /* update_flows: first unit on the new flow (TX media index, RX RTP) */
};
struct mtl_event_timing {
  int64_t shortfall_ns;
  int64_t suggested_min_tx_delay_ns;
};
struct mtl_event_overflow {
  uint64_t lost;
  uint32_t producer_class; /* enum mtl_producer_class */
  uint32_t reserved;
};
struct mtl_event_backpressure {
  uint32_t begin;
  uint32_t blocked_on;
};
struct mtl_event_epoch_tick {
  int64_t media_index;
  int64_t tai_ns;
};
struct mtl_event_rx_format {
  uint32_t changed; /* bitmask */
  uint32_t width;
  uint32_t height;
  uint32_t interlaced;
  struct mtl_rational fps;
};
struct mtl_event_rx_discontinuity {
  uint32_t old_rtp;
  uint32_t new_rtp;
};
struct mtl_event_recovery {
  uint32_t begin;
  uint32_t units_dropped;
};
union mtl_event_payload {
  struct mtl_event_state state;
  struct mtl_event_port port;
  struct mtl_event_time_step time_step;
  struct mtl_event_flow flow;
  struct mtl_event_timing timing;
  struct mtl_event_overflow overflow;
  struct mtl_event_backpressure backpressure;
  struct mtl_event_epoch_tick epoch_tick;
  struct mtl_event_rx_format rx_format;
  struct mtl_event_rx_discontinuity rx_discontinuity;
  struct mtl_event_recovery recovery;
  uint8_t user[32];
  uint8_t raw[MTL_EVENT_PAYLOAD];
};
/* EQ record; size is an OUTPUT (C4). */
struct mtl_event {
  uint16_t size;        /* bytes written */
  uint16_t severity;    /* enum mtl_severity */
  uint32_t type;        /* enum mtl_event_type */
  uint32_t coalesced;   /* occurrences this record stands for, >= 1 */
  uint32_t reason;      /* enum mtl_state_reason, NONE if not applicable */
  uint64_t seq;         /* per EQ; a gap means overflow */
  int64_t time_tai_ns;  /* of the (last) occurrence */
  uint32_t time_valid;  /* 0x1: time_tai_ns */
  uint32_t origin_kind; /* enum mtl_origin_kind */
  uint64_t origin;      /* the origin's handle id, or the port index */
  /* the origin's name (session, port, group, timeline), readable after retirement */
  char origin_name[MTL_NAME_MAX];
  uint32_t producer_class; /* OVERFLOW: enum mtl_producer_class */
  uint32_t reserved;
  union mtl_event_payload u;
};
struct mtl_eq_config {
  uint32_t struct_size;
  uint32_t capacity; /* per producer ring; 0 = 64 records (07 §2.1) */
  uint64_t mask;     /* MTL_EQ_SUB_*, instance-wide subscriptions */
  uint64_t reserved[4];
};
MTL_API_DP void mtl_eq_config_init(struct mtl_eq_config* p);
MTL_API_CP int mtl_eq_create(mtl_instance_h mt, const struct mtl_eq_config* c,
                             mtl_eq_h* out);
/* Drops a reference (private EQs are refcounted, see mtl_session_get_eq). */
MTL_API_CP int mtl_eq_destroy(mtl_eq_h eq);
/* Subscribe to one object's events (a session, a port, a group); mask 0 unsubscribes.
   Each subscribed EQ gets its own copy (07 §3.4). */
MTL_API_CP int mtl_eq_subscribe(mtl_eq_h eq, struct mtl_object obj, uint64_t mask);
/* The instance EQ: PORT, TIME and INSTANCE events, and SESSION events of its sessions
   (SESSION_RETIRED included, 07 §3.4). */
MTL_API_DP int mtl_instance_get_eq(mtl_instance_h mt, mtl_eq_h* eq);
/* Array read (C4); a read with a timeout is the wait. */
MTL_API_WT int mtl_eq_read(mtl_eq_h eq, struct mtl_event* ev, size_t ev_size,
                           uint32_t max, int64_t timeout_ns);
MTL_API_CP int mtl_eq_get_wait_object(mtl_eq_h eq, struct mtl_wait_object* out);
/* 1 / 0 / < 0 (C1). */
MTL_API_DP int mtl_eq_trywait(mtl_eq_h eq);
MTL_API_AS int mtl_eq_interrupt(mtl_eq_h eq);
MTL_API_CP int mtl_eq_uninterrupt(mtl_eq_h eq);
/* Type USER. */
MTL_API_DP int mtl_eq_post(mtl_eq_h eq, const struct mtl_event* ev);

/* ---- 20. C value initialisers (C only; C++ and bindings call *_init) ------------ */

#if !defined(__cplusplus)
#define MTL_STRUCT_INIT_(type, ...) \
  ((struct type){.struct_size = sizeof(struct type), __VA_ARGS__})
#define MTL_INSTANCE_PARAMS_INIT(...) MTL_STRUCT_INIT_(mtl_instance_params, __VA_ARGS__)
#define MTL_SESSION_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_session_config, __VA_ARGS__)
#define MTL_VIDEO_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_video_config, __VA_ARGS__)
#define MTL_CVIDEO_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_cvideo_config, __VA_ARGS__)
#define MTL_AUDIO_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_audio_config, __VA_ARGS__)
#define MTL_ANC_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_anc_config, __VA_ARGS__)
#define MTL_FASTMETA_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_fastmeta_config, __VA_ARGS__)
#define MTL_MEM_DESC_INIT(...) MTL_STRUCT_INIT_(mtl_mem_desc, __VA_ARGS__)
#define MTL_BUFFER_DESC_INIT(...) MTL_STRUCT_INIT_(mtl_buffer_desc, __VA_ARGS__)
#define MTL_TIMELINE_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_timeline_config, __VA_ARGS__)
#define MTL_GROUP_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_group_config, __VA_ARGS__)
#define MTL_START_PARAMS_INIT(...) MTL_STRUCT_INIT_(mtl_start_params, __VA_ARGS__)
#define MTL_DISCARD_PARAMS_INIT(...) MTL_STRUCT_INIT_(mtl_discard_params, __VA_ARGS__)
#define MTL_RECONFIGURE_PARAMS_INIT(...) \
  MTL_STRUCT_INIT_(mtl_reconfigure_params, __VA_ARGS__)
#define MTL_ACTIVATION_INIT(...) MTL_STRUCT_INIT_(mtl_activation, __VA_ARGS__)
#define MTL_TX_SUBMISSION_INIT(...) MTL_STRUCT_INIT_(mtl_tx_submission, __VA_ARGS__)
#define MTL_CQ_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_cq_config, __VA_ARGS__)
#define MTL_EQ_CONFIG_INIT(...) MTL_STRUCT_INIT_(mtl_eq_config, __VA_ARGS__)
#define MTL_BUFFER_VIEW_INIT() MTL_STRUCT_INIT_(mtl_buffer_view, .plane_count = 0)
#define MTL_SLOT_HINT_INIT() MTL_STRUCT_INIT_(mtl_slot_hint, .queued = 0)
#define MTL_BUFFER_REQUIREMENTS_INIT() \
  MTL_STRUCT_INIT_(mtl_buffer_requirements, .plane_count = 0)
#define MTL_SESSION_INFO_INIT() MTL_STRUCT_INIT_(mtl_session_info, .direction = 0)
#define MTL_SESSION_STATUS_INIT() MTL_STRUCT_INIT_(mtl_session_status, .state = 0)
#define MTL_ERROR_INFO_INIT() MTL_STRUCT_INIT_(mtl_error_info, .code = 0)
#endif

/* ---- 21. Size checks (A1): every public struct and union, 64-bit targets --------- */

MTL_SIZE_CHECK(mtl_instance_h, 8);
MTL_SIZE_CHECK(mtl_session_h, 8);
MTL_SIZE_CHECK(mtl_buffer_h, 8);
MTL_SIZE_CHECK(mtl_lease_h, 8);
MTL_SIZE_CHECK(mtl_region_h, 8);
MTL_SIZE_CHECK(mtl_cq_h, 8);
MTL_SIZE_CHECK(mtl_eq_h, 8);
MTL_SIZE_CHECK(mtl_timeline_h, 8);
MTL_SIZE_CHECK(mtl_group_h, 8);
MTL_SIZE_CHECK(mtl_object, 16);
MTL_SIZE_CHECK(mtl_error_info, 152);
MTL_SIZE_CHECK(mtl_wait_object, 16);
MTL_SIZE_CHECK(mtl_rational, 16);
MTL_SIZE_CHECK(mtl_time, 16);
MTL_SIZE_CHECK(mtl_time_status, 96);
MTL_SIZE_CHECK(mtl_port_spec, 148);
MTL_SIZE_CHECK(mtl_instance_params, 1496);
MTL_SIZE_CHECK(mtl_instance_info, 72);
MTL_SIZE_CHECK(mtl_instance_status, 56);
MTL_SIZE_CHECK(mtl_mem_status, 104);
MTL_SIZE_CHECK(mtl_port_caps, 176);
MTL_SIZE_CHECK(mtl_port_status, 136);
MTL_SIZE_CHECK(mtl_port_capacity, 64);
MTL_SIZE_CHECK(mtl_sched_status, 96);
MTL_SIZE_CHECK(mtl_port_params, 192);
MTL_SIZE_CHECK(mtl_mem_desc, 88);
MTL_SIZE_CHECK(mtl_mem_info, 80);
MTL_SIZE_CHECK(mtl_plane_desc, 48);
MTL_SIZE_CHECK(mtl_buffer_desc, 248);
MTL_SIZE_CHECK(mtl_plane_view, 24);
MTL_SIZE_CHECK(mtl_buffer_view, 144);
MTL_SIZE_CHECK(mtl_pool_config, 48);
MTL_SIZE_CHECK(mtl_plane_requirements, 32);
MTL_SIZE_CHECK(mtl_buffer_requirements, 216);
MTL_SIZE_CHECK(mtl_video_config, 128);
MTL_SIZE_CHECK(mtl_cvideo_config, 144);
MTL_SIZE_CHECK(mtl_audio_config, 120);
MTL_SIZE_CHECK(mtl_anc_config, 128);
MTL_SIZE_CHECK(mtl_fastmeta_config, 112);
MTL_SIZE_CHECK(mtl_anc_packet, 16);
MTL_SIZE_CHECK(mtl_format_pair, 16);
MTL_SIZE_CHECK(mtl_flow, 104);
MTL_SIZE_CHECK(mtl_timing_config, 240);
MTL_SIZE_CHECK(mtl_completion_config, 32);
MTL_SIZE_CHECK(mtl_capability_request, 32);
MTL_SIZE_CHECK(mtl_session_options, 64);
MTL_SIZE_CHECK(mtl_session_config, 792);
MTL_SIZE_CHECK(mtl_leg_info, 28);
MTL_SIZE_CHECK(mtl_ts_refclk, 16);
MTL_SIZE_CHECK(mtl_session_info, 512);
MTL_SIZE_CHECK(mtl_leg_status, 32);
MTL_SIZE_CHECK(mtl_timing_warning, 24);
MTL_SIZE_CHECK(mtl_session_status, 200);
MTL_SIZE_CHECK(mtl_histogram, 160);
MTL_SIZE_CHECK(mtl_window_max, 16);
MTL_SIZE_CHECK(mtl_tx_stats, 1264);
MTL_SIZE_CHECK(mtl_rx_stats, 896);
MTL_USIZE_CHECK(mtl_dir_stats, 1264);
MTL_SIZE_CHECK(mtl_leg_stats, 112);
MTL_SIZE_CHECK(mtl_queue_gauges, 296);
MTL_SIZE_CHECK(mtl_video_stats, 128);
MTL_SIZE_CHECK(mtl_cvideo_stats, 128);
MTL_SIZE_CHECK(mtl_audio_stats, 128);
MTL_SIZE_CHECK(mtl_anc_stats, 128);
MTL_SIZE_CHECK(mtl_fastmeta_stats, 128);
MTL_USIZE_CHECK(mtl_media_stats, 128);
MTL_SIZE_CHECK(mtl_session_stats, 2096);
MTL_SIZE_CHECK(mtl_stat_desc, 64);
MTL_SIZE_CHECK(mtl_option_desc, 72);
MTL_SIZE_CHECK(mtl_timeline_config, 80);
MTL_SIZE_CHECK(mtl_timeline_info, 96);
MTL_SIZE_CHECK(mtl_group_config, 48);
MTL_SIZE_CHECK(mtl_start_params, 64);
MTL_SIZE_CHECK(mtl_discard_params, 56);
MTL_SIZE_CHECK(mtl_reconfigure_params, 64);
MTL_SIZE_CHECK(mtl_activation, 40);
MTL_SIZE_CHECK(mtl_cq_entry_hdr, 48);
MTL_SIZE_CHECK(mtl_tx_timing, 120);
MTL_SIZE_CHECK(mtl_tx_result, 208);
MTL_SIZE_CHECK(mtl_rx_timing, 80);
MTL_SIZE_CHECK(mtl_rx_missing_range, 8);
MTL_SIZE_CHECK(mtl_rx_unit, 240);
MTL_SIZE_CHECK(mtl_rx_progress, 64);
MTL_SIZE_CHECK(mtl_tx_source_released, 56);
MTL_SIZE_CHECK(mtl_rx_missing, 240);
MTL_USIZE_CHECK(mtl_cq_entry, MTL_CQ_ENTRY_SIZE);
MTL_SIZE_CHECK(mtl_launch, 16);
MTL_SIZE_CHECK(mtl_tx_submission, 152);
MTL_SIZE_CHECK(mtl_slot_hint, 32);
MTL_SIZE_CHECK(mtl_cq_config, 48);
MTL_SIZE_CHECK(mtl_event_state, 16);
MTL_SIZE_CHECK(mtl_event_port, 16);
MTL_SIZE_CHECK(mtl_event_time_step, 16);
MTL_SIZE_CHECK(mtl_event_flow, 24);
MTL_SIZE_CHECK(mtl_event_timing, 16);
MTL_SIZE_CHECK(mtl_event_overflow, 16);
MTL_SIZE_CHECK(mtl_event_backpressure, 8);
MTL_SIZE_CHECK(mtl_event_epoch_tick, 16);
MTL_SIZE_CHECK(mtl_event_rx_format, 32);
MTL_SIZE_CHECK(mtl_event_rx_discontinuity, 8);
MTL_SIZE_CHECK(mtl_event_recovery, 8);
MTL_USIZE_CHECK(mtl_event_payload, MTL_EVENT_PAYLOAD);
MTL_SIZE_CHECK(mtl_event, 184);
MTL_SIZE_CHECK(mtl_eq_config, 48);
/* Every CQ record kind leaves >= 16 B of growth inside the entry. */
typedef char
    mtl_sz_cq_tx_fits[(sizeof(struct mtl_tx_result) <= MTL_CQ_RECORD_MAX) ? 1 : -1];
typedef char
    mtl_sz_cq_rx_fits[(sizeof(struct mtl_rx_unit) <= MTL_CQ_RECORD_MAX) ? 1 : -1];
typedef char
    mtl_sz_cq_miss_fits[(sizeof(struct mtl_rx_missing) <= MTL_CQ_RECORD_MAX) ? 1 : -1];

/* ---- 22. Named in the design, not in 0.1 (check.sh compiles MTL_UNIFIED_LATER) --- */

#if defined(MTL_UNIFIED_LATER)
/* 04 §6, Q-THR-1: opt-in inline notification on the completing thread (DP context). */
typedef void (*mtl_inline_notify_fn)(void* user, mtl_session_h s, uint32_t what);
MTL_API_CP int mtl_session_set_inline_notify(mtl_session_h s, mtl_inline_notify_fn fn,
                                             void* user);
/* 05 §2, Q-MEM-10: RX dynamic provide. */
MTL_API_DP int mtl_rx_provide(mtl_session_h s, mtl_buffer_h b);
/* 04 §7, Q-THR-3: manual progress. */
MTL_API_CP int mtl_sch_run_once(mtl_instance_h mt, uint32_t sched, int64_t budget_ns);
/* 11 §2.4, Q-ABI-3 (c): named, versioned backend extension tables. */
MTL_API_CP int mtl_open_ext(mtl_instance_h mt, const char* name, uint32_t version,
                            void* ops);
/* 03 §6.4, Q-LIFE-3: instance close that destroys children in dependency order. */
#define MTL_UNINIT_DESTROY_ALL 0x1u
MTL_API_CP int mtl_instance_close(mtl_instance_h mt, uint64_t flags);
#endif

#if defined(__cplusplus)
}
#endif

#endif /* MTL_EXPERIMENTAL_MTL_UNIFIED_H */
