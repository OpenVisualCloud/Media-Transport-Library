/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl.h - the unified MTL API, core header, experimental revision 0.2 (design sketch).
 *
 * STATUS: design sketch for maintainer review (doc/unified-api/, revision 4). Nothing is
 * implemented. This file is normative for names, types, layouts and call classes; where
 * the prose disagrees, the header wins (sketch/README.md).
 *
 * The core is what a media application needs: open an instance, create a session for any
 * essence (video, compressed video, audio, ANC, fast metadata, generic RTP) in frame, row or
 * packet units, start and stop it, move units in and out, wait, and read why something
 * failed. Everything else is an optional header that includes this one:
 *
 *   mtl_mem.h      imported memory, attached pools, named slots, IOVA
 *   mtl_sync.h     timelines, media-index arithmetic, clocks and the time reference
 *   mtl_queue.h    events, and shared queues for many sessions
 *   mtl_packet.h   packet tables and RTP header layouts for unit = MTL_UNIT_PACKETS
 *   mtl_observe.h  stats registry, full result records, diagnostics, logging, capture
 *   mtl_options.h  option keys for every tuning knob (generated)
 *   mtl_reasons.h  the reason codes (generated)
 *   mtl_format.h   application pixel formats, format names and layouts
 *   mtl_util.h     helpers built only on public calls (copy path, parsers, ANC helpers)
 *   mtl_rtcp.h     RTCP sender reports and the IPMX Info Block
 *   mtl_crypto.h   payload encryption (IPMX PEP)    mtl_sdp.h  SDP render and parse
 *   mtl_plugin.h   codec and converter plugin ABI      mtl_convert.h  colour conversion
 *   mtl_legacy.h   bridge to a legacy mtl_handle        mtl_debug.h    tests only
 *
 * Rules
 *
 * R1  Returns: 0 or a count/mask > 0 on success, a negative MTL_E* code on failure.
 * R2  Timeouts are the last argument, int64_t ns: 0 = do not wait, MTL_FOREVER = no limit.
 *     A data call (acquire, dequeue, reap, read, wait) that finds nothing, now or by its
 *     timeout, returns -MTL_EAGAIN; a read that returns a count returns at least 1.
 *     Arming: a data call that returns -MTL_EAGAIN on an application thread sets its
 *     target's wake request; the first completion for that target clears it and signals
 *     the session's one wait handle; the next data call on the session drains the handle.
 *     So "drain until -MTL_EAGAIN, then sleep on the wait handle" never misses a wake-up,
 *     and an application that never sleeps never causes a wake-up syscall. -MTL_EAGAIN
 *     sets only code and reason in mtl_last_error(). -MTL_ETIMEDOUT is for control-plane
 *     deadlines (stop).
 * R3  Input structs start with uint32_t struct_size. MTL_INIT(&s) zero-fills and sets it;
 *     a zero-filled struct is the default configuration. The library reads
 *     min(struct_size, known); unknown non-zero bytes are -MTL_EINVAL. Output structs carry
 *     no struct_size: the library fills them up to the size argument and zeroes what it
 *     does not know. Structs the library writes and reads back (mtl_unit, a config from
 *     get_config) use their own struct_size. Every pointer in an input struct is
 *     read during the call and deep-copied (strings included); the caller may free it on
 *     return. Embedded structs are fixed size.
 * R4  Handles are 64-bit values of distinct types; 0 is the null handle of every type and
 *     a closed handle is never reissued. A call with an out handle writes the null handle
 *     on failure; every close returns 0 for a null handle. A stale or foreign handle fails
 *     with -MTL_EBADF, a lease already returned with -MTL_ESTALE; the one exception is
 *     mtl_instance_get_health on an instance consumed by close or shutdown, which returns
 *     -MTL_ESHUTDOWN, so a probe racing the close sees "shutting down". Handle slots are
 *     process-wide and never freed, so a handle stays safe to pass after its instance is
 *     gone: on an object the instance closed (deployment.md), data calls return -MTL_ESHUTDOWN,
 *     and its close and a lease's release return 0. A buffer is named by its pool slot
 *     index, never by a handle.
 * R5  Times are int64_t ns since 1970-01-01 TAI on the instance clock, valid only when their
 *     flag says so: TAI while the time base is locked, flagged ESTIMATED when it is not (a
 *     free-running clock without PTP); an RX value on the sender's clock is flagged
 *     SENDER_TIME (IPMX, mtl_rtcp.h).
 * R6  Call classes: CP control plane (may allocate and block); DP data plane (O(1), no
 *     allocation, lock a tasklet takes, syscall or logging; the one syscall is the
 *     non-blocking read that drains an armed wait handle, R2); DPC data plane that does
 *     work
 *     in the caller (copy, conversion); WT wait, DP when timeout is 0; AS async-signal-safe.
 *     No application code ever runs on an MTL tasklet. The library calls application code
 *     only from the threads mtl_queue_dispatch_start() and mtl_log_set_sink() create (and
 *     codec plugins from their own threads; the later inline notify of M5 is the one
 *     exception that runs on the completing context). Those threads may not close or shut
 *     down the instance, which joins them (-MTL_EDEADLK). Library busy-loop threads (only
 *     through a later advanced header, M12) may call DP functions with timeout 0; anything
 *     else returns -MTL_EDEADLK there.
 * R7  Tuning knobs are options (mtl_options.h): absent means the documented default.
 * R8  Process: the library installs no signal handler and no atexit (DPDK's own SIGBUS
 *     handlers, held while it grows its heap and installed by instance.hotplug, are the
 *     exceptions); a signal handler
 *     may call the AS functions only, at any time, also during and after close. Every
 *     descriptor MTL or DPDK opens for it is close-on-exec. An instance belongs to the
 *     process that opened it: in a fork()ed child MTL closes the descriptors it tracks
 *     (VFIO, MtlManager, CPU locks, eventfds) at once, every call returns -MTL_EBADF
 *     (reason FORKED) except close, which drops local state only, and library memory is
 *     MADV_DONTFORK. What MTL holds outside the process (device DMA, queues, CPUs, kernel
 *     programs, MtlManager grants) is tied to a descriptor the kernel closes at exit or
 *     reconciled at the next open, and nothing is found by PID, so a SIGKILL at any
 *     instant leaves nothing that blocks a restart; the residuals (IGMP membership until
 *     the switch ages it out) are listed in deployment.md
 */

#ifndef MTL_EXPERIMENTAL_MTL_H
#define MTL_EXPERIMENTAL_MTL_H

#include <stddef.h>
#include <stdint.h>

#if UINTPTR_MAX != 0xFFFFFFFFFFFFFFFFu
#error "mtl.h supports 64-bit targets only"
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Export and annotation macros ---------------------------------------------- */

#if defined(SWIG) || defined(__bindgen)
#define MTL_API
#elif defined(_WIN32)
#if defined(MTL_BUILD)
#define MTL_API __declspec(dllexport)
#else
#define MTL_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define MTL_API __attribute__((visibility("default")))
#else
#define MTL_API
#endif
#define MTL_API_CP MTL_API
#define MTL_API_DP MTL_API
#define MTL_API_DPC MTL_API
#define MTL_API_WT MTL_API
#define MTL_API_AS MTL_API
#define MTL_NULLABLE /* the pointer may be NULL */
/* Data addresses: pointers in C and C++, integers in bindings. */
#if defined(SWIG) || defined(__bindgen)
#define MTL_ADDR(T) uintptr_t
#else
#define MTL_ADDR(T) T*
#endif
/* C99 size check: every public struct has one. */
#ifndef MTL_SIZE_CHECK
#define MTL_SIZE_CHECK(name, n) \
  typedef char mtl_sz_##name[(sizeof(struct name) == (n)) ? 1 : -1]
#endif

/* ---- Versions, limits, time units ----------------------------------------------- */

#define MTL_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
#define MTL_API_VERSION MTL_VERSION_NUM(0, 2, 0)

#define MTL_FOREVER ((int64_t)-1)
#define MTL_US(x) ((int64_t)(x)*1000)
#define MTL_MS(x) ((int64_t)(x)*1000000)
#define MTL_SEC(x) ((int64_t)(x)*1000000000)

#define MTL_MAX_LEGS 2   /* ST 2022-7 legs */
#define MTL_MAX_PLANES 4 /* planes per unit */
#define MTL_NAME_MAX 64  /* names, NUL included */

/* ---- Errors: Linux errno values on every OS, one meaning each -------------------- */

#define MTL_EIO 5         /* it failed: a session in ERROR, or a device that could not be
                             stopped; the reason says why */
#define MTL_EBADF 9       /* null, foreign or closed handle */
#define MTL_EAGAIN 11     /* nothing now (or by the timeout); the wait target is armed */
#define MTL_ENOMEM 12     /* allocation failed */
#define MTL_EBUSY 16      /* in use, or not allowed in this state */
#define MTL_EEXIST 17     /* name in use with another configuration */
#define MTL_ENODEV 19     /* device removed, or its reset failed */
#define MTL_EINVAL 22     /* invalid argument; mtl_last_error() names the field */
#define MTL_ENOSPC 28     /* capacity: queues, lcores, regions, payload size */
#define MTL_ERANGE 34     /* time outside the accepted window */
#define MTL_EDEADLK 35    /* not allowed from this thread: a library busy-loop thread, or
                             a library thread that close would join */
#define MTL_ENOTSUP 95    /* capability or option absent */
#define MTL_ESHUTDOWN 108 /* the application stopped or closed the session or instance */
#define MTL_ETIMEDOUT 110 /* a control-plane deadline passed (a DRAIN stop) */
#define MTL_ESTALE 116    /* a lease that was already returned */
#define MTL_ECANCELED 125 /* data waits interrupted; sticky until interrupt(..., 0) */

/* The calling thread's last failure: valid until its next MTL call that is not AS. */
struct mtl_error_info {
  int32_t code;    /* negative MTL_E*; 0 = none */
  uint32_t reason; /* mtl_reasons.h */
  char field[48];  /* the config field or option at fault, "" if none */
  char detail[128];
};
MTL_API_DP int mtl_last_error(struct mtl_error_info* out, size_t size);
MTL_API_AS const char* mtl_error_name(int code);       /* "MTL_EINVAL" */
MTL_API_AS const char* mtl_reason_name(uint32_t reason); /* "TOO_LATE" */
MTL_API_AS uint32_t mtl_version_num(void);              /* of the running library */

/* ---- Handles and struct initialisation ------------------------------------------- */

typedef struct mtl_instance_h {
  uint64_t id;
} mtl_instance_h;
typedef struct mtl_session_h {
  uint64_t id;
} mtl_session_h;
/* Access to one pool slot, from acquire or dequeue until submit or release. */
typedef struct mtl_lease_h {
  uint64_t id;
} mtl_lease_h;
/* Memory MTL may DMA (mtl_mem.h); library pools are regions too. */
typedef struct mtl_region_h {
  uint64_t id;
} mtl_region_h;
/* An exact clock origin shared by sessions (mtl_sync.h); null = the SMPTE epoch. */
typedef struct mtl_timeline_h {
  uint64_t id;
} mtl_timeline_h;

#if defined(__cplusplus)
#define MTL_NULL(T) (T{})
#else
#define MTL_NULL(T) ((T){0})
#endif
#define MTL_IS_NULL(h) ((h).id == 0)
/* Equality of two lvalue handles of one type: mixing types warns in C (an error with
   -Werror) and does not compile in C++. Compare with null through MTL_IS_NULL. */
#define MTL_SAME(a, b) ((void)sizeof(&(a) == &(b)), (a).id == (b).id)

/* A reference to any object, for events, stats, options and fault injection. */
enum mtl_object_kind {
  MTL_OBJ_INSTANCE = 1,
  MTL_OBJ_PORT = 2, /* index = port, id = the instance */
  MTL_OBJ_SESSION = 3,
  MTL_OBJ_TIMELINE = 4,
  MTL_OBJ_REGION = 5,
  MTL_OBJ_SCHED = 6, /* index = scheduler, id = the instance */
  MTL_OBJ_QUEUE = 7,
};
struct mtl_object {
  uint32_t kind; /* enum mtl_object_kind */
  uint32_t index;
  uint64_t id;
};
static inline struct mtl_object mtl_obj(uint32_t kind, uint32_t index, uint64_t id) {
  struct mtl_object o;
  o.kind = kind;
  o.index = index;
  o.id = id;
  return o;
}
#define MTL_OBJ_OF_SESSION(s) mtl_obj(MTL_OBJ_SESSION, 0, (s).id)
#define MTL_OBJ_OF_INSTANCE(mt) mtl_obj(MTL_OBJ_INSTANCE, 0, (mt).id)
#define MTL_OBJ_OF_PORT(mt, p) mtl_obj(MTL_OBJ_PORT, (p), (mt).id)

/* Zero-fills size bytes and sets struct_size = size. Bindings call it directly. */
MTL_API_AS void mtl_struct_init(void* p, size_t size);
#define MTL_INIT(p) mtl_struct_init((p), sizeof(*(p)))

/* ---- Common enumerations --------------------------------------------------------- */

enum mtl_dir {
  MTL_TX = 1,
  MTL_RX = 2,
};

enum mtl_essence {
  MTL_VIDEO = 1,    /* ST 2110-20 */
  MTL_CVIDEO = 2,   /* ST 2110-22 */
  MTL_AUDIO = 3,    /* ST 2110-30/31 */
  MTL_ANC = 4,      /* ST 2110-40 */
  MTL_FASTMETA = 5, /* ST 2110-41 */
  MTL_RTP = 6,      /* generic RTP: ST 2022-6, custom payloads; packet units only */
};

enum mtl_unit_kind {
  MTL_UNIT_FRAME = 0,   /* a frame or a field */
  MTL_UNIT_ROWS = 1,    /* video: rows published progressively */
  MTL_UNIT_PACKETS = 2, /* app-built RTP packets in chunks (mtl_packet.h) */
};

enum mtl_scan {
  MTL_PROGRESSIVE = 0,
  MTL_INTERLACED = 1, /* the unit is a field */
  MTL_PSF = 2,        /* paced as interlaced, one index per frame */
};

/* What a TX unit's media time is (timing.md). RTP = floor(M x rate) in every mode but SENDER. */
enum mtl_media_mode {
  MTL_MEDIA_AUTO = 0,  /* the next slot */
  MTL_MEDIA_INDEX = 1, /* unit.media_index on the session's timeline */
  MTL_MEDIA_TAI = 2,   /* unit.media_tai_ns, snapped to the grid */
  /* unit.media_tai_ns is the source's own sampling instant on the instance clock, never
     snapped: an async source (IPMX mediaclk:sender). RTP follows the source: RTP = RTP0 +
     floor(k x period x rate), RTP0 = floor(M0 x rate) at the first unit or after a
     DISCONTINUITY, and k advances by max(1, round((M - M_prev) / period)) per unit, so a
     missed VSYNC skips one period without drift building up. Launch = M + min_tx_delay_ns
     on the nominal-period schedule; a unit that would overlap the previous one is DROPPED
     (WOULD_OVERLAP); sender reports carry M (nmos-ipmx.md). AT_INDEX, RX_BY_INDEX and tx.precede
     targets and mtl_index_at() are -MTL_EINVAL on such a session; a result's
     media_index is k. */
  MTL_MEDIA_SENDER = 3,
};

/* When content exists relative to its media time (timing.md). */
enum mtl_source_kind {
  MTL_SOURCE_PLAYBACK = 0, /* before its time: files, generators */
  MTL_SOURCE_CAPTURE = 1,  /* only after its sampling instant: cameras, encoders */
  MTL_SOURCE_GATEWAY = 2,  /* in the same frame, line by line: SDI to IP */
};

enum mtl_state {
  MTL_STATE_CREATED = 1,
  MTL_STATE_ARMED = 2, /* start instant ahead */
  MTL_STATE_RUNNING = 3,
  MTL_STATE_DRAINING = 4,
  MTL_STATE_FLUSHING = 5,
  MTL_STATE_STOPPED = 6,
  MTL_STATE_ERROR = 7,      /* only stop and close are legal */
  MTL_STATE_CLOSING = 8,    /* closed, retiring: leases or device references remain */
  MTL_STATE_RETIRED = 9,    /* closed and retired; the handle stays retired */
};

enum mtl_stop_mode {
  MTL_STOP_DRAIN = 0, /* send every queued unit at its slot */
  MTL_STOP_FLUSH = 1, /* queued units FLUSHED (STOP_FLUSH); a unit whose first packet left
                         is sent to its end at its pace (rows: tx.rows_late, STALL as
                         TRUNCATE) and gets its normal status */
};

/* Why acquire is waiting (status.blocked_on). */
enum mtl_blocked_on {
  MTL_BLOCKED_NONE = 0,
  MTL_BLOCKED_BUFFERS = 1,    /* every slot queued or in flight */
  MTL_BLOCKED_RESULTS = 2,    /* unread results fill the ring: reap */
  MTL_BLOCKED_APP_LEASES = 3, /* every slot is leased by the application */
  MTL_BLOCKED_APP_PINS = 4,   /* the free slots are pinned (mtl_tx_pin, mtl_mem.h) */
};

/* Per-leg flow state (status.leg[].flow_state, MTL_EVENT_FLOW_STATE). */
enum mtl_flow_state {
  MTL_FLOW_WAITING_NEIGHBOUR = 1, /* TX: ARP unresolved; units are DROPPED/WAITING_NEIGHBOUR */
  MTL_FLOW_RESOLVED = 2,
  MTL_FLOW_JOINING = 3, /* RX: IGMP join sent */
  MTL_FLOW_JOINED = 4,
  MTL_FLOW_JOIN_FAILED = 5,
};
/* Pacing classes (info.pacing_class, the option caps.pacing, MTL_EVENT_PACING_CHANGED). */
enum mtl_pacing {
  MTL_PACING_HW = 1,        /* any hardware class (a request only) */
  MTL_PACING_HW_RATE = 2,   /* rate-limit shaper */
  MTL_PACING_HW_LAUNCH = 3, /* launch time (E830) */
  MTL_PACING_SW = 4,        /* TSC */
  MTL_PACING_SW_NARROW = 5,
  MTL_PACING_PTP = 6,
  MTL_PACING_BEST_EFFORT = 7,
};
/* State of an instance's time base (MTL_EVENT_TIME_STATE, stat "time.state"). FREERUN: the
   source is FREERUN or SYSTEM_TAI, or AUTO fell back to free run; LOCKED only for
   PTP_BUILTIN, PHC, CLOCK_TAI and USER sources. */
enum mtl_time_state {
  MTL_TIME_FREERUN = 1,
  MTL_TIME_ACQUIRING = 2,
  MTL_TIME_LOCKED = 3,
  MTL_TIME_HOLDOVER = 4,
  MTL_TIME_LOST = 5,
};

/* ---- Time ------------------------------------------------------------------------- */

#define MTL_TIMEF_VALID 0x1u
#define MTL_TIMEF_ESTIMATED 0x2u /* the time base is not locked, e.g. system clock */
#define MTL_TIMEF_HOLDOVER 0x4u
/* TAI now from the published time base; returns MTL_TIMEF_* flags (>= 0). DP. */
MTL_API_DP int mtl_time_now(mtl_instance_h mt, int64_t* tai_ns);

struct mtl_rational {
  uint64_t num;
  uint64_t den;
};
/* Named frame rates; mtl_fps_rational() gives the exact value ({0, 0} if unknown). */
/* enum st_fps + 1, so 0 stays "not set"; 47.95 and 48 are new. */
enum mtl_fps {
  MTL_FPS_59_94 = 1, /* ST_FPS_P59_94: 60000/1001 */
  MTL_FPS_50 = 2,
  MTL_FPS_29_97 = 3, /* 30000/1001 */
  MTL_FPS_25 = 4,
  MTL_FPS_119_88 = 5,
  MTL_FPS_120 = 6,
  MTL_FPS_100 = 7,
  MTL_FPS_60 = 8,
  MTL_FPS_30 = 9,
  MTL_FPS_24 = 10,
  MTL_FPS_23_98 = 11, /* 24000/1001 */
  MTL_FPS_47_95 = 12,
  MTL_FPS_48 = 13,
};
MTL_API_AS struct mtl_rational mtl_fps_rational(uint32_t fps);

/* ---- Options ---------------------------------------------------------------------- */

/* One tuning knob: key from mtl_options.h. scope: 0 = every port, leg or scheduler the
   key applies to (or not scoped), else index + 1; mtl_option_desc says which kind. A
   later duplicate (key, scope) wins. String keys use str, the others value. */
struct mtl_option {
  uint32_t key;
  uint32_t scope;
  int64_t value;
  const char* MTL_NULLABLE str;
};

/* ---- Instance --------------------------------------------------------------------- */

/* One port. name: PCI BDF ("0000:af:01.0"), "kernel:<ifname>", "native_af_xdp:<ifname>",
   "null:<n>" (no NIC, no root: units complete on the instance clock), or "env:<VAR>[#n]":
   the n-th (from 0) PCI address in the environment variable VAR, as the Kubernetes SR-IOV
   device plugin sets PCIDEVICE_<resource> (-MTL_EINVAL, PORT_ENV_UNSET, if absent). The
   DPDK AF_XDP and AF_PACKET PMDs ("dpdk_af_xdp:", "dpdk_af_packet:") are on the removal
   list (M12). Addresses: IPv4 in bytes 0..3, the rest zero. MTL never sets a port's MAC:
   a VF keeps the one its PF (or the CNI) gave it. */
struct mtl_port_spec {
  char name[MTL_NAME_MAX];
  uint8_t ip_family;  /* 0 = IPv4, 6 = IPv6 */
  uint8_t prefix_len; /* 0 = 24 (IPv6: 64) */
  uint8_t reserved0[2];
  uint8_t sip[16];     /* source address; all zero = DHCP on kernel backends */
  uint8_t gateway[16]; /* all zero = none */
  uint32_t tx_queues;  /* 0 = auto */
  uint32_t rx_queues;  /* 0 = auto */
  uint32_t numa;       /* 0 = the device's socket, else node + 1 */
  uint8_t mac[6];      /* output of mtl_port_get_spec(): the MAC on the wire; ignored on input */
  uint8_t reserved1[2];
  uint32_t reserved[2];
};

#define MTL_INSTANCE_SHARED 0x1u /* the refcounted process-wide instance (see open) */
#define MTL_INSTANCE_TASKLET_THREAD 0x4u /* schedulers are pinned pthreads, not EAL lcores */
#define MTL_INSTANCE_TASKLET_SLEEP 0x8u  /* idle schedulers sleep */
#define MTL_INSTANCE_HW_TIMESTAMP 0x10u  /* enable NIC timestamps; sessions may require them */

/* MTL adjusts a clock only with PTP_BUILTIN: the PHC of a port it owns (a PF), or, on a
   VF, its own software time base (as today; the VF's PHC is never steered); never
   CLOCK_REALTIME, never a clock a node daemon disciplines (deployment.md). From Phase 7,
   AUTO re-evaluates while running: it moves to a better source when one appears and,
   when its source is lost, holds over and then falls back by time.fallback, without a
   step (nmos-ipmx.md). Until then AUTO chooses once, at open. */
enum mtl_time_source {
  MTL_TIME_SOURCE_AUTO = 0,        /* a disciplined NIC PHC, else CLOCK_TAI, else SYSTEM_TAI
                                      (FREERUN from Phase 7) */
  MTL_TIME_SOURCE_PTP_BUILTIN = 1, /* MTL's own PTP client (named only, never by AUTO) */
  MTL_TIME_SOURCE_PHC = 2,         /* ptp4l/phc2sys discipline the NIC PHC */
  MTL_TIME_SOURCE_CLOCK_TAI = 3,   /* CLOCK_TAI; rejected if the kernel offset is 0 */
  MTL_TIME_SOURCE_SYSTEM_TAI = 4,  /* CLOCK_REALTIME + UTC offset, ESTIMATED, follows NTP */
  MTL_TIME_SOURCE_USER = 5,        /* the app feeds it (mtl_sync.h) */
  MTL_TIME_SOURCE_FREERUN = 6,     /* seeded once from the system clock, never stepped,
                                      ESTIMATED: an IPMX internal clock without PTP */
};

struct mtl_instance_params {
  uint32_t struct_size;
  uint32_t port_count;                           /* entries in ports */
  const struct mtl_port_spec* MTL_NULLABLE ports; /* copied at open */
  const char* MTL_NULLABLE lcores;               /* CPU ids "2-5,8"; NULL = auto (deployment.md) */
  uint32_t time_source;                          /* enum mtl_time_source */
  uint32_t reserved1;
  uint64_t flags;                                /* MTL_INSTANCE_* */
  const struct mtl_option* MTL_NULLABLE options; /* instance keys, copied at open */
  uint32_t option_count;
  uint32_t reserved0;
  uint64_t reserved[8];
};

/* Opens the ports of p (legacy mtl_init). p NULL, or no port in p: the environment variable
   MTL_PORTS lists them, "0000:af:01.0=192.168.1.10/24,0000:af:01.1=192.168.2.10" or
   "null:1", a deployment convenience so the same test or pod image runs on any host; with
   no port at all, -MTL_EINVAL naming "ports". In a set-uid process MTL_PORTS and env:
   ports are -MTL_EINVAL.
   MTL_INSTANCE_SHARED: the first open creates the process-wide instance, later ones join
   it; each open returns its own reference handle (MTL_SAME is false between two), and
   every call accepts any live reference; a later open that names ports, lcores, time
   source or options that differ from the live instance fails with -MTL_EEXIST
   (INSTANCE_MISMATCH). Every check that needs no device runs before any device is
   touched, so a misconfigured pod fails at once with one reason and a one-line detail
   (mtl_last_error) fit for a termination message. Open does not wait for links,
   neighbours or time lock: mtl_instance_get_health() (mtl_observe.h) reports them. After
   a close in the same process, open works again on the same ports and a subset of the
   first open's CPUs (EAL keeps its arguments); a port quarantined by an earlier close is
   -MTL_EBUSY (QUEUE_QUARANTINED) until exit. CP. */
MTL_API_CP int mtl_instance_open(const struct mtl_instance_params* MTL_NULLABLE p,
                                 mtl_instance_h* out);
/* Drops this reference and consumes mt. The last reference shuts the instance down
   within timeout_ns, network first (deployment.md): calls entering a data call get -MTL_ESHUTDOWN
   and those inside one are waited for; TX finishes the unit on the wire and flushes the
   queued rest (close sessions first to drain them); RX leaves its groups; pending results
   and events are delivered to dispatch threads, which are then joined; the schedulers and
   devices stop; MtlManager grants are returned; memory is released. Sessions, queues and
   timelines still open are closed by it (R4). 0: retired. 1: quiesced: no device can
   reach any memory, but leases or regions are still referenced, or a library thread is
   still in application code past the deadline (counted; its memory is kept). A slot's
   memory is freed by the next control-plane call of the process after its last lease is
   returned, or at exit. -MTL_EIO (reason QUEUE_QUARANTINED): a port could not be stopped;
   nothing it can reach is freed; exit the process, and the kernel stops the device.
   timeout 0: abort semantics (no drain); MTL_FOREVER: each step is still bounded by its
   own budget. From a library thread (dispatch, log sink, codec): -MTL_EDEADLK, and mt is
   not consumed. mtl_instance_shutdown() (mtl_observe.h) adds flags and a report. 0 for a
   null handle and for a reference of a shared instance that is not the last. CP. */
MTL_API_CP int mtl_instance_close(mtl_instance_h mt, int64_t timeout_ns);
/* on = 1: every data wait of every session returns -MTL_ECANCELED until on = 0; stop and
   close still work. For signal handlers: AS with on = 1, CP with on = 0. */
MTL_API_AS int mtl_instance_interrupt(mtl_instance_h mt, int on);
/* Emergency stop (legacy mtl_abort; a second SIGTERM): interrupt every wait and stop
   every TX session at its next packet: the cut unit is FLUSHED (ABORTED) with
   MTL_TXR_PKT_SHORT, queued units FLUSHED (ABORTED). During close it skips to the hard
   stop; after close it does nothing. Close still has to follow. AS. */
MTL_API_AS int mtl_instance_abort(mtl_instance_h mt);

/* ---- Flows ------------------------------------------------------------------------ */

#define MTL_FLOWF_USER_MAC 0x1u     /* use dst_mac, no ARP */
#define MTL_FLOWF_DSCP_LITERAL 0x2u /* dscp as given, 0 = CS0 even where the profile differs */

/* One leg's network flow. The legs of a session are fixed at create (in STOPPED, FLOWS
   or LEGS may change the set): a leg exists when its udp_port is not 0 or its bit in
   legs_disabled is set, a reserved leg that stays all zero until an update gives it an
   address together with LEGS. RX: ip is a group (multicast), or the port's own address
   or all zero (unicast, then source_filter checks the sender). Fixed size. */
struct mtl_flow {
  uint32_t port;         /* 0 = the leg's own instance port, else port index + 1 */
  uint8_t ip_family;     /* 0 = IPv4 (bytes 0..3), 6 = IPv6 */
  uint8_t payload_type;  /* TX: 0 = essence default; RX: 0 = no check */
  uint8_t dscp;          /* 0 = the profile's (ST2110: CS0; IPMX: AF41 audio, AF42 others) */
  uint8_t ttl;           /* 0 = 64 */
  uint8_t ip[16];        /* TX destination; RX group or unicast */
  uint8_t source_filter[16]; /* RX source-specific multicast; all zero = none */
  uint16_t udp_port;         /* required unless the leg is reserved */
  uint16_t udp_src_port;     /* TX: 0 = udp_port; RX: 0 = any */
  uint32_t ssrc;             /* TX: 0 = random; RX: 0 = no check */
  uint8_t dst_mac[6];        /* with MTL_FLOWF_USER_MAC */
  uint16_t vlan;             /* reserved, 0 */
  uint64_t flow_flags;       /* MTL_FLOWF_* */
  uint64_t reserved[2];
};

/* A flow to an IPv4 address and UDP port (legacy dip_addr[i] and udp_port[i]). */
static inline void mtl_flow_ipv4(struct mtl_flow* f, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                                 uint16_t udp_port) {
  f->ip_family = 0;
  f->ip[0] = a;
  f->ip[1] = b;
  f->ip[2] = c;
  f->ip[3] = d;
  f->udp_port = udp_port;
}

/* ---- Essence configurations -------------------------------------------------------- */

/* A video raster: the session's own, or the video an ANC/fastmeta session belongs to.
   Any width and height up to 32767 and any rate with num <= 4194303, den <= 1023 (the RFC
   4175 and IPMX limits), not a table. */
struct mtl_raster {
  uint32_t width;
  uint32_t height;
  uint32_t scan; /* enum mtl_scan */
  uint32_t rate; /* enum mtl_fps (legacy st_fps); 0 = the rational fps below */
  struct mtl_rational fps; /* any other frame rate; {0, 0} when rate is set */
};

/* ST 2110-20 transport formats (RFC 4175 sampling and depth): enum st20_fmt + 1, so
   0 stays "not set" and migration is mechanical. */
enum mtl_video_format {
  MTL_YUV422_10 = 1, /* ST20_FMT_YUV_422_10BIT */
  MTL_YUV422_8 = 2,
  MTL_YUV422_12 = 3,
  MTL_YUV422_16 = 4,
  MTL_YUV420_8 = 5,
  MTL_YUV420_10 = 6,
  MTL_YUV420_12 = 7,
  MTL_YUV420_16 = 8,
  MTL_RGB_8 = 9,
  MTL_RGB_10 = 10,
  MTL_RGB_12 = 11,
  MTL_RGB_16 = 12,
  MTL_YUV444_8 = 13,
  MTL_YUV444_10 = 14,
  MTL_YUV444_12 = 15,
  MTL_YUV444_16 = 16,
};
enum mtl_packing { /* enum st20_packing: the same values and default */
  MTL_PACKING_BPM = 0,
  MTL_PACKING_GPM = 1,
  MTL_PACKING_GPM_SL = 2,
};
/* ST 2110-21. Under the IPMX profile N uses the TR-10-1 CMAX and VRX (TP=2110TPN). */
enum mtl_sender_type { /* enum st21_pacing: the same values */
  MTL_SENDER_N = 0,  /* ST21_PACING_NARROW */
  MTL_SENDER_W = 1,  /* ST21_PACING_WIDE */
  MTL_SENDER_NL = 2, /* ST21_PACING_LINEAR */
};
enum mtl_detect {
  MTL_DETECT_OFF = 0,
  MTL_DETECT_ON = 1, /* RX: raster and format from the stream (MTL_EVENT_RX_FORMAT) */
};

struct mtl_video_config {
  struct mtl_raster raster; /* required unless detect */
  uint32_t format;          /* enum mtl_video_format; 0 = from app_format */
  uint32_t app_format;      /* mtl_format.h; 0 = format itself, no conversion */
  uint32_t packing;         /* enum mtl_packing */
  uint32_t sender_type;     /* enum mtl_sender_type */
  uint32_t detect;          /* RX: enum mtl_detect */
  /* SDP and conversion metadata, never on the wire (mtl_format.h); uint8_t to fit the
     reserved space. MTL_UPDATE_MEDIA may change them while running when no conversion
     uses them. */
  uint8_t colorimetry; /* enum mtl_colorimetry; 0 = UNSPECIFIED (rendered: SDP requires it) */
  uint8_t tcs;         /* enum mtl_tcs; 0 = SDR */
  uint8_t range;       /* enum mtl_range; 0 = narrow */
  uint8_t reserved0;
  int64_t troffset_ns; /* 0 = TRODEFAULT; an explicit 0: option tx.troffset_ns */
  uint32_t linesize[MTL_MAX_PLANES]; /* library pools: bytes per row per plane, legacy
                                        linesize; 0 = packed */
  uint64_t reserved[2];
};

enum mtl_codec {
  MTL_CODEC_JPEGXS = 1,
  MTL_CODEC_H264 = 2,
  MTL_CODEC_H265 = 3,
};
enum mtl_cvideo_rate {
  MTL_CVIDEO_CBR = 0,     /* constant bytes per unit, as ST 2110-22 requires */
  MTL_CVIDEO_VBR_MAX = 1, /* packets follow the codestream: IPMX; ST 2110-22 non-compliant */
};
struct mtl_cvideo_config {
  struct mtl_raster raster;  /* required */
  uint32_t codec;            /* enum mtl_codec; required */
  uint32_t codestream_bytes; /* required: CBR bytes (or VBR ceiling) per unit */
  uint32_t rate_mode;        /* enum mtl_cvideo_rate */
  uint32_t app_format;       /* 0 = the app gives the codestream, else a codec plugin */
  uint32_t sender_type;      /* enum mtl_sender_type */
  uint8_t colorimetry;       /* as mtl_video_config */
  uint8_t tcs;
  uint8_t range;
  uint8_t reserved0;
  int64_t troffset_ns;
  uint64_t reserved[4];
};

enum mtl_audio_format {
  MTL_PCM8 = 1,
  MTL_PCM16 = 2,
  MTL_PCM24 = 3,
  MTL_AM824 = 4, /* ST 2110-31 */
};
enum mtl_ptime { /* enum st30_ptime + 1 */
  MTL_PTIME_1MS = 1,
  MTL_PTIME_125US = 2,
  MTL_PTIME_250US = 3,
  MTL_PTIME_333US = 4,
  MTL_PTIME_4MS = 5,
  MTL_PTIME_80US = 6,   /* ST31_PTIME_80US */
  MTL_PTIME_1_09MS = 7, /* 44.1 kHz: ST31_PTIME_1_09MS */
  MTL_PTIME_0_14MS = 8,
  MTL_PTIME_0_09MS = 9,
};
struct mtl_audio_config {
  uint32_t format;             /* enum mtl_audio_format; required */
  uint32_t sample_rate;        /* 48000, 96000, 44100; required */
  uint32_t channels;           /* required */
  uint32_t ptime;              /* enum mtl_ptime (legacy st30_ptime); 0 = 1 ms */
  uint32_t unit_samples;       /* RX unit, TX pool capacity; 0 = 10 ms in whole packets */
  /* audio media indices count samples: unit.media_index is the unit's first sample */
  uint32_t reserved0;
  uint64_t reserved[5];
};

struct mtl_anc_config {
  struct mtl_raster video; /* fps (and scan) required on the epoch timeline; all zero on a
                              created timeline: the first video of the timeline's first
                              start (-MTL_EINVAL, FIELD_REQUIRED, if there is none) */
  uint32_t max_udw_bytes;  /* per unit; 0 = 64 KiB */
  uint32_t reserved0;
  uint64_t reserved[3];
};

#define MTL_FASTMETA_FREE_RUNNING 0x1u /* own rate in video.fps, not a video's */
#define MTL_FASTMETA_RX_MATCH_DIT 0x2u /* RX: filter on data_item_type */
#define MTL_FASTMETA_RX_MATCH_K 0x4u   /* RX: filter on k_bit */
struct mtl_fastmeta_config {
  struct mtl_raster video; /* as ANC; FREE_RUNNING: fps = own rate, always required */
  uint32_t data_item_type; /* TX: on the wire; RX: filter */
  uint32_t k_bit;
  uint32_t fastmeta_flags;        /* MTL_FASTMETA_* */
  uint32_t buffer_capacity_bytes; /* 0 = 64 KiB */
  uint64_t reserved[2];
};

/* Generic RTP essence (packet units only). */
enum mtl_rtp_profile {
  MTL_RTP_LINEAR = 0, /* equal spacing over the unit, as ST 2022-6 senders do */
  MTL_RTP_GAPPED = 1, /* ST 2110-21-like, with a vertical blanking gap */
};
struct mtl_rtp_config {
  uint32_t clock_rate; /* RTP clock in Hz; required (ST 2022-6: 27000000) */
  uint32_t profile;    /* enum mtl_rtp_profile */
  struct mtl_raster unit; /* unit rate and scan in fps/scan; size fields unused */
  uint64_t bitrate_bps;   /* rate of chunks without a unit grid; 0 = derived */
  char encoding[32];      /* SDP rtpmap name, e.g. "SMPTE2022-6" */
  uint64_t reserved[4];
};

/* Packet units (unit = MTL_UNIT_PACKETS): sizes and pacing. Tables in mtl_packet.h. */
enum mtl_pkt_pacing {
  MTL_PKT_PACE_UNIT = 0,   /* the essence's wire model over each unit */
  MTL_PKT_PACE_LAUNCH = 1, /* a chunk at unit.launch_tai_ns, then at the session rate */
  MTL_PKT_PACE_ASAP = 2,   /* as fast as the queue accepts; non-compliant */
};
enum mtl_pkt_unit_time {
  MTL_PKT_TIME_SUBMIT = 0,   /* from the first chunk's submission, by media mode */
  MTL_PKT_TIME_FROM_RTP = 1, /* from the RTP timestamp of the unit's first packet */
};
struct mtl_packet_config {
  uint32_t packets_per_chunk; /* slots per lease; 0 = by essence */
  uint32_t slot_bytes;        /* max RTP packet; 0 = session.max_udp_payload (MTU - 28) */
  uint32_t packets_per_unit;  /* UNIT pacing; 0 = derived (video) or unlimited (anc) */
  uint32_t pacing;            /* enum mtl_pkt_pacing */
  uint32_t unit_time;         /* enum mtl_pkt_unit_time */
  uint32_t rx_ring_packets;   /* RX: NIC buffers the session may hold; 0 = 512 */
  uint64_t set_fields;        /* MTL_PKT_SET_* (mtl_packet.h); 0 = verbatim */
  uint64_t packet_flags;      /* MTL_PKT_* (mtl_packet.h) */
  uint64_t reserved[3];
};

/* ---- Session configuration ---------------------------------------------------------- */

/* mtl_session_config.flags */
#define MTL_SESSION_RESULTS 0x1u        /* results for library memory (app memory: always) */
#define MTL_SESSION_POOL_ATTACHED 0x2u  /* slots come from mtl_session_attach (mtl_mem.h) */
#define MTL_SESSION_REQUIRE_DIRECT 0x4u /* fail at create if any unit would be copied */
/* RX slot = media index mod pool_count (MXL). A unit whose slot is leased or held is
   dropped and counted in the next unit's missed_before, never written over; with
   RX_LATEST an unread (not leased) unit in that slot is replaced. */
#define MTL_SESSION_RX_BY_INDEX 0x8u
#define MTL_SESSION_RX_LATEST 0x10u     /* RX: a full pool reclaims the oldest unread unit */
#define MTL_SESSION_RX_NO_FILL 0x20u    /* RX: do not zero what lost packets left out */
#define MTL_SESSION_MT_SUBMIT 0x40u     /* several threads acquire and submit */
#define MTL_SESSION_SINGLE_READER 0x80u /* one thread reaps and dequeues (release: any) */
#define MTL_SESSION_EXPORT_POOL 0x100u  /* slots lent to a framework pool: implies RESULTS */

/* One stream of one essence in one direction. The member for `essence` is read; the
   other essence members must stay zero. MTL_INIT it, set direction, essence, flows[0] and
   the essence's required fields; everything else defaults. */
struct mtl_session_config {
  uint32_t struct_size;
  uint32_t direction; /* enum mtl_dir; required */
  uint32_t essence;   /* enum mtl_essence; required */
  uint32_t unit;      /* enum mtl_unit_kind */
  char name[MTL_NAME_MAX];             /* unique per instance; "" = generated */
  struct mtl_flow flows[MTL_MAX_LEGS]; /* flows[1] that exists is the ST 2022-7 leg */
  uint64_t flags;                      /* MTL_SESSION_* */
  uint32_t pool_count;                 /* 0 = by essence */
  uint32_t legs_disabled;              /* bit per existing leg (others -MTL_EINVAL): admin
                                          down, reserved. Every existing leg set = muted:
                                          RUNNING; TX units retire at their slot, counted
                                          in tx.units_muted, no sender reports; RX groups
                                          left */
  uint32_t media_mode;                 /* enum mtl_media_mode */
  uint32_t source_kind;                /* enum mtl_source_kind */
  mtl_timeline_h timeline;             /* null = the SMPTE epoch */
  int64_t min_tx_delay_ns;             /* 0 = by source kind */
  int64_t media_time_offset_ns;        /* declared latency: shifts media time and RTP */
  const struct mtl_option* MTL_NULLABLE options; /* caller's array, deep-copied */
  uint32_t option_count;
  uint32_t reserved1;
  const void* MTL_NULLABLE next; /* reserved, NULL */
  struct mtl_video_config video;
  struct mtl_cvideo_config cvideo;
  struct mtl_audio_config audio;
  struct mtl_anc_config anc;
  struct mtl_fastmeta_config fastmeta;
  struct mtl_rtp_config rtp;
  struct mtl_packet_config packet; /* unit = MTL_UNIT_PACKETS */
  uint64_t reserved[8];
};

/* Create and start now (legacy *_create with ops, then running): the whole setup of a
   stream that needs nothing else. On failure *out is the null handle. CP. */
MTL_API_CP int mtl_session_open(mtl_instance_h mt, const struct mtl_session_config* sc,
                                mtl_session_h* out);

/* ---- Session lifecycle --------------------------------------------------------------- */

/* Creates a CREATED session: validated, resources reserved, nothing sent. CP. */
MTL_API_CP int mtl_session_create(mtl_instance_h mt, const struct mtl_session_config* sc,
                                  mtl_session_h* out);

/* The granted configuration (output; the size argument versions it). */
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
  uint8_t dst_mac[6]; /* resolved; zero while the neighbour is unresolved */
};
#define MTL_INFO_NON_COMPLIANT 0x1u /* a mode outside the standard of session.profile */
#define MTL_INFO_MEDIACLK_SENDER 0x2u /* RTP follows the source (MTL_MEDIA_SENDER) */
struct mtl_session_info {
  uint32_t direction;
  uint32_t essence;
  uint32_t unit;
  uint32_t flags; /* MTL_INFO_* */
  char name[MTL_NAME_MAX];
  int64_t created_tai_ns;
  uint32_t pool_count;
  uint32_t max_count; /* pool_count limit */
  uint32_t results;   /* 1 = results are produced */
  uint32_t direct;    /* 1 = no unit is copied */
  uint32_t pacing_class; /* enum mtl_pacing, granted */
  uint32_t leg_count;
  uint32_t pkts_per_unit;
  uint32_t unit_samples;          /* audio */
  uint32_t buffer_capacity_bytes; /* audio, ANC, fastmeta, cvideo per unit */
  uint32_t meta_capacity;         /* the per-slot meta area */
  uint32_t codestream_bytes;      /* cvideo, granted in whole packets */
  uint32_t sched_index;
  uint64_t unit_bytes;      /* one unit in the app layout */
  uint64_t pool_slot_pitch; /* library pools: bytes between slots */
  int64_t latency_min_ns;   /* for framework LATENCY queries */
  int64_t latency_max_ns;
  int64_t min_submit_lead_ns; /* submit at least this long before the media time */
  struct mtl_leg_info leg[MTL_MAX_LEGS];
  uint64_t reserved[3];
};

#define MTL_QUERY_CHECK_CAPACITY 0x1u /* also check free capacity: -MTL_ENOSPC + reason */
struct mtl_buffer_requirements; /* mtl_mem.h */
/* Dry run of create: validates and grants without allocating. info and req may be NULL;
   req gives the layout an attached pool needs. ANC/fastmeta rasters taken from a start are
   reported as 0 until then. CP. */
MTL_API_CP int mtl_session_query(mtl_instance_h mt, const struct mtl_session_config* sc,
                                 uint64_t flags, struct mtl_session_info* MTL_NULLABLE info,
                                 size_t info_size,
                                 struct mtl_buffer_requirements* MTL_NULLABLE req,
                                 size_t req_size);
MTL_API_CP int mtl_session_get_info(mtl_session_h s, struct mtl_session_info* info,
                                    size_t info_size);
/* The current configuration, to edit and pass to mtl_session_update(). Options are not
   returned (options NULL): read them with mtl_get_option(). CP. */
MTL_API_CP int mtl_session_get_config(mtl_session_h s, struct mtl_session_config* sc);

/* When something happens. A NULL `when` means NOW. */
enum mtl_when_kind {
  MTL_NOW = 0,
  MTL_AT_TAI = 1,   /* value = TAI ns */
  MTL_AT_INDEX = 2, /* value = media index on the timeline (TX start, TX flow change) */
};
struct mtl_when {
  uint32_t kind; /* enum mtl_when_kind */
  uint32_t reserved;
  int64_t value;
  int64_t preroll_ns; /* start only (ignored by update): extra lead before the first unit */
};

/* Starts n sessions, all or none. They must share one timeline and one direction, and
   with n > 1 no TX session may use MTL_MEDIA_AUTO (-MTL_EINVAL, START_SET_MIXED). T0 is
   resolved once on the timeline's grid: index 0 of every session is at T0 and index k
   at T0 + k x that session's index period (a frame or field for video and ANC, a sample
   for audio; mtl_sync.h). On an already resolved
   timeline a start joins it at the first feasible index, or at when->value with
   MTL_AT_INDEX. RX: `when` is the earliest media time delivered. t0 (may be NULL)
   receives T0. CP. */
MTL_API_CP int mtl_session_start(const mtl_session_h* s, uint32_t n,
                                 const struct mtl_when* MTL_NULLABLE when,
                                 int64_t* MTL_NULLABLE t0_tai_ns);
/* mode: enum mtl_stop_mode; the session can be started again. Stop arrays may mix
   directions. -MTL_ETIMEDOUT if DRAIN missed the deadline: the rest became FLUSHED
   (reason STOP_TIMEOUT); units already handed to the device get their result when the
   device releases them, and the session stays FLUSHING until then. CP. */
MTL_API_CP int mtl_session_stop(const mtl_session_h* s, uint32_t n, uint32_t mode,
                                int64_t timeout_ns);

/* mtl_session_update() parts: one atomic change, applied at `when` on every leg. */
#define MTL_UPDATE_FLOWS 0x1u /* sc->flows (IS-05); a port change is made before break, or
                                 -MTL_EBUSY (PORT_CHANGE_NEEDS_STOP) on a backend that
                                 cannot */
#define MTL_UPDATE_LEGS 0x2u  /* sc->legs_disabled; every existing leg may be disabled */
#define MTL_UPDATE_MEDIA 0x4u /* the essence member; CREATED or STOPPED (colorimetry, tcs
                                 and range also while running) */
#define MTL_UPDATE_POOL 0x8u  /* pool_count; CREATED or STOPPED */
/* Modifiers */
#define MTL_UPDATE_REAPPLY 0x10u /* re-apply identical enabled legs (IS-05 re-activation):
                                    RX re-sends its membership reports and re-arms its
                                    rules, TX resolves the neighbour and rebuilds headers */
#define MTL_UPDATE_DRY_RUN 0x20u /* validate and plan (planned_tai_ns filled); nothing is
                                    reserved, posted or replaced, the status is untouched,
                                    and a later update may still fail with -MTL_ENOSPC */
/* All or nothing: resources reserved before commit; neighbours are resolved from then on
   and a leg without one waits in WAITING_NEIGHBOUR, so the update still applies. Fields
   outside `parts` must equal the current (active) configuration (-MTL_EINVAL naming the
   first that differs). Options in sc->options whose key may change
   while running (R) apply at the same boundary; any other key there must equal its
   current value (-MTL_EBUSY, OPTION_STATE). direction, essence and unit never change.
   `when` applies to FLOWS, LEGS, those options, and colorimetry, tcs and range under
   MEDIA while running (NULL otherwise); in ARMED, a `when` before T0 means T0; an AT_TAI or AT_INDEX
   already past means NOW. The switch happens at the slot boundary by the clock, whether a
   unit is there or not (TX: the first slot at or after `when`; RX: units with media time
   at or after it, or by local arrival time for SENDER or unlocked clocks; audio: a packet
   boundary, so a salvo lands within one packet time). A new update replaces a pending one
   (REPLACED; an NMOS Node answers 423 instead). parts 0 with sc and when NULL cancels the
   pending one: 0 cancelled, 1 none pending, -MTL_EBUSY (UPDATE_COMMITTING) when it is
   already committing and will apply. A pending update fails (TIME_STEP) if the time base
   steps. Returns once the change is posted; planned_tai_ns (may be NULL) receives the
   media time of the boundary, and status.update_* and MTL_EVENT_UPDATE report when it
   applied. In CREATED and STOPPED it applies during the call and planned = applied = now.
   update_seq counts posted updates (not dry runs, not cancels). CP. */
MTL_API_CP int mtl_session_update(mtl_session_h s,
                                  const struct mtl_session_config* MTL_NULLABLE sc,
                                  uint64_t parts, const struct mtl_when* MTL_NULLABLE when,
                                  int64_t* MTL_NULLABLE planned_tai_ns);

#define MTL_DISCARD_REBASE 0x1u /* map first_index to the next feasible slot (seek) */
/* Queued units become FLUSHED and the session stays RUNNING (seek); RX force-completes
   the unit being received. CP. */
MTL_API_CP int mtl_session_discard(mtl_session_h s, uint64_t flags, int64_t first_index);

/* Stops (DRAIN until the deadline, then FLUSH), destroys, and waits up to timeout_ns for
   the session to retire. Always consumes s, whatever it returns: never close twice. 0:
   retired; no memory, lease, hold or device reference remains. 1: still retiring
   (leases out, or units held by the device); MTL_WAIT_RETIRED on s and
   MTL_EVENT_SESSION_RETIRED report the end, and a closed handle stays readable for that
   wait and for get_state (RETIRED). Unread results are discarded; the session is unbound
   from every queue. Sessions attached over this session's pool must close first (1
   until they do). A null handle returns 0, so cleanup paths need no checks. From a
   dispatch callback of a queue s is bound to: -MTL_EDEADLK, and s is not consumed
   (mtl_queue.h). CP. */
MTL_API_CP int mtl_session_close(mtl_session_h s, int64_t timeout_ns);

/* enum mtl_state as the result (>= 0), or < 0. DP. */
MTL_API_DP int mtl_session_get_state(mtl_session_h s);

/* Why the session is where it is (output; the size argument versions it). */
struct mtl_leg_status {
  uint32_t admin;      /* 1 = enabled */
  uint32_t oper;       /* 1 = link up and flow resolved or joined */
  uint32_t flow_state; /* enum mtl_flow_state */
  uint32_t reserved;
};
#define MTL_STATUS_RX_SIGNAL 0x1u          /* RX: packets are arriving */
#define MTL_STATUS_FORMAT_CHANGED 0x2u     /* RX: the stream differs from the config */
#define MTL_STATUS_PACING_DOWNGRADED 0x4u  /* TX: running below the granted pacing class */
#define MTL_STATUS_TIMING_WARNING 0x8u     /* TX: timing_reason, shortfall_ns are set */
#define MTL_STATUS_MUTED 0x10u             /* every leg admin-disabled; RUNNING, nothing sent
                                              (a transport state: NMOS master_enable stays
                                              the Node's) */
enum mtl_update_state {
  MTL_UPDATE_STATE_NONE = 0,
  MTL_UPDATE_STATE_PENDING = 1,
  MTL_UPDATE_STATE_APPLIED = 2,
  MTL_UPDATE_STATE_FAILED = 3,   /* update_reason says why; the old configuration stays */
  MTL_UPDATE_STATE_REPLACED = 4, /* a later update took its place before it applied */
  MTL_UPDATE_STATE_CANCELLED = 5,
};
struct mtl_session_status {
  uint32_t state;        /* enum mtl_state */
  uint32_t reason;       /* of the last transition (mtl_reasons.h) */
  uint32_t error_reason; /* why it entered ERROR, else 0 */
  int32_t error;         /* in ERROR: the code data calls return */
  uint32_t blocked_on;   /* enum mtl_blocked_on */
  uint32_t flags;        /* MTL_STATUS_* */
  uint32_t leg_count;
  uint32_t timing_reason;
  int64_t shortfall_ns;              /* TIMING_WARNING */
  int64_t suggested_min_tx_delay_ns; /* TIMING_WARNING */
  struct mtl_leg_status leg[MTL_MAX_LEGS];
  uint32_t update_state;  /* enum mtl_update_state, of the last update call */
  uint32_t update_reason; /* FAILED: mtl_reasons.h */
  uint64_t update_seq;    /* +1 per posted update; read it after the call to match events */
  int64_t update_applied_tai_ns; /* when it switched (IS-05 200); INT64_MIN until then */
};
MTL_API_CP int mtl_session_get_status(mtl_session_h s, struct mtl_session_status* st,
                                      size_t st_size);

/* ---- Units: what acquire and dequeue lend ------------------------------------------- */

struct mtl_plane {
  MTL_ADDR(void) addr; /* NULL in device memory without a CPU mapping */
  uint32_t stride;     /* bytes between rows (packet units: between slots) */
  uint32_t row_bytes;  /* valid bytes per row (packet units: slot capacity) */
  uint32_t rows;       /* rows (packet units: slots) */
  uint32_t reserved;
};

/* unit.flags, TX inputs (low 16 bits). NOT_BEFORE and EXACT exclude each other; UNIT_END
   only on packet units (-MTL_EINVAL otherwise). */
#define MTL_SUBMIT_DISCONTINUITY 0x1u /* media time jumps (seek, new clip) */
#define MTL_SUBMIT_RTP_TS 0x2u        /* send unit.rtp as the RTP timestamp (not AUTO) */
#define MTL_SUBMIT_NOT_BEFORE 0x4u    /* first packet not before launch_tai_ns */
#define MTL_SUBMIT_EXACT 0x8u         /* first packet at launch_tai_ns (non-compliant) */
#define MTL_SUBMIT_UNIT_END 0x10u     /* packet units: the last chunk of its unit */
/* SENDER mode, inline processors: RTP and the sender report's NTP come from unit.rtp and
   unit.media_tai_ns (a received SENDER_TIME unit); launch = submit + min_tx_delay_ns */
#define MTL_SUBMIT_SENDER_TIME 0x20u
/* unit.flags, RX outputs (high 16 bits) */
#define MTL_UNITF_INDEX_VALID 0x10000u
#define MTL_UNITF_TAI_VALID 0x20000u
#define MTL_UNITF_USED_REDUNDANCY 0x40000u /* complete thanks to the other leg */
#define MTL_UNITF_DISCONTINUITY 0x80000u
#define MTL_UNITF_FORMAT_CHANGED 0x100000u
#define MTL_UNITF_PARTIAL 0x200000u      /* rows: more rows follow */
#define MTL_UNITF_SECOND_FIELD 0x400000u /* interlaced: the second field (from the stream) */
#define MTL_UNITF_SENDER_TIME 0x800000u  /* media_tai_ns is on the sender's clock (mtl_rtcp.h) */

enum mtl_rx_status {
  MTL_RX_COMPLETE = 1,
  MTL_RX_INCOMPLETE = 2, /* lost packets; library pools read them as zero */
};

/* One unit. acquire and dequeue write every field up to struct_size but struct_size
   itself, so MTL_INIT it once; on failure they leave *u unchanged. TX: fill the planes,
   set the per-use fields (used, flags, media time, cookie, hold, launch), submit. RX:
   read, then release the lease. As a template (mtl_tx_write, mtl_tx_send_slot) a unit
   contributes media_index, media_tai_ns, cookie, hold, launch_tai_ns, meta and the low 16
   bits of flags; a received unit is a valid template. */
struct mtl_unit {
  uint32_t struct_size;
  uint32_t plane_count;
  mtl_lease_h lease; /* the access token */
  uint32_t slot;     /* the pool slot: stable buffer identity */
  uint32_t status;   /* RX: enum mtl_rx_status */
  struct mtl_plane plane[MTL_MAX_PLANES];
  MTL_ADDR(void) meta;    /* the slot's meta area (layout below) */
  uint32_t meta_capacity; /* bytes */
  /* per use: TX inputs (acquire zeroes them), RX outputs */
  uint32_t used;     /* video frames and rows: rows; packets: packets; audio, cvideo, ANC
                        user data words, fastmeta: bytes. TX 0 = the whole unit */
  uint32_t flags;    /* MTL_SUBMIT_* (TX), MTL_UNITF_* (RX) */
  uint32_t rtp;      /* TX: with MTL_SUBMIT_RTP_TS; RX: received */
  int64_t media_index;  /* TX: MTL_MEDIA_INDEX; RX: with MTL_UNITF_INDEX_VALID */
  int64_t media_tai_ns; /* TX: MTL_MEDIA_TAI; RX: with MTL_UNITF_TAI_VALID */
  uint64_t cookie;      /* TX: returned in the result */
  mtl_lease_h hold;     /* TX: an RX lease this unit reads from, kept until its result */
  int64_t launch_tai_ns; /* TX: with NOT_BEFORE or EXACT, and always with PKT_PACE_LAUNCH */
  uint32_t missed_before; /* RX: units the pool could not take since the last one */
  uint32_t reserved0;
  uint64_t reserved[2];
};

/* The meta area of a slot, TX and RX alike. Frame and row units: records back to back,
   each a struct mtl_meta_hdr and its payload padded to 4 bytes, ended by kind NONE or the
   capacity; at most one record per (kind, tag). Packet units: no header; the packet table starts
   at meta, one entry per slot, `used` entries valid (mtl_packet.h). TX: the meta area is
   validated and copied at submit, so later writes never reach the wire; plane bytes are
   read at send time. */
enum mtl_meta_kind {
  MTL_META_NONE = 0,
  MTL_META_ANC = 1,  /* count x struct mtl_anc_packet follow; UDW in plane 0 */
  MTL_META_USER = 2, /* bytes of user meta follow (video, at most 1332 B) */
  MTL_META_RTCP_MIB = 3, /* TX: Media Info Blocks appended to this unit's sender report
                            only (HDR per field); never changes the block version */
};
struct mtl_meta_hdr {
  uint16_t kind; /* enum mtl_meta_kind */
  uint16_t tag_version;
  uint32_t count;
  uint32_t bytes; /* after the header */
  uint32_t tag;   /* USER: 0 = untagged */
};
/* One ST 2110-40 ANC packet; its user data words are at udw_offset in plane 0. */
struct mtl_anc_packet {
  uint16_t did;
  uint16_t sdid;
  uint16_t line; /* 0 = unspecified */
  uint16_t hoffset;
  uint8_t c;      /* C bit */
  uint8_t stream; /* RFC 8331: bit 7 = S (stream number valid), bits 0-6 = StreamNum */
  uint16_t udw_count; /* <= 255 */
  uint32_t udw_offset;
};

/* ---- Data path ------------------------------------------------------------------------ */

/* TX: a writable slot. 0, or -MTL_EAGAIN (none free: status.blocked_on says why),
   -MTL_ECANCELED, -MTL_ESHUTDOWN, -MTL_EIO, ... WT. */
MTL_API_WT int mtl_tx_acquire(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);
/* Hands the unit to MTL; exactly one result follows (if results are on). The results
   ring holds pool_count entries and acquire reserves one, so producing a result never
   waits. On failure of a first submit the slot goes back to the pool without a result,
   except -MTL_EAGAIN (contention from a busy-loop thread), where the lease stays the
   application's. Rows units: submit the same lease again with a larger `used` to publish
   more rows; once a submit was accepted, a later failing submit ends the unit where its
   rows stopped (option tx.rows_late), consumes the lease, and the unit's one result
   follows when its packets have left. DPC. */
MTL_API_DPC int mtl_tx_submit(mtl_session_h s, const struct mtl_unit* u);
/* Returns an acquired, unsubmitted lease; no result. DP. */
MTL_API_DP int mtl_tx_release(mtl_session_h s, mtl_lease_h lease);

/* RX: a received unit, in delivery order. Missing packets read as zero (library pools)
   and `status` says so. WT; DPC for packet units without MTL_PKT_RX_LEND (mtl_packet.h),
   which copy the packets in the caller. */
MTL_API_WT int mtl_rx_dequeue(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);
/* Any thread, any order. DP. */
MTL_API_DP int mtl_rx_release(mtl_session_h s, mtl_lease_h lease);

/* What happened to one submitted TX unit. */
enum mtl_tx_status {
  MTL_TX_ON_TIME = 1,
  MTL_TX_LATE = 2,    /* sent late, within the late tolerance */
  MTL_TX_DROPPED = 3, /* not sent; `reason` says why; its slot stays empty on the wire */
  MTL_TX_FLUSHED = 4, /* stop(FLUSH), discard or close */
  MTL_TX_FAILED = 5,  /* device or queue failure; `error` and `reason` */
};
/* mtl_tx_result.flags */
#define MTL_TXR_MEDIA_VALID 0x1u  /* media_index, media_tai_ns */
#define MTL_TXR_MARGIN_VALID 0x2u /* margin_ns */
#define MTL_TXR_SENT_VALID 0x4u   /* sent_tai_ns */
#define MTL_TXR_SENT_HW 0x8u      /* sent_tai_ns is a NIC timestamp */
#define MTL_TXR_ESTIMATED 0x10u   /* the time base was not locked */
#define MTL_TXR_SNAPPED 0x20u     /* TAI media time moved to the grid */
#define MTL_TXR_RESLOTTED 0x40u   /* AUTO unit moved to a later slot */
#define MTL_TXR_COPIED 0x80u      /* this unit took a copy path */
#define MTL_TXR_PKT_SHORT 0x100u  /* packet units: fewer packets than declared */
/* The core result record; mtl_observe.h has the full one (pass its size to reap). */
struct mtl_tx_result {
  uint32_t status; /* enum mtl_tx_status */
  uint32_t reason; /* mtl_reasons.h */
  uint32_t slot;   /* the slot the unit was sent from */
  uint32_t flags;  /* MTL_TXR_* */
  uint64_t cookie;
  uint64_t seq; /* per session, in submission order */
  int64_t media_index;
  int64_t media_tai_ns;
  int64_t margin_ns;   /* deadline - submit; negative = late */
  int64_t sent_tai_ns; /* first packet */
  uint32_t rtp;        /* first packet on the wire */
  int32_t error;       /* MTL_E* for FAILED */
  mtl_session_h session;
  uint64_t reserved[2];
};
/* Up to max results in submission order, each rec_size bytes apart (a larger record is
   filled further, mtl_observe.h). A count >= 1, or -MTL_EAGAIN when there is none. WT. */
MTL_API_WT int mtl_tx_reap(mtl_session_h s, void* rec, size_t rec_size, uint32_t max,
                           int64_t timeout_ns);

/* ---- Waiting ----------------------------------------------------------------------------- */

#define MTL_WAIT_ACQUIRE 0x1u /* TX: acquire would succeed */
#define MTL_WAIT_DEQUEUE 0x2u /* RX: dequeue would succeed */
#define MTL_WAIT_RESULTS 0x4u /* TX: reap would return a result */
#define MTL_WAIT_EVENTS 0x8u  /* events are pending (mtl_queue.h) */
#define MTL_WAIT_RETIRED 0x10u /* a close that returned 1 finished */
#define MTL_WAIT_RTCP 0x20u    /* RX: mtl_rtcp_read() would return a report (mtl_rtcp.h) */
/* > 0: the ready subset of mask. Nothing ready by the timeout: -MTL_EAGAIN, and the
   targets are armed, so block on the wait handle. A closed handle answers RETIRED until
   it retired, then MTL_WAIT_RETIRED. -MTL_ECANCELED, -MTL_ESHUTDOWN, ... WT. */
MTL_API_WT int mtl_session_wait(mtl_session_h s, uint64_t mask, int64_t timeout_ns);
/* The session's one wait handle for an event loop: a Linux eventfd (poll POLLIN; data
   calls drain it, R2) or a Windows auto-reset event HANDLE. It fires for the armed targets
   in the union of the masks ever requested. CP. */
MTL_API_CP int mtl_session_get_wait_handle(mtl_session_h s, uint64_t mask, intptr_t* native);
/* on = 1: every data wait on s returns -MTL_ECANCELED until on = 0 (GStreamer unlock and
   unlock_stop). AS with on = 1, CP with on = 0. */
MTL_API_AS int mtl_session_interrupt(mtl_session_h s, int on);

/* ---- Size checks (64-bit targets) ---------------------------------------------------------- */

MTL_SIZE_CHECK(mtl_error_info, 184);
MTL_SIZE_CHECK(mtl_instance_h, 8);
MTL_SIZE_CHECK(mtl_session_h, 8);
MTL_SIZE_CHECK(mtl_lease_h, 8);
MTL_SIZE_CHECK(mtl_region_h, 8);
MTL_SIZE_CHECK(mtl_timeline_h, 8);
MTL_SIZE_CHECK(mtl_object, 16);
MTL_SIZE_CHECK(mtl_rational, 16);
MTL_SIZE_CHECK(mtl_option, 24);
MTL_SIZE_CHECK(mtl_port_spec, 128);
MTL_SIZE_CHECK(mtl_instance_params, 120);
MTL_SIZE_CHECK(mtl_flow, 80);
MTL_SIZE_CHECK(mtl_raster, 32);
MTL_SIZE_CHECK(mtl_video_config, 96);
MTL_SIZE_CHECK(mtl_cvideo_config, 96);
MTL_SIZE_CHECK(mtl_audio_config, 64);
MTL_SIZE_CHECK(mtl_anc_config, 64);
MTL_SIZE_CHECK(mtl_fastmeta_config, 64);
MTL_SIZE_CHECK(mtl_rtp_config, 112);
MTL_SIZE_CHECK(mtl_packet_config, 64);
MTL_SIZE_CHECK(mtl_session_config, 936);
MTL_SIZE_CHECK(mtl_leg_info, 28);
MTL_SIZE_CHECK(mtl_session_info, 256);
MTL_SIZE_CHECK(mtl_when, 24);
MTL_SIZE_CHECK(mtl_leg_status, 16);
MTL_SIZE_CHECK(mtl_session_status, 104);
MTL_SIZE_CHECK(mtl_plane, 24);
MTL_SIZE_CHECK(mtl_unit, 208);
MTL_SIZE_CHECK(mtl_meta_hdr, 16);
MTL_SIZE_CHECK(mtl_anc_packet, 16);
MTL_SIZE_CHECK(mtl_tx_result, 96);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_H */
