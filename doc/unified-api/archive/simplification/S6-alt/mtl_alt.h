/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 *
 * mtl_alt.h - S6 minimal alternative to the unified MTL API (design sketch, nothing is
 * implemented). Explained in doc/unified-api/simplification/S6-minimal-alternative.md.
 *
 * R1 Returns: 0, or a count/mask >= 0, on success; a negative MTL_E* code on failure.
 * R2 Timeouts are the last argument, in ns: 0 = do not wait, MTL_FOREVER = no limit.
 *    Nothing ready with timeout 0 is -MTL_EAGAIN; an expired timeout is -MTL_ETIMEDOUT.
 * R3 mtl_h names any object: instance, stream, timeline, queue or region. Its kind is
 *    encoded in the id and checked on every call (-MTL_EBADF, like a file descriptor).
 *    id 0 is null; as an instance argument it means the process-wide default instance.
 *    A lease (mtl_lease_h) is the only access token and has its own C type; a buffer's
 *    identity is a plain pool index (mtl_unit.buf) that no access-moving call accepts.
 * R4 Configuration is keys: "key=value" strings set with mtl_set*() and read with
 *    mtl_get*(). Zero or absent is the default of every key. An unknown key is
 *    -MTL_ENOTSUP (feature test); a bad value is -MTL_EINVAL with the key named in
 *    mtl_last_error(). Keys are control plane only; the data path uses fixed structs.
 *    The key catalogue is in the S6 document and in the optional mtl_keys.h.
 * R5 Data structs have frozen sizes (size checks below); reserved fields must be zero;
 *    a zero-filled struct is the default. Growth goes through the mtl_unit.ext chain.
 * R6 Call classes: CP control plane (may allocate and block); DP data plane (O(1), no
 *    allocation, syscall or lock a tasklet takes); WT wait (DP when timeout is 0); AS
 *    async-signal-safe. No call runs on an MTL tasklet and the library never calls
 *    application code. From a library busy-loop thread only DP calls with timeout 0
 *    are legal; anything else returns -MTL_EDEADLK.
 */

#ifndef MTL_ALT_H
#define MTL_ALT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define MTL_API __declspec(dllimport)
#elif defined(__GNUC__)
#define MTL_API __attribute__((visibility("default")))
#else
#define MTL_API
#endif

#define MTL_FOREVER ((int64_t)-1)
#define MTL_US(x) ((int64_t)(x) * 1000)
#define MTL_MS(x) ((int64_t)(x) * 1000000)
#define MTL_SEC(x) ((int64_t)(x) * 1000000000)
#define MTL_MAX_PLANES 4

/* ---- Errors: Linux errno values on every OS, one meaning each ------------------- */

#define MTL_EIO 5         /* the object failed (ERROR); "status.reason" says why */
#define MTL_EBADF 9       /* null, destroyed or wrong-kind handle */
#define MTL_EAGAIN 11     /* nothing now (timeout 0) */
#define MTL_ENOMEM 12     /* allocation failed */
#define MTL_EBUSY 16      /* in use, or not allowed in this state */
#define MTL_EEXIST 17     /* name in use with another configuration */
#define MTL_ENODEV 19     /* device gone or reset */
#define MTL_EINVAL 22     /* invalid argument or key value; see mtl_last_error() */
#define MTL_ENOSPC 28     /* capacity: queues, lcores, regions, payload size */
#define MTL_ERANGE 34     /* time outside the accepted window */
#define MTL_EDEADLK 35    /* not allowed from a library busy-loop thread */
#define MTL_ENOTSUP 95    /* unknown key or absent capability */
#define MTL_ESHUTDOWN 108 /* the app stopped or closed the object */
#define MTL_ETIMEDOUT 110 /* the timeout expired */
#define MTL_ESTALE 116    /* a lease that was already returned */
#define MTL_ECANCELED 125 /* waiters interrupted; sticky until mtl_interrupt(h, 0) */

/* ---- Handles -------------------------------------------------------------------- */

typedef struct mtl_h {
  uint64_t id; /* | kind:8 | reserved:8 | index:16 | generation:32 | */
} mtl_h;
typedef struct mtl_lease_h {
  uint64_t id; /* | stream index:16 | slot:16 | generation:32 | */
} mtl_lease_h;
#if defined(__cplusplus)
#define MTL_DEFAULT (mtl_h{0})
#else
#define MTL_DEFAULT ((mtl_h){0})
#endif

/* ---- Units: one struct for every essence, both directions and every use ---------- */

/* One plane of a unit. Frame units: rows of pixels or bytes. Packet units: plane 0 is
   `rows` packet slots of `stride` bytes (RTP header + payload, no UDP/IP), plane 1 is
   `rows` uint16_t packet lengths, optional plane 2 is `rows` int64_t TAI arrival times
   (RX). With MTL_U_SCATTER plane 0 holds `rows` packet pointers instead of slots. */
struct mtl_plane {
  void* addr;         /* NULL in a layout query */
  uint32_t stride;    /* bytes between rows (or packet slots); >= row_bytes */
  uint32_t row_bytes; /* valid bytes per row (packet units: slot capacity) */
  uint32_t rows;      /* rows (packet units: packets in this unit) */
  uint32_t reserved;
};

enum mtl_time_mode { /* how a TX unit names its media time (input of mtl_put) */
                     MTL_TIME_AUTO = 0,  /* the next slot; get() suggests it in `index` */
                     MTL_TIME_INDEX = 1, /* `index` on the stream's timeline: exact RTP */
                     MTL_TIME_TAI = 2,   /* `media_ns` in TAI, snapped to the grid */
};

enum mtl_status { /* results (TX) and deliveries (RX); 0 is never terminal */
                  MTL_STATUS_NONE = 0,
                  MTL_ON_TIME = 1,
                  MTL_LATE = 2,    /* sent late within late_tolerance_ns */
                  MTL_DROPPED = 3, /* not sent; `reason` says why, its slot stays empty */
                  MTL_FLUSHED = 4, /* stop(FLUSH), stop(DISCARD) or close */
                  MTL_FAILED = 5,  /* device or queue failure; `reason` */
                  MTL_COMPLETE = 6,
                  MTL_INCOMPLETE = 7, /* RX: pkts_lost > 0; missing data reads as zero
                                         (library pools) */
};

/* mtl_unit.flags: inputs (put, write) */
#define MTL_U_DISCONTINUITY 0x1u /* media time jumps; a new time_mode is allowed */
#define MTL_U_REBASE 0x2u        /* map `index` to the next feasible slot (seek) */
#define MTL_U_PARTIAL 0x4u /* more follows: rows (same lease) or packet chunks (next) */
#define MTL_U_LAUNCH_NOT_BEFORE                                                    \
  0x8u                           /* user pacing: first packet not before launch_ns \
                                  */
#define MTL_U_LAUNCH_EXACT 0x10u /* first packet at launch_ns (flagged non-compliant) */
#define MTL_U_RTP 0x20u          /* send `rtp` as the RTP timestamp (rtp=passthrough) */
#define MTL_U_SCATTER 0x40u      /* packet units: plane 0 is a packet pointer table */
/* mtl_unit.flags: outputs */
#define MTL_U_USED_REDUNDANCY 0x10000u /* RX: complete thanks to the other 2022-7 leg */
#define MTL_U_COPIED 0x20000u          /* this unit did not take the direct path */
#define MTL_U_NON_COMPLIANT 0x40000u   /* EXACT launch, or a timing model violation */
#define MTL_U_SNAPPED 0x80000u         /* TAI media time moved to the grid */
#define MTL_U_RESLOTTED 0x100000u      /* AUTO unit moved to a later slot */

/* mtl_unit.time_valid: which time fields hold a value (zero is a valid time) */
#define MTL_T_INDEX 0x1u
#define MTL_T_MEDIA 0x2u
#define MTL_T_LAUNCH 0x4u
#define MTL_T_DEADLINE 0x8u
#define MTL_T_FIRST 0x10u
#define MTL_T_LAST 0x20u
#define MTL_T_MARGIN 0x40u
#define MTL_T_HW 0x80u         /* first/last are NIC timestamps, not software estimates */
#define MTL_T_ESTIMATED 0x100u /* the time base is not locked (e.g. SYSTEM_TAI) */

/* A unit is what get() lends, put() sends, results() returns and attach() describes.
   Field use by call (TX = transmit stream, RX = receive stream):
     get TX:     lease, buf, planes; index + deadline_ns = the next slot and its submit-by
     put TX:     lease, time_mode + index/media_ns, flags, cookie, hold, launch_ns, size,
                 samples, rows_ready, rtp, meta (copied), ext
     result TX:  everything from put + status, reason, launch_ns (scheduled), first_ns /
                 last_ns (wire), deadline_ns, margin_ns, rtp; lease is stale, buf valid
     get RX:     everything: status, index/media_ns, first_ns/last_ns (arrival),
                 deadline_ns (presentation = media + link_offset_ns), rtp, size, samples,
                 pkts_lost, meta (ANC packet table or sender user meta), planes
     attach:     planes (addresses inside an imported region) and cookie per buffer
   get() reads only `buf` (with MTL_GET_BUF) and `ext` (with MTL_GET_EXT) and writes
   every other field, so an uninitialised unit is fine for get. Zero a unit you build
   yourself (mtl_write, attach). */
struct mtl_unit {
  mtl_h stream;      /* the stream (results read from a shared queue) */
  mtl_lease_h lease; /* access to this slot, from get until put or release */
  mtl_lease_h hold;  /* TX: an RX lease kept alive until this unit's result (fan-out) */
  uint64_t cookie;   /* TX: returned in the result; RX: the attached buffer's cookie */
  int64_t index;     /* media index on the stream's timeline */
  int64_t media_ns;  /* TAI media time; RTP = floor(media x rate) */
  int64_t launch_ns; /* TX: requested (MTL_U_LAUNCH_*) or scheduled first packet */
  int64_t deadline_ns;
  int64_t first_ns; /* TX: first packet on the wire; RX: first packet arrival */
  int64_t last_ns;
  int64_t margin_ns;   /* TX: deadline - submit; negative = late */
  uint32_t time_mode;  /* enum mtl_time_mode */
  uint32_t time_valid; /* MTL_T_* */
  uint32_t status;     /* enum mtl_status */
  uint32_t reason;     /* MTL_REASON_* (mtl_reasons.h); mtl_reason_name() */
  uint32_t flags;      /* MTL_U_* */
  uint32_t buf;        /* pool index of the buffer: identity, never an access token */
  uint32_t rtp;        /* RTP timestamp of the first packet */
  uint32_t size;       /* valid bytes: audio, ANC user data words, fastmeta, cvideo */
  uint32_t samples;    /* audio samples */
  uint32_t rows_ready; /* progressive: rows [0, rows_ready) are final */
  uint32_t pkts_lost;  /* RX: after the 2022-7 merge */
  uint32_t plane_count;
  uint32_t meta_size;
  uint32_t reserved0;
  const void* meta; /* see the field table above */
  void* ext;        /* chain of struct mtl_ext records (mtl_unit_ext.h); NULL = none */
  struct mtl_plane plane[MTL_MAX_PLANES];
  uint64_t reserved[8];
};

/* Head of every ext chain record (rare per-unit inputs and outputs: per-leg times,
   missing-packet ranges, typed user meta). */
struct mtl_ext {
  uint32_t type;
  uint32_t size;
  struct mtl_ext* next;
};

/* ---- Events ------------------------------------------------------------------------ */

enum mtl_event_type {
  MTL_EV_NONE = 0,
  MTL_EV_STATE = 1,   /* arg0 old, arg1 new enum mtl_state; reason */
  MTL_EV_RETIRED = 2, /* a closed stream returned its last lease; pool freed */
  MTL_EV_LINK = 3,    /* port arg0: arg1 up, arg2 Mb/s; reason PORT_RESET on reset */
  MTL_EV_LEG = 4,     /* leg: arg0 flow state, arg1 admin, arg2 oper */
  MTL_EV_TIME = 5,    /* arg0 time state, arg1 step ns (0 = no step) */
  MTL_EV_SIGNAL = 6,  /* RX: arg0 1 = packets present */
  MTL_EV_FORMAT = 7,  /* RX: detected format differs; arg0 changed-property mask */
  MTL_EV_DISCONTINUITY = 8, /* RX: arg0 old, arg1 new RTP */
  MTL_EV_TIMING = 9, /* infeasible timing: arg0 shortfall ns, arg1 suggested delay */
  MTL_EV_BACKPRESSURE = 10, /* arg0 begin/end, "status.blocked_on" says on what */
  MTL_EV_UNDERRUN = 11,     /* TX: slots filled by the underrun policy, arg0 count */
  MTL_EV_RECOVERY = 12,     /* arg0 begin/end, arg1 units dropped */
  MTL_EV_PACING = 13,       /* granted pacing changed ("granted.pacing") */
  MTL_EV_MANAGER = 14,      /* MtlManager lost or back: arg0 state */
  MTL_EV_TICK = 15,         /* opt-in epoch tick: arg0 index (was vsync) */
  MTL_EV_USER = 16,         /* posted by the app (mtl_advanced.h) */
  MTL_EV_OVERFLOW = 17,     /* arg0 events lost; every state has a getter key */
};
struct mtl_event {
  uint32_t type;   /* enum mtl_event_type */
  uint32_t reason; /* MTL_REASON_* */
  mtl_h origin;    /* instance, stream or timeline */
  uint64_t seq;    /* per reader; a gap means overflow */
  int64_t tai_ns;  /* of the last occurrence */
  uint32_t coalesced;
  uint32_t leg; /* or port, for LINK */
  int64_t arg[3];
  char name[64]; /* the origin's name, readable after it retired */
};

/* Stream and timeline states: the value of key "state". */
enum mtl_state {
  MTL_STATE_CREATED = 1,
  MTL_STATE_ARMED = 2, /* start instant ahead */
  MTL_STATE_RUNNING = 3,
  MTL_STATE_DRAINING = 4,
  MTL_STATE_FLUSHING = 5,
  MTL_STATE_STOPPED = 6, /* keys may change; the next start applies them */
  MTL_STATE_ERROR = 7,   /* only stop and close are legal */
  MTL_STATE_CLOSING = 8, /* deferred close: leases still out */
};

struct mtl_error {
  int32_t code;    /* negative MTL_E*; 0 = none */
  uint32_t reason; /* MTL_REASON_* */
  char key[40];    /* the key at fault, "" if none */
  char detail[80];
};

/* ---- Objects and keys (CP) ------------------------------------------------------- */

/* spec: "ports=0000:af:01.0,0000:af:01.1 sip=192.168.1.10,192.168.2.10 lcores=2-5".
   "default ..." opens or joins the refcounted process-wide instance (merge rules). */
MTL_API int mtl_open(const char* spec, mtl_h* inst);
/* Any object. A stream with app leases out closes when they return (MTL_EV_RETIRED);
   a region is -MTL_EBUSY while referenced; an instance lives until its last stream. */
MTL_API int mtl_close(mtl_h h);

/* spec: "<essence> <tx|rx> key=value ...", essence: video cvideo audio anc fastmeta rtp.
   inst MTL_DEFAULT: the default instance, opened from this spec's port= and sip= when
   no default instance exists yet. The stream is CREATED; nothing is reserved yet. */
MTL_API int mtl_stream(mtl_h inst, const char* spec, mtl_h* out);
/* A named timeline (spec "name=av1 anchor=at_start"; "name=epoch" is predefined).
   Streams join with key timeline=<name>; starting the timeline starts its members. */
MTL_API int mtl_timeline(mtl_h inst, const char* spec, mtl_h* out);
/* A shared queue (spec "name=ui events=port,time,session"); streams bind with
   queue=<name>, their results, RX units and events are then read here. */
MTL_API int mtl_queue(mtl_h inst, const char* spec, mtl_h* out);
/* Memory MTL may DMA: page-aligned va/len, refcounted, mapped into every port and DMA
   engine a stream using it needs. alloc: library hugepages, *va written. */
#define MTL_MEM_READ 0x1u    /* TX reads it; 0 = read and write */
#define MTL_MEM_WRITE 0x2u   /* RX writes it */
#define MTL_MEM_MAP_ALL 0x4u /* map into every port now, or fail */
MTL_API int mtl_mem_import(mtl_h inst, void* va, uint64_t len, uint32_t flags,
                           mtl_h* region);
MTL_API int mtl_mem_alloc(mtl_h inst, uint64_t len, uint32_t flags, void** va,
                          mtl_h* region);

/* "key=value key=value ...". CREATED/STOPPED: applied at the next start. RUNNING: staged
   until mtl_apply(); a key that needs STOPPED is -MTL_EBUSY. */
MTL_API int mtl_set(mtl_h h, const char* kv);
MTL_API int mtl_set_int(mtl_h h, const char* key, int64_t value);
/* Effective, granted, status and stats keys. DP for "state", "status.*" and "stats.*". */
MTL_API int mtl_get_int(mtl_h h, const char* key, int64_t* value);
/* Returns the length; enum-valued keys read as names ("status.blocked_on" = "results").
 */
MTL_API int mtl_get_str(mtl_h h, const char* key, char* buf, size_t cap);

/* ---- Lifecycle (CP) --------------------------------------------------------------- */

enum mtl_at {
  MTL_AT_NOW = 0,
  MTL_AT_TAI = 1,   /* `when` = TAI ns */
  MTL_AT_INDEX = 2, /* `when` = media index on the timeline */
};
/* Stream: reserve, join, arm. Timeline: arm every member at one T0, all or none. */
MTL_API int mtl_start(mtl_h h, uint32_t at, int64_t when);
enum mtl_stop_mode {
  MTL_STOP_DRAIN = 0,   /* send every queued unit at its slot */
  MTL_STOP_FLUSH = 1,   /* queued units become FLUSHED */
  MTL_STOP_DISCARD = 2, /* FLUSHED, but stay RUNNING (seek); next put may REBASE */
};
/* Returns when every accepted unit is terminal, or -MTL_ETIMEDOUT (then FLUSH). */
MTL_API int mtl_stop(mtl_h h, uint32_t mode, int64_t timeout_ns);
/* Commit every staged key of a running stream at one activation, on every leg, or
   change nothing (IS-05 activation, leg enable, flow change). */
MTL_API int mtl_apply(mtl_h h, uint32_t at, int64_t when);

#define MTL_ANY_BUF UINT32_MAX
/* buf MTL_ANY_BUF: the natural layout and a dry run of admission (nothing allocated);
   the granted.* and req.* keys are valid afterwards. buf = j: buffer j's planes. */
MTL_API int mtl_layout(mtl_h s, uint32_t buf, struct mtl_unit* out);
/* pool=attached: the stream's buffers, n = key count; CREATED or STOPPED. */
MTL_API int mtl_attach(mtl_h s, const struct mtl_unit* bufs, uint32_t n);

/* ---- Data path ----------------------------------------------------------------------
 */

#define MTL_GET_BUF 0x1u /* TX: lease buffer u->buf (a framework surface) */
#define MTL_GET_SLOT                                                \
  0x2u /* TX: a slot without memory; put sets planes (pool=dynamic) \
        */
#define MTL_GET_PARTIAL 0x4u /* RX progressive: deliver once rows start to arrive */
#define MTL_GET_EXT 0x8u     /* fill the output records chained at u->ext */
/* TX: a writable slot; RX: a received unit. WT. */
MTL_API int mtl_get(mtl_h s, struct mtl_unit* u, uint32_t flags, int64_t timeout_ns);
/* TX only: send the leased unit; exactly one result follows. On failure the lease
   stays the app's. MTL_U_PARTIAL: progressive rows re-put the same lease with a larger
   rows_ready; packet chunks continue the media unit in the next chunk. DP (conversion
   runs in the caller when app_format differs). */
MTL_API int mtl_put(mtl_h s, const struct mtl_unit* u);
/* Give a lease back without sending: TX abort, RX done. Any thread, any order. DP. */
MTL_API int mtl_release(mtl_h s, mtl_lease_h lease);
/* TX copy path (audio, ANC, fastmeta, packet units): get + copy + put in one call;
   `meta` carries time_mode, index/media_ns, samples, meta, flags (NULL = AUTO). WT. */
MTL_API int mtl_write(mtl_h s, const void* data, size_t bytes,
                      const struct mtl_unit* meta, int64_t timeout_ns);
/* TX results ("results=all|errors"), and RX units of streams bound to queue h. WT. */
MTL_API int mtl_results(mtl_h h, struct mtl_unit* out, uint32_t max, int64_t timeout_ns);
/* Events of a stream, timeline, instance or queue. WT. */
MTL_API int mtl_events(mtl_h h, struct mtl_event* out, uint32_t max, int64_t timeout_ns);

/* ---- Waiting ----------------------------------------------------------------------- */

#define MTL_READY_UNIT 0x1u   /* get would succeed */
#define MTL_READY_RESULT 0x2u /* results would return >= 1 */
#define MTL_READY_EVENT 0x4u  /* events would return >= 1 */
/* > 0: the ready subset of mask. 0 (timeout 0 only): nothing ready and the wait handle
   is armed, so block on it. < 0: -MTL_ETIMEDOUT, -MTL_ECANCELED, ... WT. */
MTL_API int mtl_wait(mtl_h h, uint32_t mask, int64_t timeout_ns);
/* An fd (Linux eventfd, poll POLLIN) or a Windows event HANDLE; the library drains it. */
MTL_API int mtl_wait_handle(mtl_h h, uint32_t mask, intptr_t* native);
/* on = 1: every waiter on h (an instance: on everything) gets -MTL_ECANCELED until
   mtl_interrupt(h, 0). AS when on = 1, CP when on = 0. */
MTL_API int mtl_interrupt(mtl_h h, int on);

/* ---- Time, errors, version --------------------------------------------------------- */

/* TAI ns from the published time base. DP. */
MTL_API int mtl_now(mtl_h inst, int64_t* tai_ns);
/* Exact k = floor((tai - T0) / period) on a stream's (or timeline's) grid; -MTL_EAGAIN
   while T0 is unresolved. With timeline=epoch this is the cross-process index. DP. */
MTL_API int mtl_index_at(mtl_h h, int64_t tai_ns, int64_t* index);
/* The calling thread's last failure; valid right after a call returned < 0. DP. */
MTL_API int mtl_last_error(struct mtl_error* out);
MTL_API const char* mtl_strerror(int code); /* "MTL_EINVAL"; AS */
MTL_API uint32_t mtl_version(void);         /* (major << 16 | minor << 8 | patch); AS */

/* ---- Size checks (64-bit targets) ------------------------------------------------- */

#define MTL_ALT_SIZE(name, n) \
  typedef char mtl_alt_sz_##name[(sizeof(struct name) == (n)) ? 1 : -1]
MTL_ALT_SIZE(mtl_h, 8);
MTL_ALT_SIZE(mtl_lease_h, 8);
MTL_ALT_SIZE(mtl_plane, 24);
MTL_ALT_SIZE(mtl_unit, 320);
MTL_ALT_SIZE(mtl_ext, 16);
MTL_ALT_SIZE(mtl_event, 128);
MTL_ALT_SIZE(mtl_error, 128);

#if defined(__cplusplus)
}
#endif

#endif /* MTL_ALT_H */
