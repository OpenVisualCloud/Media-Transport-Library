/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl.h - the unified MTL API, core header, experimental revision 0.2 (design sketch).
 *
 * STATUS: design sketch (doc/unified-api/), the normative input of MS1. Nothing is
 * implemented. This file is normative for names, types, layouts and call classes; where
 * the prose disagrees, the header wins (sketch/README.md).
 *
 * libmtl exports this API in symbol version nodes per milestone, and a node that a release
 * carries never changes again (migration.md §7.2). A function is exported from the milestone its comment ends with, (MS1) to (MS7), which is
 * also the argument of its call-class macro, MTL_API_CP(n) and the others; this header
 * declares the whole design, and a call to a function of a milestone above MTL_LEVEL fails
 * to compile, naming the milestone (the availability block below). A function tagged
 * (Phase 7) or (later) is declared under MTL_LATER only, which is for design checks. A
 * value of an exported call (a flag, an enum value, an option key, a port prefix, a when
 * kind, a wait mask, an update part) is declared from the start and returns -MTL_ENOTSUP
 * until its milestone (R1).
 *
 * The core is what a media application needs: open an instance, create a session for any
 * essence (video, compressed video, audio, ANC, fast metadata, generic RTP) in frame, row or
 * packet units, start and stop it, move units in and out, wait, and read why something
 * failed. Everything else is an optional header that includes this one:
 *
 *   mtl_mem.h      imported memory, attached pools, named slots
 *   mtl_sync.h     media-index arithmetic, the next TX unit, clocks and the time reference
 *   mtl_events.h   the events of a session and of an instance
 *   mtl_packet.h   packet tables, RTP header layouts and the RFC 8331 codec for
 *                  unit = MTL_UNIT_PACKETS
 *   mtl_observe.h  stats registry, full result records, diagnostics, logging, capture,
 *                  health and shutdown
 *   mtl_options.h  option keys for every tuning knob (key enum from mtl_options.def)
 *   mtl_reasons.h  the reason codes (enum from reasons.def)
 *   mtl_format.h   application formats, format descriptions, standalone conversion
 *   mtl_util.h     inline helpers built only on public calls (copy path, meta, ANC, RX reserve)
 *   mtl_ipmx.h     RTCP sender reports and the Info Block, payload encryption (Phase 7)
 *   mtl_sdp.h      SDP render and parse, in the companion library libmtl_sdp (Phase 7)
 *   mtl_plugin.h   codec and converter plugin ABI
 *   mtl_legacy.h   legacy mtl_handle, legacy enum values
 *   mtl_debug.h    tests only
 *
 * Rules
 *
 * R1  Returns: 0 or a count/mask > 0 on success, a negative MTL_E* code on failure. A known
 *     value that is not implemented yet (a flag, enum value, update part, when kind, wait
 *     mask, option key or port prefix) is -MTL_ENOTSUP with reason NOT_IMPLEMENTED and the
 *     item in mtl_last_error().field; an unknown value is -MTL_EINVAL; an output field not
 *     computed yet is zero with its VALID flag clear. A call that succeeds never writes
 *     mtl_last_error() (errno semantics).
 * R2  Timeouts are the last argument, int64_t ns: 0 = do not wait, MTL_FOREVER = no limit;
 *     a timeout below MTL_FOREVER is -MTL_EINVAL. A timeout is a duration on
 *     CLOCK_MONOTONIC fixed at entry, never changed by a step of the time base. A data call
 *     (acquire, dequeue, reap, read, wait) that finds nothing, now or by its timeout, returns
 *     -MTL_EAGAIN; a read that returns a count returns at least 1.
 *     With timeout 0 a data call tries once and changes no wait state. A call with a
 *     timeout sleeps on its object, so any number of threads may wait on any targets of
 *     one object; an event loop waits on a queue (mtl_queue_create, MS2). An application
 *     that never sleeps never causes a wake-up syscall. A completing tasklet never makes
 *     the syscall itself: its scheduler wakes after its handler loop. -MTL_EAGAIN sets only code and
 *     reason in mtl_last_error(). -MTL_ETIMEDOUT is for control-plane deadlines (stop).
 * R3  Input structs start with uint32_t struct_size. MTL_INIT(&s) zero-fills a struct and
 *     sets its struct_size; zero in every other field is the default (struct_size 0 is
 *     -MTL_EINVAL). The library reads min(struct_size, known); unknown non-zero bytes are
 *     -MTL_EINVAL. Output structs carry no struct_size: the library fills them up to the
 *     size argument and zeroes what it does not know. A struct the library writes and reads
 *     back (mtl_unit) uses its own struct_size. Every pointer in an input struct is read
 *     during the call and deep-copied (strings included); the caller may free it on return.
 *     Embedded structs are fixed size.
 * R4  Handles are 64-bit values of distinct types; 0 is the null handle of every type and
 *     a closed handle is never reissued. A call with an out handle writes the null handle
 *     on failure. Close is idempotent: it returns 1 while the object retires and 0 once it
 *     retired, also when called again on the same handle and for a null handle; no other
 *     call is valid on a closing handle except mtl_session_get_status (CLOSING, RETIRED),
 *     mtl_release of a lease taken before the close (retirement waits for it),
 *     mtl_interrupt (AS; a no-op that returns 0), and on a queue an mtl_queue_wait already
 *     in progress, which returns -MTL_ESHUTDOWN (a later call on the handle: -MTL_EBADF). A
 *     stale or foreign handle fails with -MTL_EBADF, a lease already returned with
 *     -MTL_ESTALE; the one exception is mtl_instance_get_health on an instance consumed by
 *     close or shutdown, which returns -MTL_ESHUTDOWN, so a probe racing the close sees
 *     "shutting down". Handle entries are process-wide and never freed, so a handle stays safe
 *     to pass after its instance is gone: on an object the instance closed (deployment.md),
 *     data calls return -MTL_ESHUTDOWN, and its close and a lease's release return 0. A
 *     buffer is named by its pool slot index, never by a handle.
 * R5  Times are int64_t ns since 1970-01-01 TAI on the instance clock, valid only when their
 *     flag says so: TAI while the time base is locked, flagged ESTIMATED when it is not (a
 *     clock without PTP), and only a time common to the PTP domain when the grandmaster
 *     runs an ARB timescale (MTL_TIMEF_ARB_TIMESCALE), and UTC read as TAI where a
 *     legacy clock or the PTP client before its first Announce gives UTC (MTL_TIMEF_UTC);
 *     an RX value on the sender's clock
 *     is flagged SENDER_TIME (Phase 7, mtl_ipmx.h).
 * R6  Call classes: CP control plane (may allocate and block); DP data plane (O(1), no
 *     allocation, lock a tasklet takes or logging; its only syscalls are one wake when it
 *     makes a target ready for a sleeper (a futex wake, or a write() of a queue);
 *     DP, WT and AS calls are not cancellation points); DPC data plane that does work in
 *     the caller (copy, conversion); WT wait, DP when timeout is 0 (mtl_queue_wait's
 *     timeout 0 arms its queue); AS async-signal-safe
 *     (atomics, the futex call and write() only; errno kept; never mtl_last_error()). An
 *     application thread may busy-poll the DP calls and the WT calls with timeout 0. No
 *     application code ever runs on an MTL tasklet, and the library
 *     calls application code only from the one log thread that runs the log sinks
 *     (mtl_log_add_sink(), mtl_observe.h) and, for codec plugins, from their own threads.
 *     A wrapper of a legacy instance adds the one exception, stated in mtl_legacy.h, and
 *     the exception ends with that header, which is not part of MTL_1.0. Those threads may
 *     not open, close or shut down the instance (close and shutdown join them):
 *     -MTL_EDEADLK.
 * R7  Tuning knobs are options (mtl_options.h): absent means the documented default.
 * R8  Process: the library installs no signal handler and no atexit (DPDK's own SIGBUS
 *     handlers, held while it grows its heap and installed by instance.hotplug, are the
 *     exceptions); a signal handler
 *     may call the AS functions only, at any time, also during and after close. Every
 *     descriptor MTL or DPDK opens for it is close-on-exec. An instance belongs to the
 *     process that opened it: in a fork()ed child MTL closes the descriptors it tracks
 *     (VFIO, MtlManager, CPU locks, queue descriptors) at once, every call returns -MTL_EBADF
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
#define MTL_NULLABLE /* the pointer may be NULL */
/* The result must be read (mtl_mem_close). GCC warns even through a (void) cast in C and
   in C++ before C++17 (GCC bug 66425), so only a result that guards memory carries it. */
#if defined(SWIG) || defined(__bindgen)
#define MTL_MUST_CHECK
#elif defined(__cplusplus) && __cplusplus >= 201703L
#define MTL_MUST_CHECK [[nodiscard]]
#elif defined(__GNUC__)
#define MTL_MUST_CHECK __attribute__((warn_unused_result))
#else
#define MTL_MUST_CHECK
#endif
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

/* ---- Availability: call class and milestone (OI-64) ----------------------------- */

/* MTL_LEVEL: the last milestone whose exit this header and its library passed (0 before the
   MS1 exit). Every function declared MTL_API_<class>(n) with n <= MTL_LEVEL is exported, in
   the version node of milestone n; one of a later milestone is not, or not yet. With GCC and
   Clang 14 or later, a call to a function above MTL_TARGET_LEVEL fails to compile, naming
   the function and its milestone: the error attribute acts on the calls left in the
   generated code, so an inline helper that is not used never fails. Through a helper used
   by the program, the error points at the helper's line in the header and names the
   function the helper calls; GCC at -O1 and above also names each calling function and
   line ("inlined from"). An indirect call, or a call in code the optimiser removes,
   may be reported at one -O level and not at another; the link fails either way. MSVC,
   older Clang, SWIG, bindgen and -fsyntax-only report nothing, and the call fails at link
   time. MTL_TARGET_LEVEL (0 to 8, default MTL_LEVEL): lower, to find the calls a library at
   that level lacks; MTL_LEVEL + 1 for the library's own tree, which builds the milestone
   under way: libmtl, its unit tests and each in-tree consumer project, set in its own
   meson.build, never in mtl.pc (implementation-plan.md §4). A build that emits unused inline functions (GCC -fkeep-inline-functions, Clang
   -femit-all-decls) sets MTL_TARGET_LEVEL to 7: every helper is then compiled, and a later
   call fails at link time. Declarations under MTL_LATER (design checks only) take LATER:
   a call fails to compile where the attribute exists, else to link; names under MTL_LATER
   may change in any release. After the freeze, n = 8 and up are reserved for MTL_1.(n - 7)
   (MTL_SINCE_8_ and a wider range are added then). The exits that set MTL_LEVEL are those
   of MS1, MS2b (2), MS3, MS4b (4), MS5, MS6 and MS7; the exits of MS2a and MS4a leave it
   unchanged. A release cut while milestone MTL_LEVEL + 1 is open exports the functions of
   that milestone built so far: MTL_TARGET_LEVEL = MTL_LEVEL + 1 compiles every call to
   them, and the link against that release fails for any it lacks. Every version node a
   release carries is sealed, so ld.so refuses at load, never at a call, a library that
   lacks a function the binary was linked with. The macros ending in "_" are internal. */
#define MTL_LEVEL 0
#ifndef MTL_TARGET_LEVEL
#define MTL_TARGET_LEVEL MTL_LEVEL
#endif
#if MTL_TARGET_LEVEL < 0 || MTL_TARGET_LEVEL > 8
#error "MTL_TARGET_LEVEL is 0 to 8"
#endif
#define MTL_STR_(x) #x
#define MTL_XSTR_(x) MTL_STR_(x)
#define MTL_NOT_YET_(n)
#if !defined(SWIG) && !defined(__bindgen) && defined(__has_attribute)
#if __has_attribute(error)
#undef MTL_NOT_YET_
#define MTL_NOT_YET_(n)                                                                    \
  __attribute__((error("MS" #n " function, above MTL_TARGET_LEVEL (this mtl.h: MTL_LEVEL " \
                       MTL_XSTR_(MTL_LEVEL) "); define MTL_TARGET_LEVEL=" #n               \
                       " only against a library that exports it")))
#endif
#endif
#if MTL_TARGET_LEVEL >= 1
#define MTL_SINCE_1_
#else
#define MTL_SINCE_1_ MTL_NOT_YET_(1)
#endif
#if MTL_TARGET_LEVEL >= 2
#define MTL_SINCE_2_
#else
#define MTL_SINCE_2_ MTL_NOT_YET_(2)
#endif
#if MTL_TARGET_LEVEL >= 3
#define MTL_SINCE_3_
#else
#define MTL_SINCE_3_ MTL_NOT_YET_(3)
#endif
#if MTL_TARGET_LEVEL >= 4
#define MTL_SINCE_4_
#else
#define MTL_SINCE_4_ MTL_NOT_YET_(4)
#endif
#if MTL_TARGET_LEVEL >= 5
#define MTL_SINCE_5_
#else
#define MTL_SINCE_5_ MTL_NOT_YET_(5)
#endif
#if MTL_TARGET_LEVEL >= 6
#define MTL_SINCE_6_
#else
#define MTL_SINCE_6_ MTL_NOT_YET_(6)
#endif
#if MTL_TARGET_LEVEL >= 7
#define MTL_SINCE_7_
#else
#define MTL_SINCE_7_ MTL_NOT_YET_(7)
#endif
#define MTL_SINCE_LATER_ MTL_NOT_YET_LATER_
#define MTL_NOT_YET_LATER_
#if !defined(SWIG) && !defined(__bindgen) && defined(__has_attribute)
#if __has_attribute(error)
#undef MTL_NOT_YET_LATER_
#define MTL_NOT_YET_LATER_ __attribute__((error("not implemented: Phase 7 or later (MTL_LATER)")))
#endif
#endif
/* Call classes (R6), each with the milestone n of the function: 1 to 7, or LATER. */
#define MTL_API_CP(n) MTL_API MTL_SINCE_##n##_
#define MTL_API_DP(n) MTL_API MTL_SINCE_##n##_
#define MTL_API_DPC(n) MTL_API MTL_SINCE_##n##_
#define MTL_API_WT(n) MTL_API MTL_SINCE_##n##_
#define MTL_API_AS(n) MTL_API MTL_SINCE_##n##_

/* ---- Versions, limits, time units ----------------------------------------------- */

#define MTL_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
#define MTL_API_VERSION MTL_VERSION_NUM(0, 2, 0)

#define MTL_FOREVER ((int64_t)-1)
#define MTL_US(x) ((int64_t)(x)*1000)
#define MTL_MS(x) ((int64_t)(x)*1000000)
#define MTL_SEC(x) ((int64_t)(x)*1000000000)

#define MTL_MAX_LEGS 2   /* ST 2022-7 legs */
#define MTL_MAX_PLANES 4 /* planes per unit */
/* Zero bytes after every video and cvideo RX library-pool unit at dequeue (FFmpeg's
   AV_INPUT_BUFFER_PADDING_SIZE), inside the slot (contract §9.1) */
#define MTL_RX_TAIL_BYTES 64
#define MTL_NAME_MAX 64  /* names, NUL included */
/* An index that may be absent (a field, argument or output where 0 means the default, all
   or none: a port, a scheduler, a NUMA node, an option scope) holds MTL_INDEX(i) = i + 1.
   An index that is always present (object references, event indices, leg numbers in
   records, bit positions, mtl_leg_info.port) is 0-based. An option value is literal
   (session.numa is the node). */
#define MTL_INDEX(i) ((uint32_t)(i) + 1u)

/* ---- Errors: Linux errno values on every OS, one meaning each -------------------- */

#define MTL_EIO 5         /* it failed: a session in ERROR, or a device that could not be
                             stopped; the reason says why */
#define MTL_EBADF 9       /* null, foreign or closed handle */
#define MTL_EAGAIN 11     /* nothing now, or by the timeout */
#define MTL_ENOMEM 12     /* allocation failed */
#define MTL_EBUSY 16      /* in use, or not allowed in this state */
#define MTL_EEXIST 17     /* name in use with another configuration */
#define MTL_ENODEV 19     /* device missing or removed, or its reset failed */
#define MTL_EINVAL 22     /* invalid argument; mtl_last_error() names the field */
#define MTL_ENOSPC 28     /* capacity: queues, lcores, regions, payload size */
#define MTL_ERANGE 34     /* time outside the accepted window */
#define MTL_EDEADLK 35    /* not allowed from this thread: a library thread that the call
                             would join or wait for */
#define MTL_ENOTSUP 95    /* capability or option absent */
#define MTL_ESHUTDOWN 108 /* the application stopped or closed the session or instance */
#define MTL_ETIMEDOUT 110 /* a control-plane deadline passed (a DRAIN stop) */
#define MTL_ESTALE 116    /* a lease that was already returned */
#define MTL_ECANCELED 125 /* data waits interrupted; sticky until interrupt(..., 0) */

/* The calling thread's last failure, kept until its next failing call: a call that
   succeeds never writes it (R1), so cleanup after a failure cannot clobber it. */
struct mtl_error_info {
  int32_t code;    /* negative MTL_E*; 0 = none */
  uint32_t reason; /* mtl_reasons.h */
  char field[48];  /* the config field or option at fault, "" if none */
  char detail[128];
};
/* Copies it into out, up to size. DP. (MS1) */
MTL_API_DP(1) int mtl_last_error(struct mtl_error_info* out, size_t size);
/* The name of a reason: "TOO_LATE". AS. (MS1) */
MTL_API_AS(1) const char* mtl_reason_name(uint32_t reason);
/* "MTL_EINVAL" for MTL_EINVAL or -MTL_EINVAL. */
static inline const char* mtl_error_name(int code) {
  switch (code < 0 ? -code : code) {
    case 0: return "OK";
    case MTL_EIO: return "MTL_EIO";
    case MTL_EBADF: return "MTL_EBADF";
    case MTL_EAGAIN: return "MTL_EAGAIN";
    case MTL_ENOMEM: return "MTL_ENOMEM";
    case MTL_EBUSY: return "MTL_EBUSY";
    case MTL_EEXIST: return "MTL_EEXIST";
    case MTL_ENODEV: return "MTL_ENODEV";
    case MTL_EINVAL: return "MTL_EINVAL";
    case MTL_ENOSPC: return "MTL_ENOSPC";
    case MTL_ERANGE: return "MTL_ERANGE";
    case MTL_EDEADLK: return "MTL_EDEADLK";
    case MTL_ENOTSUP: return "MTL_ENOTSUP";
    case MTL_ESHUTDOWN: return "MTL_ESHUTDOWN";
    case MTL_ETIMEDOUT: return "MTL_ETIMEDOUT";
    case MTL_ESTALE: return "MTL_ESTALE";
    case MTL_ECANCELED: return "MTL_ECANCELED";
    default: return "unknown";
  }
}
/* The running library, against MTL_API_VERSION of the header compiled: "26.09.0 (git ...,
   gcc ...)"; *num (may be NULL) receives its MTL_VERSION_NUM. Not mtl_version: the legacy
   mtl_api.h has that name. AS. (MS1) */
MTL_API_AS(1) const char* mtl_library_version(uint32_t* MTL_NULLABLE num);
static inline uint32_t mtl_library_version_num(void) {
  uint32_t num = 0;
  mtl_library_version(&num);
  return num;
}

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
#if defined(MTL_LATER)
/* An exact clock origin shared by sessions (mtl_sync.h); null = the SMPTE epoch. */
typedef struct mtl_timeline_h {
  uint64_t id;
} mtl_timeline_h;
#endif

#if defined(__cplusplus)
#define MTL_NULL(T) (T{})
#else
#define MTL_NULL(T) ((T){0})
#endif
#define MTL_IS_NULL(h) ((h).id == 0)
/* Equality of two lvalue handles of one type: mixing types warns in C (an error with
   -Werror) and does not compile in C++. Compare with null through MTL_IS_NULL. */
#define MTL_SAME(a, b) ((void)sizeof(&(a) == &(b)), (a).id == (b).id)

/* A reference to any object, for the object verbs below, events, stats, options and fault
   injection. */
enum mtl_object_kind {
  MTL_OBJ_INSTANCE = 1,
  MTL_OBJ_PORT = 2, /* index = port, id = the instance */
  MTL_OBJ_SESSION = 3,
#if defined(MTL_LATER)
  MTL_OBJ_TIMELINE = 4,
#endif
  MTL_OBJ_REGION = 5,
  MTL_OBJ_SCHED = 6, /* index = scheduler, id = the instance */
  MTL_OBJ_QUEUE = 7, /* mtl_queue_create (MS2) */
  MTL_OBJ_PLUGIN = 8, /* mtl_plugin.h */
  MTL_OBJ_LOG_SINK = 9, /* mtl_observe.h; closed by mtl_log_remove_sink() */
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

/* Zero-fills size bytes and sets struct_size (the first uint32_t) = size. Bindings do the
   same in their own language. */
static inline void mtl_struct_init(void* p, size_t size) {
  unsigned char* b = (unsigned char*)p;
  for (size_t i = 0; i < size; i++) b[i] = 0;
  *(uint32_t*)p = (uint32_t)size;
}
#define MTL_INIT(p) mtl_struct_init((p), sizeof(*(p)))

/* ---- Object verbs ---------------------------------------------------------------- */

/* One exported function per verb, over every object kind that has it; a kind without the
   verb is -MTL_EINVAL. Typed static inline wrappers keep each object's own name
   (mtl_session_close(s, t) is mtl_close(MTL_OBJ_OF_SESSION(s), t)), and the rules of a
   verb for a kind are stated at its wrapper. Waiting, reaping and releasing are in the
   data path below; reading events is mtl_read_events() (mtl_events.h). */
/* What close returns while the object retires; call close again to poll. */
#define MTL_RETIRING 1
/* Closes o: an instance, session, region, plugin, log sink or queue. A queue closes at
   once (0) and never reads timeout_ns. MTL_RETIRING: still retiring; 0: retired. Calling it again on the same handle polls (0 or MTL_RETIRING),
   never -MTL_EBADF; 0 for a null object. Retirement needs no further call: the last
   release or reference completes it. From the first call on, the only other valid calls
   on the handle are mtl_session_get_status, mtl_release of a lease taken before the close
   and mtl_interrupt (a no-op) (R4). timeout_ns bounds the instance and session closes and
   the wait for a running log-sink call; regions and plugins never wait and do not read
   it. CP. (MS1) */
MTL_API_CP(1) int mtl_close(struct mtl_object o, int64_t timeout_ns);
/* mtl_interrupt() modes */
#define MTL_INTR_OFF 0u   /* clear the selected targets: their waits work again (CP) */
#define MTL_INTR_ON 1u    /* the selected waits return -MTL_ECANCELED until OFF, sticky (AS) */
#define MTL_INTR_ABORT 2u /* instance only, targets 0: the emergency stop of
                             mtl_instance_abort() (AS) */
/* Interrupts the waits of o on targets (MTL_WAIT_*; 0 = every target): on a session its
   own, on an instance its own and the selected waits of every session. While ON, every
   call with a timeout on a selected target, and mtl_wait() with any timeout, returns
   -MTL_ECANCELED; a call blocked in one returns at once; on a queue (targets 0) every
   mtl_queue_wait returns -MTL_ECANCELED; an instance's interrupt reaches the waits of its
   sessions and of its queues; a session's interrupt does not reach its queues. A
   GStreamer unlock interrupts
   acquire (MTL_INTR_ON, MTL_WAIT_ACQUIRE) without making a reaper of the same session
   spin. Stop and close still work. -MTL_EINVAL for an unknown mode, a bit not declared
   outside MTL_LATER, ABORT with targets, ABORT on a session or a queue, and targets on a
   queue; -MTL_ENOTSUP for a
   declared bit of a later milestone (R1). These are checked first, so on a closing or
   retired handle a valid call is a no-op that returns 0 (R4); -MTL_EBADF for a stale
   generation or in a fork()ed child. AS with ON and ABORT: atomics, the futex call and
   write() only, errno kept, never mtl_last_error(). CP with OFF. (MS1) */
MTL_API_AS(1) int mtl_interrupt(struct mtl_object o, uint32_t mode, uint64_t targets);

/* ---- Common enumerations --------------------------------------------------------- */

enum mtl_dir {
  MTL_TX = 1,
  MTL_RX = 2,
};

/* One severity scale, syslog order with legacy enum mtl_log_level + 1: the log sinks
   (mtl_log_record.severity, min_severity), mtl_instance_params.log_level and
   mtl_event.severity (INFO, WARNING, ERR). */
enum mtl_severity {
  MTL_SEV_DEBUG = 1,
  MTL_SEV_INFO = 2,
  MTL_SEV_NOTICE = 3,
  MTL_SEV_WARNING = 4,
  MTL_SEV_ERR = 5,
  MTL_SEV_CRIT = 6,
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

/* What a TX unit's media time is (timing.md). RTP = floor(M x rate). */
enum mtl_media_mode {
  MTL_MEDIA_AUTO = 0,  /* the next feasible media index */
  MTL_MEDIA_INDEX = 1, /* unit.media_index: from the epoch, or from T0 (MTL_WHEN_ORIGIN) */
  MTL_MEDIA_TAI = 2,   /* unit.media_tai_ns, snapped to the grid */
#if defined(MTL_LATER)
  /* unit.media_tai_ns is the source's own sampling instant on the instance clock, never
     snapped: an async source (IPMX mediaclk:sender). RTP follows the source: RTP = RTP0 +
     floor(k x period x rate), RTP0 = floor(M0 x rate) at the first unit or after a
     DISCONTINUITY, and k advances by max(1, round((M - M_prev) / period)) per unit, so a
     missed VSYNC skips one period without drift building up. Launch = M + min_tx_delay_ns
     on the nominal-period schedule; a unit that would overlap the previous one is DROPPED
     (WOULD_OVERLAP); sender reports carry M (nmos-ipmx.md). AT_INDEX, RX_BY_INDEX and
     tx.precede targets are -MTL_EINVAL on such a session; a result's media_index is k.
     Phase 7. */
  MTL_MEDIA_SENDER = 3,
#endif
};

enum mtl_state {
  MTL_STATE_CREATED = 1,
  MTL_STATE_ARMED = 2, /* start instant ahead */
  MTL_STATE_RUNNING = 3,
  MTL_STATE_DRAINING = 4,
  MTL_STATE_FLUSHING = 5,
  MTL_STATE_STOPPED = 6,
  MTL_STATE_ERROR = 7,      /* only stop and close leave it; data calls fail (contract §4.2) */
  MTL_STATE_CLOSING = 8,    /* closed, retiring: leases or device references remain */
  MTL_STATE_RETIRED = 9,    /* closed and retired; the handle stays retired */
};

enum mtl_stop_mode {
  MTL_STOP_DRAIN = 0, /* send every queued unit at its launch index */
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
   source is SYSTEM_TAI, or from MS6 MTL_TIME_SOURCE_FREERUN (Phase 7: also AUTO that fell
   back to free run); LOCKED only for PTP_BUILTIN, PHC, CLOCK_TAI and USER sources. */
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
/* the grandmaster runs an ARB timescale (clockClass 220 or 228, or timeSource F0h, ST
   2059-2 §5.5.4; mtl_time_reference): times are common to its PTP domain, not TAI */
#define MTL_TIMEF_ARB_TIMESCALE 0x8u
/* the value is UTC read as TAI (37 s behind it today): CLOCK_REALTIME as a legacy instance's
   default clock, its PTP_SOURCE_TSC, a legacy ptp_get_time_fn found to follow
   CLOCK_REALTIME, or the built-in PTP client before its first Announce. Media times and RTP
   of the instance's sessions are on that scale too. Always with ESTIMATED; never with
   ARB_TIMESCALE */
#define MTL_TIMEF_UTC 0x10u
/* TAI now from the published time base (a wrapper of a legacy instance: from a snapshot of
   the legacy clock, mtl_legacy.h); returns MTL_TIMEF_* flags (>= 0). monotonic_ns
   and realtime_ns (may be NULL) receive CLOCK_MONOTONIC and CLOCK_REALTIME of the same
   instant: one consistent sample, for frameworks with their own clock. DP. (MS1) */
MTL_API_DP(1) int mtl_time_now(mtl_instance_h mt, int64_t* tai_ns,
                               int64_t* MTL_NULLABLE monotonic_ns,
                               int64_t* MTL_NULLABLE realtime_ns);

/* An exact fraction. As a rate (raster.fps, rtp.unit.fps) the library reduces it to lowest
   terms and accepts it only when the reduced value has num <= 4194303 and den <= 1023, the
   widths of the rate fields of the IPMX Media Info Block (VSF TR-10-2 §10); then num x den
   < 2^32 and every conversion between ns, indices and ticks is exact in 64-bit integers
   (timing.md §3.5).
   {0, 0} means "not set"; exactly one zero term is -MTL_EINVAL. */
struct mtl_rational {
  uint64_t num;
  uint64_t den;
};
/* Named frame rates (frames, not fields: 1080i59.94 is MTL_FPS_29_97), the inputs of
   mtl_fps_rational(): enum st_fps + 1, so 0 stays "not set"; 47.95 and 48 are new. A
   raster carries only the rational. */
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
/* The exact rate of a named one; {0, 0} if unknown. */
static inline struct mtl_rational mtl_fps_rational(uint32_t fps) {
  struct mtl_rational r = {0, 0};
  switch (fps) {
    case MTL_FPS_59_94: r.num = 60000; r.den = 1001; break;
    case MTL_FPS_50: r.num = 50; r.den = 1; break;
    case MTL_FPS_29_97: r.num = 30000; r.den = 1001; break;
    case MTL_FPS_25: r.num = 25; r.den = 1; break;
    case MTL_FPS_119_88: r.num = 120000; r.den = 1001; break;
    case MTL_FPS_120: r.num = 120; r.den = 1; break;
    case MTL_FPS_100: r.num = 100; r.den = 1; break;
    case MTL_FPS_60: r.num = 60; r.den = 1; break;
    case MTL_FPS_30: r.num = 30; r.den = 1; break;
    case MTL_FPS_24: r.num = 24; r.den = 1; break;
    case MTL_FPS_23_98: r.num = 24000; r.den = 1001; break;
    case MTL_FPS_47_95: r.num = 48000; r.den = 1001; break;
    case MTL_FPS_48: r.num = 48; r.den = 1; break;
    default: break;
  }
  return r;
}

/* ---- Options ---------------------------------------------------------------------- */

/* One tuning knob: key from mtl_options.h. scope: 0 = every port, leg or scheduler the
   key applies to (or not scoped), else MTL_INDEX(i); mtl_option_desc says which kind. A
   later duplicate (key, scope) wins. String keys use str, the others value. */
struct mtl_option {
  uint32_t key;
  uint32_t scope;
  int64_t value;
  const char* MTL_NULLABLE str;
};

/* ---- Instance --------------------------------------------------------------------- */

/* One port. name: PCI BDF ("0000:af:01.0"), "kernel:<ifname>", "native_af_xdp:<ifname>",
   "null:<id>", or "env:<VAR>[#n]": the n-th (from 0) PCI address in the environment
   variable VAR, as the Kubernetes SR-IOV device plugin sets PCIDEVICE_<resource>
   (-MTL_EINVAL, PORT_ENV_UNSET, if absent). The DPDK AF_XDP and AF_PACKET PMDs are
   removed: a name "dpdk_af_xdp:" or "dpdk_af_packet:" fails open with -MTL_ENOTSUP
   (BACKEND_REMOVED; use "native_af_xdp:" or "kernel:").
   "null:<id>": one null port (id 0-255, a label): no NIC, no root, no hugepages; units
   complete on the instance clock. An
   instance whose ports are all null never calls the legacy init: it initialises EAL
   itself (no hugepages, no PCI, in memory) or joins an EAL the process already
   initialised, and runs one library thread as its loop instead of pinned schedulers. Its
   RX sessions receive the TX units of the same instance whose flow (destination IP and
   UDP port) they match: loopback without packets.
   Addresses: IPv4 in bytes 0..3, the rest zero. MTL never sets a port's MAC: a VF keeps
   the one its PF (or the CNI) gave it. */
struct mtl_port_spec {
  char name[MTL_NAME_MAX];
  uint8_t ip_family;  /* 0 = IPv4, 6 = IPv6 */
  uint8_t prefix_len; /* 0 = 24 (IPv6: 64) */
  uint8_t reserved0[2];
  uint8_t sip[16];     /* source address; all zero = DHCP on kernel backends */
  uint8_t gateway[16]; /* all zero = none */
  uint32_t tx_queues;  /* 0 = auto */
  uint32_t rx_queues;  /* 0 = auto */
  uint32_t numa;       /* 0 = the device's socket, else MTL_INDEX(node) */
  uint8_t mac[6];      /* output of mtl_port_get_spec(): the MAC on the wire; ignored on input */
  uint8_t reserved1[2];
  uint32_t reserved[10];
};
/* MTL_PORTS text to port specs, the parser mtl_instance_open() uses for MTL_PORTS
   (contract §2.2): ports separated by ','; spaces and tabs around a port ignored; port =
   name ["=" addr ["/" prefix] ["@" gateway]]; addr and gateway IPv4 dotted or "[IPv6]",
   one family; prefix 1-32 (IPv6 1-128), absent = 24 (IPv6 64). name, at most 63 bytes,
   none of ',', '=', '@' or whitespace: a PCI BDF, "kernel:<ifname>",
   "native_af_xdp:<ifname>", "null:<id>" (id 0-255: one null port; "null:0,null:1" is two)
   or "env:<VAR>[#n]" (resolved by open); another prefix is copied for open to judge. The
   name is stored normalised (a BDF in lower case with its domain, "af:01.0" as
   "0000:af:01.0"; a null id in decimal without leading zeros) and duplicates are found on
   it. Without "=addr" sip stays zero (open: DHCP on the kernel backends, kernel: and
   native_af_xdp:; -MTL_EINVAL on a PCI port without port.dhcp; null ports need none).
   Each spec written is zeroed first; a port's index is its position. *n = ports in text
   (0 for "" or whitespace only), at most cap written (specs NULL with cap 0 counts them).
   -MTL_EINVAL (field "ports", detail the position and the token) for a syntax error, an
   invalid address or prefix, a duplicate name or an empty port (",,", a trailing ',').
   Reads no environment and calls no locale-dependent function. AS. (MS2) */
MTL_API_AS(2) int mtl_port_parse(const char* text, struct mtl_port_spec* MTL_NULLABLE specs,
                                 uint32_t cap, uint32_t* n);

#define MTL_INSTANCE_SHARED 0x1u /* the refcounted process-wide instance (see open) */
#define MTL_INSTANCE_TASKLET_THREAD 0x4u /* schedulers are pinned pthreads, not EAL lcores */
#define MTL_INSTANCE_TASKLET_SLEEP 0x8u  /* idle schedulers sleep */
#define MTL_INSTANCE_HW_TIMESTAMP 0x10u  /* enable NIC timestamps; sessions may require them */

/* MTL adjusts a clock only with PTP_BUILTIN: the PHC of a port it owns (a PF), or, on a
   VF, its own software time base (as today; the VF's PHC is never steered); never
   CLOCK_REALTIME, never a clock a node daemon disciplines (deployment.md). AUTO chooses
   once, at open (Phase 7: it re-evaluates while running and falls back without a step,
   nmos-ipmx.md). */
enum mtl_time_source {
  MTL_TIME_SOURCE_AUTO = 0,        /* CLOCK_TAI when its kernel offset is set, else
                                      SYSTEM_TAI, both read through the vDSO; from MS6 a
                                      disciplined NIC PHC first, through the published
                                      time base */
  MTL_TIME_SOURCE_PTP_BUILTIN = 1, /* MTL's own PTP client (named only, never by AUTO) */
  MTL_TIME_SOURCE_PHC = 2,         /* ptp4l/phc2sys discipline the NIC PHC */
  MTL_TIME_SOURCE_CLOCK_TAI = 3,   /* CLOCK_TAI; rejected if the kernel offset is 0 */
  MTL_TIME_SOURCE_SYSTEM_TAI = 4,  /* CLOCK_REALTIME + UTC offset, ESTIMATED, follows NTP */
  MTL_TIME_SOURCE_USER = 5,        /* the app feeds it (mtl_sync.h) */
#if defined(MTL_LATER)
  MTL_TIME_SOURCE_FREERUN = 6,     /* seeded once from the system clock, never stepped,
                                      ESTIMATED: an IPMX internal clock without PTP
                                      (MS6) */
#endif
  MTL_TIME_SOURCE_LEGACY = 7,      /* reported only (stat time.source): a wrapper's legacy
                                      default, TSC or ptp_get_time_fn clock (a legacy
                                      built-in PTP clock reports PTP_BUILTIN; mtl_legacy.h);
                                      -MTL_EINVAL as input */
};

struct mtl_instance_params {
  uint32_t struct_size;
  uint32_t port_count;                           /* entries in ports */
  const struct mtl_port_spec* MTL_NULLABLE ports; /* copied at open */
  const char* MTL_NULLABLE lcores;               /* CPU ids "2-5,8"; NULL = auto (deployment.md) */
  uint32_t time_source;                          /* enum mtl_time_source */
  uint32_t log_level;                            /* enum mtl_severity: the process's stderr
                                                    threshold while no log sink exists; 0 =
                                                    leave it (INFO, or a bridged legacy
                                                    instance's level) */
  uint64_t flags;                                /* MTL_INSTANCE_* */
  const struct mtl_option* MTL_NULLABLE options; /* instance keys, copied at open */
  uint32_t option_count;
  uint32_t reserved0;
  uint64_t reserved[8];
};

/* Opens the ports of p (legacy mtl_init). p NULL, or no port in p: the environment variable
   MTL_PORTS lists them in the grammar of mtl_port_parse(), parsed by that function:
   "0000:af:01.0=192.168.1.10/24@192.168.1.1,0000:af:01.1=192.168.2.10" or "null:1", a
   deployment convenience so the same test or pod image runs on any host; with no port at
   all, -MTL_EINVAL naming "ports". In a set-uid process MTL_PORTS and env: ports are
   -MTL_EINVAL.
   MTL_INSTANCE_SHARED (MS2a): the first open creates the process-wide instance and later
   ones join it, as they join a wrapper of a legacy instance (mtl_legacy.h); each open
   returns its own reference handle (MTL_SAME is false between two), and every call accepts
   any live reference. A joining open's ports (from p, or MTL_PORTS when p names none; none
   at all joins every port) are a subset of the live instance's, matched by normalised
   name, and an address, prefix, gateway or numa it sets equals the live port's; a port
   keeps the instance's index (mtl_port_find, mtl_observe.h). A port the instance lacks, or
   lcores, time source, other flags or options it sets that differ from the live
   instance's, fail with -MTL_EEXIST (INSTANCE_MISMATCH, the detail naming the first
   difference). log_level is process-wide: a non-zero value that differs from the
   threshold another open instance (shared, exclusive or bridged) set is -MTL_EEXIST
   (INSTANCE_MISMATCH, field "log_level"); 0 never conflicts. Every check that needs no
   device runs before any device is touched, so a misconfigured pod fails at once with one
   reason and a one-line detail (mtl_last_error) fit for a termination message. Open does
   not wait for links, neighbours or time lock: mtl_instance_get_health() (mtl_observe.h)
   reports them. After a close in the same process, open works again on the same ports and
   a subset of the first open's CPUs (EAL keeps its arguments); a port quarantined by an
   earlier close is -MTL_EBUSY (QUEUE_QUARANTINED) until exit. CP. (MS1) */
MTL_API_CP(1) int mtl_instance_open(const struct mtl_instance_params* MTL_NULLABLE p,
                                    mtl_instance_h* out);
/* Drops this reference. The last reference shuts the instance down within timeout_ns,
   network first (deployment.md): calls entering a data call get -MTL_ESHUTDOWN and those
   inside one are waited for; TX finishes the unit on the wire and flushes the queued rest
   (close sessions first to drain them); RX leaves its groups; the schedulers and devices
   stop; this instance's queued log lines reach the sinks, and the log thread is joined
   when no sink and no other instance remain; MtlManager grants are returned; memory is
   released.
   Queues, sessions and regions still open are closed by it, the queues first (R4). 0:
   retired. 1: quiesced: no
   device can reach any memory, but leases or regions are still referenced, or a library
   thread is still in application code past the deadline (counted; its memory is kept);
   call it again to poll. A slot's memory is freed by the next control-plane call of the
   process after its last lease is returned, or at exit. -MTL_EIO (reason
   QUEUE_QUARANTINED): a port could not be stopped; nothing it can reach is freed; exit the
   process, and the kernel stops the device. timeout 0: abort semantics (no drain);
   MTL_FOREVER: each step is still bounded by its own budget. From a library thread (log
   sink, codec): -MTL_EDEADLK, and the call has no effect. mtl_instance_shutdown()
   (mtl_observe.h) adds flags and a report. 0 for a null handle and for a reference of a
   shared instance that is not the last. CP. */
static inline int mtl_instance_close(mtl_instance_h mt, int64_t timeout_ns) {
  return mtl_close(MTL_OBJ_OF_INSTANCE(mt), timeout_ns);
}
/* on = 1: every wait of the instance and of every session returns -MTL_ECANCELED until
   on = 0, and blocked calls return at once; so do the waits on its queues; stop and close
   still work. For signal handlers: AS with on = 1, CP
   with on = 0. */
static inline int mtl_instance_interrupt(mtl_instance_h mt, int on) {
  return mtl_interrupt(MTL_OBJ_OF_INSTANCE(mt), on ? MTL_INTR_ON : MTL_INTR_OFF, 0);
}
/* Emergency stop (legacy mtl_abort; a second SIGTERM): interrupt every wait and stop
   every TX session at its next packet: the cut unit is FLUSHED (ABORTED) with
   MTL_TXR_PKT_SHORT, queued units FLUSHED (ABORTED). During close it skips to the hard
   stop; after close it does nothing. Close still has to follow. AS. */
static inline int mtl_instance_abort(mtl_instance_h mt) {
  return mtl_interrupt(MTL_OBJ_OF_INSTANCE(mt), MTL_INTR_ABORT, 0);
}

/* ---- Flows ------------------------------------------------------------------------ */

#define MTL_FLOWF_USER_MAC 0x1u     /* use dst_mac, no ARP */
#if defined(MTL_LATER)
#define MTL_FLOWF_DSCP_LITERAL 0x2u /* dscp as given, 0 = CS0 even where the IPMX profile
                                       differs (Phase 7) */
#endif

/* One leg's network flow. The legs of a session are fixed at create (in STOPPED, FLOWS
   or LEGS may change the set): a leg exists when its udp_port is not 0 (Phase 7: also
   when its bit in legs_disabled is set, a reserved leg that stays all zero until an update
   gives it an address together with LEGS; until then that bit is -MTL_ENOTSUP). Two legs
   with the same source and destination (the same port, ip and udp_port, and TX
   udp_src_port or RX source_filter) are -MTL_EINVAL: SDP cannot describe them (ST
   2110-10 §8.5). RX: ip is a group (multicast),
   or the port's own address or all zero (unicast, then source_filter checks the sender).
   The RTP identity (SSRC, payload type) is the session's, the same on every leg (ST
   2022-7). Fixed size. */
struct mtl_flow {
  uint32_t port;         /* 0 = the leg's own instance port, else MTL_INDEX(port):
                            mtl_flow_on_port() */
  uint8_t ip_family;     /* 0 = IPv4 (bytes 0..3), 6 = IPv6 */
  uint8_t dscp;          /* TX: into the IP TOS; 0 = CS0 (the IPMX profile's defaults,
                            Phase 7, differ) */
  uint8_t ttl;           /* 0 = 64 */
  uint8_t reserved0;
  uint8_t ip[16];        /* TX destination; RX group or unicast */
  uint8_t source_filter[16]; /* RX source-specific multicast; all zero = none */
  uint16_t udp_port;         /* required unless the leg is reserved */
  uint16_t udp_src_port;     /* TX: 0 = udp_port; RX: 0 = any */
  uint8_t dst_mac[6];        /* with MTL_FLOWF_USER_MAC */
  uint16_t vlan;             /* reserved, 0 */
  uint32_t reserved1;
  uint64_t flow_flags;       /* MTL_FLOWF_* */
  uint64_t reserved[4];
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
/* Puts the leg on instance port `port` (0-based): f->port = MTL_INDEX(port). The field holds
   MTL_INDEX(port), and 0 is the leg's own port (leg i on port i), so a literal
   flows[1].port = 1 puts leg 1 on port 0, beside leg 0 (MTL_INFO_LEGS_SHARE_PORT). */
static inline void mtl_flow_on_port(struct mtl_flow* f, uint32_t port) {
  f->port = MTL_INDEX(port);
}
/* A flow from text: "239.1.1.1:20000", or "239.1.1.1:20000@10.0.0.5" with the source of a
   source-specific group. Sets ip, udp_port and source_filter only. 0, or -MTL_EINVAL for
   anything else (IPv4 only). */
static inline int mtl_flow_parse(struct mtl_flow* f, const char* text) {
  uint8_t b[8] = {0, 0, 0, 0, 0, 0, 0, 0}; /* the group, then the source */
  uint32_t port = 0, field = 0;            /* fields: 4 group bytes, the port, 4 source bytes */
  for (const char* c = text;; c++, field++) {
    uint32_t v = 0, digits = 0;
    for (; *c >= '0' && *c <= '9' && digits < 5; c++, digits++) v = v * 10 + (uint32_t)(*c - '0');
    if (digits == 0 || (field == 4 ? (v == 0 || v > 65535) : v > 255)) return -MTL_EINVAL;
    if (field == 4)
      port = v;
    else
      b[field < 4 ? field : field - 1] = (uint8_t)v;
    if ((field == 4 || field == 8) && *c == 0) break;
    if (field == 8 || *c != (field == 3 ? ':' : field == 4 ? '@' : '.')) return -MTL_EINVAL;
  }
  f->ip_family = 0;
  for (int i = 0; i < 16; i++) {
    f->ip[i] = i < 4 ? b[i] : 0;
    f->source_filter[i] = i < 4 && field == 8 ? b[4 + i] : 0;
  }
  f->udp_port = (uint16_t)port;
  return 0;
}

/* ---- Essence configurations -------------------------------------------------------- */

/* A video raster: the session's own, or the video an ANC/fastmeta session belongs to.
   Width and height 1-32767 (ST 2110-20 §7.2). fps is the frame rate, any rate within the
   struct mtl_rational limits, not a table; fps {0, 0} is FIELD_REQUIRED for video and
   cvideo, the start's video for ANC and fastmeta, no unit rate for generic RTP. With
   MTL_INTERLACED or MTL_PSF a reduced fps above 30 frames per second is -MTL_EINVAL
   (FIELD_RATE): legacy fps and names like 1080i59.94 count fields. The engines run 1 to
   120 frames per second; outside that range, and before the rate's milestone (MS1: the
   rates of the legacy enum st_fps, interlaced: half of them; any rate MS2a), -MTL_ENOTSUP
   (contract.md §3.3). With video.detect ON, fps is the largest rate accepted ({0, 0}: 120,
   the engines' maximum) and sizes the scheduler quota. */
struct mtl_raster {
  uint32_t width;
  uint32_t height;
  uint32_t scan; /* enum mtl_scan */
  uint32_t reserved;
  struct mtl_rational fps; /* the frame rate, exact (interlaced: frames, not fields); a
                              named one: mtl_fps_rational(MTL_FPS_59_94) */
};

/* ST 2110-20 transport formats (RFC 4175 sampling and depth): enum st20_fmt + 1, so
   0 stays "not set" and migration is mechanical. 4:2:0 is progressive only: with
   MTL_INTERLACED or MTL_PSF it is -MTL_EINVAL (ST 2110-20 §6.2.5). */
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
/* ST 2110-21 sender type as requested; create grants a type and reports it in the stat
   info.sender_type, the SDP's TP (contract.md §3.3). Under the IPMX profile N keeps its CMAX
   and adds the IPMX VRX (TP=2110TPN). */
enum mtl_sender_type { /* enum st21_pacing: the same values */
  MTL_SENDER_N = 0,  /* ST21_PACING_NARROW: the gapped schedule, defined only for the
                        formats of ST 2110-21 §6.3.1 (contract.md §3.3); on any other format
                        create grants NL, and W until the linear schedule lands (MS5).
                        cvideo: N on those formats, NL on the others (ST 2110-22 §5.3).
                        Grants from MS2a; MS1 sends every type as the legacy API does */
  MTL_SENDER_W = 1,  /* ST21_PACING_WIDE: the linear read schedule; until MS5 sent on the
                        gapped schedule with VRX0 capped so that the W model holds */
  MTL_SENDER_NL = 2, /* ST21_PACING_LINEAR: the linear read schedule; until MS5 granted N on
                        the formats of §6.3.1, -MTL_ENOTSUP on the others */
};
/* RX detection of what the stream carries (video: width, height, scan, frame rate and
   packing, never the transport format, which is configured; ANC: interlace from the F
   bits), reported per unit (mtl_rx_detail) and by MTL_EVENT_RX_FORMAT. */
enum mtl_detect {
  MTL_DETECT_AUTO = 0, /* the essence's default: video off, ANC on */
  MTL_DETECT_ON = 1,
  MTL_DETECT_OFF = 2,
};

struct mtl_video_config {
  struct mtl_raster raster; /* required; with detect ON the maximum: width and height
                               bound the stream and size the pool, fps is the largest frame
                               rate and sizes the scheduler quota ({0, 0}: 120 frames per
                               second), scan must be MTL_PROGRESSIVE and every scan is
                               accepted (contract §3.3) */
  uint32_t format;          /* enum mtl_video_format; 0 = from app_format */
  uint32_t app_format;      /* mtl_format.h; 0 = format itself, no conversion */
  uint32_t packing;         /* enum mtl_packing */
  uint32_t sender_type;     /* enum mtl_sender_type */
  uint32_t detect;          /* RX: enum mtl_detect (AUTO: off); ON publishes every format
                               within raster (MS3) */
  /* SDP and conversion metadata, never on the wire (mtl_format.h); uint8_t to fit the
     reserved space. MTL_UPDATE_MEDIA may change them while running when no conversion
     uses them. */
  uint8_t colorimetry; /* enum mtl_colorimetry; 0 = UNSPECIFIED (rendered: SDP requires it) */
  uint8_t tcs;         /* enum mtl_tcs; 0 = SDR */
  uint8_t range;       /* enum mtl_range; 0 = narrow */
  uint8_t reserved0;
  uint32_t linesize[MTL_MAX_PLANES]; /* library pools: bytes per row per plane, legacy
                                        linesize; 0 = packed: row bytes, plane p + 1 at
                                        plane p + stride x rows (FFmpeg
                                        av_image_fill_arrays with align 1); MTL_APP_V210:
                                        128 bytes per 48 pixels (contract §9.1) */
  uint32_t troffset_us; /* TX: the TROFFSET sent and signalled as TROFF; RX: the sender's
                           TROFF (the timing parser's VRX). 0 = TRODEFAULT, or where ST 2110-21
                           defines none (an interlaced or PsF linear grant outside Table 1,
                           §6.4) the library's offset, signalled from info.troffset_ns; else
                           whole us, TROFFSET < TFRAME (§6.2), else -MTL_EINVAL: SDP TROFF is
                           a positive integer of us (§8.2), so 0 us cannot be signalled */
  uint32_t reserved1;
  uint64_t reserved[6];
};

enum mtl_codec {
  MTL_CODEC_JPEGXS = 1,
  MTL_CODEC_H264 = 2,
  MTL_CODEC_H265 = 3,
};
enum mtl_cvideo_rate {
  MTL_CVIDEO_CBR = 0,     /* constant bytes per unit, as ST 2110-22 requires */
  MTL_CVIDEO_VBR_MAX = 1, /* packets follow the codestream, at most codestream_bytes per unit,
                             as the legacy ST 2110-22 sender sends */
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
  uint64_t reserved[9];
};

enum mtl_audio_format {
  MTL_PCM8 = 1,  /* L8: outside the AES67 formats ST 2110-30 takes (L16, L24), so
                    MTL_INFO_NON_COMPLIANT (inferred: AES67 not read) */
  MTL_PCM16 = 2,
  MTL_PCM24 = 3,
  MTL_AM824 = 4, /* ST 2110-31: channels counts AES3 subframes, even (two per AES3
                    signal), else -MTL_EINVAL (ST 2110-31 §6.1) */
};
enum mtl_ptime { /* enum st30_ptime + 1 */
  MTL_PTIME_1MS = 1,
  MTL_PTIME_125US = 2,  /* an ST 2110-31 SDP writes ptime 0.12 (Table 1) */
  MTL_PTIME_250US = 3,
  MTL_PTIME_333US = 4,
  MTL_PTIME_4MS = 5,
  MTL_PTIME_80US = 6,   /* ST31_PTIME_80US: the ST 2110-31 0.08 packet, 4 samples at 48 kHz
                           (8 at 96 kHz), one packet every 83 1/3 us (Table 1); the
                           legacy engine sends one every 80 us (SF-35) */
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
  /* audio media indices count samples: unit.media_index is the unit's first sample.
     Plane 0 holds the samples as on the wire: interleaved by channel, each sample in
     network byte order (big-endian; AM824: the 4-byte subframes); MTL never swaps them */
  uint32_t reserved0;
  uint64_t reserved[13];
};

/* ST 2110-40 transmission models (§6.4, §6.5), signalled as TM (§7) */
enum mtl_anc_timing_model {
  MTL_ANC_CTM = 1,  /* compatible; signalled TM=CTM */
  MTL_ANC_LLTM = 2, /* low latency; signalled TM=LLTM */
};
/* An ANC unit is the ANC packets of one frame or field (one per frame for PsF) in the planes
   of one slot (contract.md §5.7): plane 0 the packet table, a struct mtl_anc_packet per row
   (max_packets rows); plane 1 the user data words, one per row (max_udw_words rows of 1 byte
   with MTL_ANC_WORDS_8BIT, of 2 bytes otherwise); with MTL_ANC_WORDS_RAW also plane 2, per
   table row the four uint16_t words DID, SDID or DBN, Data_Count and Checksum_Word as on the
   wire. unit.used counts table entries. Submit claims the lease, then encodes the unit into the
   session's private wire area (outside every pool region) and dequeue decodes it from there, so
   the tasklets only copy whole RTP payloads. ANC planes use natural strides. */
enum mtl_anc_word_mode {
  MTL_ANC_WORDS_8BIT = 1,  /* the default (0): a user data word is a byte, its value b7-b0;
                              TX adds b8 = even parity and b9 = NOT b8, and RX skips a packet
                              whose words break that rule: the convention of the 8-bit payload
                              documents (captions, AFD, SCTE-104, timecode), not ST 291-1
                              §6.6's 8-bit applications (b9-b2) */
  MTL_ANC_WORDS_10BIT = 2, /* a uint16_t user data word with all ten bits, as it is (ST 291-1
                              §6.6): TX refuses 000h-003h and 3FCh-3FFh (§9.1), RX skips them */
  MTL_ANC_WORDS_RAW = 3,   /* relays: the user data words as in 10BIT and, in plane 2, DID,
                              SDID or DBN, Data_Count and Checksum_Word, all as on the wire;
                              MTL computes and refuses nothing in them, and RX flags what it
                              finds wrong (MTL_ANCF_PARITY_ERR, MTL_ANCF_CHECKSUM_ERR). Bit for
                              bit for well-framed packets; pad bits, trailing bytes and packets
                              past Length are not carried */
};
struct mtl_anc_config {
  struct mtl_raster video; /* the video it belongs to: fps and scan; all zero = the raster
                              and launch delay of the first video session of its start
                              (-MTL_EINVAL, FIELD_REQUIRED, if there is none) */
  uint32_t max_udw_words;  /* plane 1 rows per unit, at most 255 x max_packets; 0 =
                              max(255, 32 x max_packets), 1024 at the default; a unit's
                              entries use at most this many words in all (a shared word
                              counts once per entry) */
  uint32_t detect;         /* RX: enum mtl_detect (AUTO: on) */
  uint32_t timing_model;   /* TX: enum mtl_anc_timing_model; 0 = CTM paced, no TM in the
                              SDP (receivers presume CTM, ST 2110-40 §7); RX: 0 */
  uint16_t max_packets;    /* plane 0 rows: ANC packets per unit, 1-65535 (create also
                              checks the link can carry it); 0 = 32 (legacy: 20) */
  uint8_t word_mode;       /* enum mtl_anc_word_mode; 0 = MTL_ANC_WORDS_8BIT */
  uint8_t reserved1;
  uint64_t reserved[10];
};

#define MTL_FASTMETA_FREE_RUNNING 0x1u /* own rate in video.fps, not a video's */
#define MTL_FASTMETA_RX_MATCH_DIT 0x2u /* RX: filter on data_item_type */
#define MTL_FASTMETA_RX_MATCH_K 0x4u   /* RX: filter on k_bit */
struct mtl_fastmeta_config {
  struct mtl_raster video; /* as ANC; FREE_RUNNING: fps = own rate, always required */
  uint32_t data_item_type; /* TX: on the wire, one type per session (no Annex A
                              segmentation); RX: filter */
  uint32_t k_bit;
  uint32_t fastmeta_flags;        /* MTL_FASTMETA_* */
  uint32_t buffer_capacity_bytes; /* per unit, 0 = 64 KiB; a unit goes out as data items of
                                     at most 359 words (1436 B), one per RTP packet, to fit
                                     the 1460 B UDP limit (ST 2110-41 §5.4) */
  uint64_t reserved[10];
};

/* Generic RTP essence (packet units only). UNIT pacing over a unit of packets_per_unit
   packets. The encoding "SMPTE2022-6" (ST 2022-6 timed by ST 2022-8) adds its rules:
   clock_rate 27000000; profile LINEAR (GAPPED is -MTL_EINVAL, ST 2022-8 §6); the unit is
   one SDI frame, also for interlaced and PsF formats (ST 2022-8 §5.3 note 3), so unit.fps
   is the SDI frame rate and unit.scan MTL_PROGRESSIVE; packets_per_unit the ST 2022-6
   §6.5 count (4497 at 1080i29.97 and 1080p59.94 3G level A); every packet carries its
   own timestamp (ST 2022-6 §6.3), which the application writes: MTL_PKT_SET_TIMESTAMP is
   -MTL_EINVAL (PKT_CONFIG) and MTL_PKT_TX_VALIDATE checks that the timestamps of a unit
   increase. */
enum mtl_rtp_profile {
  MTL_RTP_LINEAR = 0, /* the ST 2110-21 type NL read schedule over the unit: TRS = unit
                         period / packets_per_unit, TROFFSET = rtp.troffset_us or, 0, the
                         ST 2022-8 §6 TRODEFAULT (the NL VRX_FULL x TRS); reported as
                         info.troffset_ns, info.vrx_full, info.cmax for TP=2110TPNL */
  MTL_RTP_GAPPED = 1, /* the ST 2110-21 gapped (type N) schedule, RACTIVE 1080/1125 */
};
struct mtl_rtp_config {
  uint32_t clock_rate; /* RTP clock in Hz; required (ST 2022-6: 27000000) */
  uint32_t profile;    /* enum mtl_rtp_profile */
  struct mtl_raster unit; /* unit rate and scan in fps/scan; size fields unused. All zero:
                             no unit rate (ST 2110-43 timed text): TAI media mode only,
                             media times never snapped, no UNIT pacing (-MTL_EINVAL), and
                             a launch may precede the media time (a document goes out
                             ahead of its activation, ST 2110-43 §5.1) */
  uint64_t bitrate_bps;   /* rate of chunks without a unit grid; 0 = derived */
  char encoding[32];      /* SDP rtpmap name, e.g. "SMPTE2022-6", "ttml+xml" */
  uint32_t troffset_us;   /* with a unit rate, as video.troffset_us (TP and TROFF as ST
                             2110-21, ST 2022-8 §6, §7.1, §8.1); without one, 0 */
  uint32_t reserved0;
  uint64_t reserved[5];
};

/* Packet units (unit = MTL_UNIT_PACKETS): sizes and pacing. Tables in mtl_packet.h. */
enum mtl_pkt_pacing {
  MTL_PKT_PACE_UNIT = 0,   /* the essence's wire model over each unit */
  MTL_PKT_PACE_LAUNCH = 1, /* a chunk at unit.launch_tai_ns, then at the session rate; not
                              flagged non-compliant (generic RTP without a unit rate has no
                              wire model to break) */
  MTL_PKT_PACE_ASAP = 2,   /* as fast as the queue accepts; non-compliant */
};
enum mtl_pkt_unit_time {
  MTL_PKT_TIME_SUBMIT = 0,   /* from the first chunk's submission, by media mode */
  MTL_PKT_TIME_FROM_RTP = 1, /* from the RTP timestamp of the unit's first packet; SMPTE2022-6:
                                the unit-grid instant nearest it, which is the frame's
                                Synchronizing Timestamp of an epoch-aligned source (that
                                packet's RTP lies 0.6-8.6 us earlier, ST 2022-8 §5.3) */
};
struct mtl_packet_config {
  uint32_t packets_per_chunk; /* slots per lease; 0 = by essence */
  uint32_t slot_bytes;        /* max RTP packet; 0 = max_udp_payload (mtl_session_config) */
  uint32_t packets_per_unit;  /* UNIT pacing; 0 = derived (video) or unlimited (anc) */
  uint32_t pacing;            /* enum mtl_pkt_pacing */
  uint32_t unit_time;         /* enum mtl_pkt_unit_time */
  uint32_t rx_ring_packets;   /* RX: NIC buffers the session may hold; 0 = 512 */
  uint64_t set_fields;        /* MTL_PKT_SET_* (mtl_packet.h); 0 = verbatim */
  uint64_t packet_flags;      /* MTL_PKT_* (mtl_packet.h) */
  uint64_t reserved[3];
};

/* ---- Session configuration ---------------------------------------------------------- */

/* ST 2110-10 §8.7 TSMODE. SAMP only when the RTP is the sampling or intended instant
   (capture, playback, a processor whose input was SAMP), PRES for a time-preserving
   processor of a non-SAMP input, NEW for one that re-stamps or snaps (§7.9, Annex C). */
enum mtl_tsmode { MTL_TSMODE_SAMP = 1, MTL_TSMODE_NEW = 2, MTL_TSMODE_PRES = 3 };

/* mtl_session_config.flags */
#define MTL_SESSION_RESULTS 0x1u        /* results for library memory (app memory: always) */
#define MTL_SESSION_POOL_ATTACHED 0x2u  /* slots come from mtl_session_attach (mtl_mem.h) */
#define MTL_SESSION_REQUIRE_DIRECT 0x4u /* fail at create if any unit would be copied */
/* RX slot = media index mod pool_count (MXL). A unit whose slot is leased or held is
   dropped and counted in the next unit's missed_before, never written over; with
   RX_LATEST an unread (not leased) unit in that slot is replaced. */
#define MTL_SESSION_RX_BY_INDEX 0x8u
#define MTL_SESSION_RX_LATEST 0x10u     /* RX: a full pool reclaims the oldest unread unit */
#define MTL_SESSION_RX_NO_FILL 0x20u    /* RX, library pools: do not zero what lost packets
                                           left out. Attached pools are never zero-filled:
                                           the unit is MTL_RX_INCOMPLETE and
                                           mtl_rx_get_detail counts the loss */
#define MTL_SESSION_MT_SUBMIT 0x40u     /* several threads submit (acquire is MP-safe) */
/* TX: the session admits MTL_SUBMIT_EXACT units (exact launch is a property of the
   transport session); the other units of such a session start at their launch index's
   first-packet time. Without it, MTL_SUBMIT_EXACT is -MTL_EINVAL. */
#define MTL_SESSION_EXACT_LAUNCH 0x80u
/* TX, a converting library pool (video.app_format other than the transport format): every
   submit names caller planes (MTL_SUBMIT_SRC_PLANES), so the pool allocates no app-format
   planes; acquire returns the slot's layout with null plane addresses, and a submit
   without MTL_SUBMIT_SRC_PLANES is -MTL_EINVAL. Any other session: -MTL_EINVAL at create.
   (MS2) */
#define MTL_SESSION_TX_SRC_PLANES 0x100u

/* One stream of one essence in one direction. The member for `essence` is read; the
   other essence members must stay zero. MTL_INIT it, set direction, essence, flows[0] and
   the essence's required fields; everything else defaults. Every session runs on the
   SMPTE epoch (mtl_session_start, mtl_sync.h). */
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
                                          down. Phase 7 (-MTL_ENOTSUP until then): a
                                          reserved leg, and every existing leg set =
                                          muted: RUNNING; TX units retire at their index,
                                          counted in tx.units_muted; RX groups left */
  uint32_t media_mode;                 /* enum mtl_media_mode */
  uint32_t ssrc;                       /* TX: 0 = one random SSRC for the session, on every
                                          leg; RX: 0 = no check */
  int64_t min_tx_delay_ns;             /* TX: the earliest send after the media time. 0 =
                                          playback (content exists before its media time);
                                          a capture producer (camera, encoder, RX -> TX)
                                          sets one frame period + the pick-up lead
                                          (timing.md), a launch delay of 1 */
  int64_t media_time_offset_ns;        /* TX, TAI mode: the producer's latency, which moves
                                          the index; RTP stays the index's, so no ST 2110-10
                                          §7.6.3 bound applies. Other modes: 0, else
                                          -MTL_EINVAL (the legacy RTP trim with the launch
                                          fixed is the option tx.rtp_trim_ns) */
  const struct mtl_option* MTL_NULLABLE options; /* caller's array, deep-copied */
  uint32_t option_count;
  uint8_t payload_type;                /* TX: 0 = the essence's default, else a dynamic
                                          type 96-127 (ST 2110-10 §6.2, ST 2110-41 §5.2;
                                          MTL_RTP: 1-127), else -MTL_EINVAL; RX: 0 = no
                                          check */
  uint8_t tsmode;                      /* TX: enum mtl_tsmode for the SDP TSMODE, 0 = none
                                          (receivers then presume NEW, ST 2110-10 §8.7);
                                          RX: 0 */
  uint16_t max_udp_payload;            /* bytes of RTP (header and payload) per packet.
                                          TX: 0 = instance.max_udp_payload, else 1452, the
                                          1460 B Standard UDP Size Limit less the UDP header
                                          (ST 2110-10 §6.3); above 1452 video and cvideo only,
                                          up to min(port MTU - 28, 8952) (the Extended limit,
                                          §6.4), and only then signalled as MAXUDP = this + 8
                                          (§8.6); MTL_RTP up to MTU - 28; others -MTL_EINVAL.
                                          RX: the sender's MAXUDP - 8; 0 = its SDP has no
                                          MAXUDP: 1452 (§8.6). It sizes the receive buffers.
                                          VRX_FULL takes MAXUDP = 1500 up to 1452, else this
                                          + 8 (ST 2110-21 §7.1.2-§7.1.4) */
  struct mtl_video_config video;
  struct mtl_cvideo_config cvideo;
  struct mtl_audio_config audio;
  struct mtl_anc_config anc;
  struct mtl_fastmeta_config fastmeta;
  struct mtl_rtp_config rtp;
  struct mtl_packet_config packet; /* unit = MTL_UNIT_PACKETS */
  uint64_t reserved[9];
};

/* ---- Session lifecycle --------------------------------------------------------------- */

/* Creates a CREATED session: validated, resources reserved, nothing sent. -MTL_EBUSY
   (MANAGER_LOST) while a configuration that needs MtlManager has lost it. CP. (MS1) */
MTL_API_CP(1) int mtl_session_create(mtl_instance_h mt, const struct mtl_session_config* sc,
                                     mtl_session_h* out);

/* The granted configuration (output; the size argument versions it). */
struct mtl_leg_info {
  uint32_t port;         /* the instance port, 0-based */
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
/* a mode outside ST 2110: MTL_CVIDEO_VBR_MAX, a transport format outside RFC 4175
   (mtl_format.h), MTL_PCM8, MTL_PKT_PACE_ASAP, MTL_SUBMIT_EXACT; video sender types the
   wire does not meet (contract.md §3.3): in MS1 NL, N on a format outside ST 2110-21
   §6.3.1, and interlaced or PsF N; from MS2a only SD interlaced N, until E5b */
#define MTL_INFO_NON_COMPLIANT 0x1u
#if defined(MTL_LATER)
#define MTL_INFO_MEDIACLK_SENDER 0x2u /* RTP follows the source (MTL_MEDIA_SENDER, Phase 7) */
#endif
#define MTL_INFO_RESULTS 0x4u       /* results are produced */
#define MTL_INFO_DIRECT 0x8u        /* no unit is copied */
/* the launch delay puts the stream outside the JT-NM Tested default windows (timing.md) */
#define MTL_INFO_JTNM_DEFAULT_WINDOW_EXCEEDED 0x10u
/* a tx.rtp_trim_ns that is not a multiple of the index period: RTP is off the N x period
   grid */
#define MTL_INFO_RTP_OFF_GRID 0x20u
/* latency_max_ns < latency_min_ns: pool_count >= ceil(latency_min_ns / U) + 1, U the unit
   period, makes it feasible; a consumer adds the units it holds (contract §3.5) */
#define MTL_INFO_LATENCY_INFEASIBLE 0x40u
/* both legs on one instance port: ST 2022-7 then covers loss on the network paths, not a
   NIC or link failure. Create accepts it and logs one WARNING line (contract.md §14.1) */
#define MTL_INFO_LEGS_SHARE_PORT 0x80u
struct mtl_session_info {
  uint32_t direction;
  uint32_t essence;
  uint32_t unit;
  uint32_t flags; /* MTL_INFO_* */
  char name[MTL_NAME_MAX];
  int64_t created_tai_ns;
  uint32_t pool_count;
  uint32_t max_count; /* pool_count limit */
  uint32_t pacing_class; /* enum mtl_pacing, granted */
  uint32_t leg_count;
  uint32_t pkts_per_unit;
  uint32_t unit_samples;          /* audio */
  uint32_t buffer_capacity_bytes; /* audio, ANC, fastmeta, cvideo per unit */
  uint32_t meta_capacity;         /* the per-slot meta area */
  uint32_t codestream_bytes;      /* cvideo, granted in whole packets */
  uint32_t sched_index;
  uint64_t unit_bytes;      /* one unit in the app layout */
  uint64_t pool_slot_pitch; /* library pools: bytes between slots (slot j at j x pitch of
                               the pool region, §9.6), a multiple of 64; video and cvideo
                               RX: >= unit_bytes (of the maximum) + MTL_RX_TAIL_BYTES */
  /* For framework latency queries; never negative; recomputed by every call from the
     current format and options (contract §3.5):
     RX min: the latest a unit is delivered, in normal operation, after its media time,
       caller work excluded: rx.link_offset_ns when set, else S + U + F + W (S the sender's
       offset: video and cvideo the RX TROFFSET, troffset_us or TRODEFAULT; ANC and
       fastmeta their timing model's target delay; audio one packet time; U the unit
       period, packet units packet.rx_max_wait_ns; F rx.flush_offset_ns; W
       info.expected_wake_latency_ns); rows units S + the time of rx.rows_step rows + W.
     RX max: a unit dequeued and released before its media time + max never makes the
       pool miss a unit: (pool_count - 1) x U; INT64_MAX with MTL_SESSION_RX_LATEST.
     TX min: max(0, min_submit_lead_ns), the work of submit itself excluded.
     TX max: tx.horizon_ns, the largest lead accepted.
     Detect sessions: from the detected format, U the current unit's mtl_rx_detail.raster;
     before the first unit from the maximum, fps {0, 0} read as 120/1. */
  int64_t latency_min_ns;
  int64_t latency_max_ns;
  int64_t min_submit_lead_ns; /* submit at least this long before the media time; negative
                                 when the deadline is after it (timing.md §6.1) */
  struct mtl_leg_info leg[MTL_MAX_LEGS];
  uint64_t wire_bps;          /* the bandwidth of one leg on the wire, headers included */
  int64_t convert_ns; /* the caller's work per unit inside dequeue (RX) or submit (TX):
                         conversion, and on TX the copy of MTL_SUBMIT_SRC_PLANES; 0 for an
                         RX session without conversion. From create a calibration of one
                         unit's work, then the largest measured in the last second; query
                         reports 0. Add it to latency_min_ns for a framework */
  struct mtl_raster raster; /* granted, fps reduced: video and cvideo their own; ANC and
                               fastmeta theirs or, all zero in the config, the start's
                               video (zero until the start); generic RTP its unit; audio
                               zero */
  uint64_t reserved[2];
};

#define MTL_QUERY_CHECK_CAPACITY 0x1u /* also check free capacity: -MTL_ENOSPC + reason */
struct mtl_buffer_requirements; /* mtl_mem.h */
/* Dry run of create: validates and grants without allocating. info and req may be NULL;
   req gives the layout an attached pool needs. ANC/fastmeta rasters taken from a start are
   reported as 0 until then. CP. (MS1) */
MTL_API_CP(1) int mtl_session_query(mtl_instance_h mt, const struct mtl_session_config* sc,
                                    uint64_t flags, struct mtl_session_info* MTL_NULLABLE info,
                                    size_t info_size,
                                    struct mtl_buffer_requirements* MTL_NULLABLE req,
                                    size_t req_size);
/* What create granted. CP. (MS1) */
MTL_API_CP(1) int mtl_session_get_info(mtl_session_h s, struct mtl_session_info* info,
                                       size_t info_size);

/* When something happens. A NULL `when` means NOW. */
enum mtl_when_kind {
  MTL_NOW = 0,
  MTL_AT_TAI = 1,   /* value = TAI ns */
  MTL_AT_INDEX = 2, /* value = a media index of the session (of s[0] for a start): TX start,
                       TX flow change */
};
/* mtl_when.flags */
#define MTL_WHEN_ORIGIN 0x1u /* start, TX: media index 0 of every started session is this
                                start's T0 (-MTL_EINVAL on RX and with MTL_AT_INDEX) */
struct mtl_when {
  uint32_t kind;  /* enum mtl_when_kind */
  uint32_t flags; /* MTL_WHEN_* */
  int64_t value;
  int64_t preroll_ns; /* start only (ignored by update): extra lead before the first unit */
  uint64_t reserved[3];
};

/* Starts n sessions of one direction, all or none; with n > 1 no TX session may use
   MTL_MEDIA_AUTO (-MTL_EINVAL, START_SET_MIXED). Every session runs on the SMPTE epoch:
   index k is at k x its index period from 1970 TAI (a frame or field for video and ANC, a
   sample for audio; mtl_sync.h). T0 is resolved once, on the common grid of the sessions'
   index periods at or after `when` (NOW: the first feasible one). Without MTL_WHEN_ORIGIN the
   media indices stay epoch indices and T0 is the first unit's media time; with it, index 0
   of every session started here is at T0, so a file's frame and sample counts are media
   indices as they are (start arrays: MS6). ANC and fastmeta sessions with an all-zero
   video raster take the raster and the launch delay of the first video session of the
   array. RX: `when` is the earliest media time delivered. t0 (may be NULL) receives T0.
   CP. (MS1) */
MTL_API_CP(1) int mtl_session_start(const mtl_session_h* s, uint32_t n,
                                    const struct mtl_when* MTL_NULLABLE when,
                                    int64_t* MTL_NULLABLE t0_tai_ns);
/* mode: enum mtl_stop_mode; the session can be started again. Stop arrays may mix
   directions. -MTL_ETIMEDOUT if DRAIN missed the deadline: the rest became FLUSHED
   (reason STOP_TIMEOUT); units already handed to the device get their result when the
   device releases them, and the session stays FLUSHING until then. -MTL_EIO if a fault
   entered ERROR during the drain or flush: the stop still ends in STOPPED, and
   mtl_last_error() names the fault. CP. (MS1) */
MTL_API_CP(1) int mtl_session_stop(const mtl_session_h* s, uint32_t n, uint32_t mode,
                                   int64_t timeout_ns);

/* mtl_session_update() parts: one atomic change, applied at `when` on every leg. */
#define MTL_UPDATE_FLOWS 0x1u /* sc->flows (IS-05); a port change while running is made
                                 before break (Phase 7), or -MTL_EBUSY
                                 (PORT_CHANGE_NEEDS_STOP) on a backend that cannot, and on
                                 every backend until then */
#define MTL_UPDATE_LEGS 0x2u  /* sc->legs_disabled; disabling every existing leg (mute) is
                                 Phase 7, -MTL_ENOTSUP until then */
#define MTL_UPDATE_MEDIA 0x4u /* the essence member, tsmode and max_udp_payload; CREATED or
                                 STOPPED (colorimetry, tcs and range also while running) */
#define MTL_UPDATE_POOL 0x8u  /* pool_count; CREATED or STOPPED */
#if defined(MTL_LATER)
/* Modifiers (Phase 7) */
#define MTL_UPDATE_REAPPLY 0x10u /* re-apply identical enabled legs (IS-05 re-activation):
                                    RX re-sends its membership reports and re-arms its
                                    rules, TX resolves the neighbour and rebuilds headers */
#define MTL_UPDATE_DRY_RUN 0x20u /* validate and plan (planned_tai_ns filled); nothing is
                                    reserved, posted or replaced, the status is untouched,
                                    and a later update may still fail with -MTL_ENOSPC */
#endif
/* All or nothing: resources reserved before commit; neighbours are resolved from then on
   and a leg without one waits in WAITING_NEIGHBOUR, so the update still applies. Reads
   only the members its `parts` name, and sc->options: a key there must equal its current
   value (-MTL_EBUSY, OPTION_STATE), except that a key that may change while running (R)
   applies at the same boundary (Phase 7; until then a changed R key is -MTL_ENOTSUP).
   direction, essence and unit never change. `when` applies to FLOWS, LEGS, those
   options, and colorimetry, tcs and range under MEDIA while running (NULL otherwise); in
   ARMED, a `when` before T0 means T0; an AT_TAI or AT_INDEX already past means NOW. The
   switch happens at the index boundary by the clock, whether a unit is there or not (TX:
   the first index at or after `when`; RX: units with media time at or after it, or by
   local arrival time for unlocked clocks; audio: a packet boundary, so a salvo lands
   within one packet time). -MTL_EBUSY (WRONG_STATE) while an earlier update is pending.
   sc NULL with parts 0 cancels a pending update (IS-05 activation mode null): 0
   cancelled, 1 none pending, -MTL_EBUSY (UPDATE_COMMITTING) when it will still apply
   (Phase 7; -MTL_ENOTSUP until then).
   A pending update fails (TIME_STEP) if the time base steps. Returns once the change is
   posted; planned_tai_ns (may be NULL) receives the media time of the boundary, and
   status.update_* and MTL_EVENT_UPDATE report when it applied. In CREATED and STOPPED it
   applies during the call and planned = applied = now (MS3, every part); in ARMED and
   RUNNING it is -MTL_ENOTSUP (NOT_IMPLEMENTED, field "state") until MS5. update_seq counts
   posted updates. CP. (MS3) */
MTL_API_CP(3) int mtl_session_update(mtl_session_h s,
                                     const struct mtl_session_config* MTL_NULLABLE sc,
                                     uint64_t parts, const struct mtl_when* MTL_NULLABLE when,
                                     int64_t* MTL_NULLABLE planned_tai_ns);

#define MTL_DISCARD_REBASE 0x1u /* map first_index to the next feasible index (seek) */
/* The session stays where it is (seek). TX: queued units become FLUSHED (DISCARD). RX:
   the unit being received and the ready, undequeued units are dropped and counted in
   rx.units_flushed, so the first dequeue after the call is a new unit; a dequeued unit
   stays the application's lease. CP. (MS2) */
MTL_API_CP(2) int mtl_session_discard(mtl_session_h s, uint64_t flags, int64_t first_index);

/* Stops (DRAIN until the deadline, then FLUSH), destroys, and waits up to timeout_ns for
   the session to retire. 0: retired; no memory, lease, hold or device reference remains.
   1: still retiring (leases out, or units held by the device): retirement completes by
   itself on the last release, so a caller over library memory may walk away; calling
   close again polls (with a timeout, waits); mtl_session_get_status reads CLOSING, then
   RETIRED (after the instance closed too: RETIRED from the last release on, the memory
   freed by the next control-plane call or at exit), and the only other valid call on s
   is mtl_release of a lease taken before the close, from any thread. Unread results are discarded. Sessions
   attached over this session's pool must close first (1 until they do). A null handle
   returns 0, so cleanup paths need no checks. CP. */
static inline int mtl_session_close(mtl_session_h s, int64_t timeout_ns) {
  return mtl_close(MTL_OBJ_OF_SESSION(s), timeout_ns);
}

/* Create and start now (legacy *_create with ops, then running): the whole setup of a
   stream that needs nothing else. On failure *out is the null handle and the failing
   step's error stays in mtl_last_error() (the close of the cleanup succeeds). CP. */
static inline int mtl_session_open(mtl_instance_h mt, const struct mtl_session_config* sc,
                                   mtl_session_h* out) {
  int ret = mtl_session_create(mt, sc, out);
  if (ret >= 0) ret = mtl_session_start(out, 1, NULL, NULL);
  if (ret < 0) {
    mtl_session_close(*out, 0); /* a CREATED session retires at once; null: 0 */
    *out = MTL_NULL(mtl_session_h);
  }
  return ret;
}

/* Why the session is where it is (output; the size argument versions it). */
struct mtl_leg_status {
  uint32_t admin;      /* 1 = enabled */
  uint32_t oper;       /* 1 = link up and flow resolved or joined */
  uint32_t flow_state; /* enum mtl_flow_state */
  uint32_t reserved;
};
#define MTL_STATUS_RX_SIGNAL 0x1u          /* RX: packets are arriving */
#define MTL_STATUS_FORMAT_CHANGED 0x2u     /* RX: the stream does not fit the config (detect
                                              ON: above the maximum, or not identified);
                                              format_reason says why */
#define MTL_STATUS_PACING_DOWNGRADED 0x4u  /* TX: running below the granted pacing class */
#define MTL_STATUS_TIMING_WARNING 0x8u     /* TX: timing_reason, shortfall_ns are set */
#if defined(MTL_LATER)
#define MTL_STATUS_MUTED 0x10u             /* every leg admin-disabled; RUNNING, nothing sent
                                              (a transport state: NMOS master_enable stays
                                              the Node's) (Phase 7) */
#endif
#define MTL_STATUS_TIMEBASE_SUSPECT 0x20u  /* RX: arrival and media time of a DIRECT stream
                                              differ by more than 1 s
                                              (MTL_EVENT_RX_TIMEBASE_SUSPECT) */
#define MTL_STATUS_RX_DETECTING 0x40u      /* RX, detect ON: measuring the stream's format;
                                              no unit is published until it is known */
enum mtl_update_state {
  MTL_UPDATE_STATE_NONE = 0,
  MTL_UPDATE_STATE_PENDING = 1,
  MTL_UPDATE_STATE_APPLIED = 2,
  MTL_UPDATE_STATE_FAILED = 3,   /* update_reason says why; the old configuration stays */
#if defined(MTL_LATER)
  MTL_UPDATE_STATE_REPLACED = 4, /* a later update took its place before it applied
                                    (Phase 7) */
  MTL_UPDATE_STATE_CANCELLED = 5, /* (Phase 7) */
#endif
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
  uint32_t format_reason; /* with FORMAT_CHANGED: RASTER_MISMATCH (outside the raster, or
                             above the maximum) or DETECT_FAILED; else 0 */
  uint32_t provide_gen;   /* provide sessions: the generation of destinations provided now,
                             1 at create, + 1 per hand-back, published before the hand-back
                             returns; 31-bit, 2^31 - 1 wraps to 1; g is older than G when
                             (G - g) mod (2^31 - 1) lies in 1 .. 2^30 - 1 (mtl_rx_provide) */
};
/* enum mtl_state as the result (>= 0), or < 0; st (may be NULL) receives the status. A
   published snapshot, never torn, lock-free from any thread. The one call valid on a
   closing handle: state CLOSING, then RETIRED (R4). DP. (MS1) */
MTL_API_DP(1) int mtl_session_get_status(mtl_session_h s,
                                         struct mtl_session_status* MTL_NULLABLE st,
                                         size_t st_size);
static inline int mtl_session_get_state(mtl_session_h s) {
  return mtl_session_get_status(s, NULL, 0);
}

/* ---- Units: what acquire and dequeue lend ------------------------------------------- */

/* Video: one row per line. Audio: one row per sample frame (row_bytes = channels x bytes
   per sample, rows = the samples a unit holds). cvideo and fastmeta: one row of the unit's
   capacity. ANC: plane 0 one row per packet table entry, plane 1 one row per user data
   word (mtl_anc_config). Packet units: one row per packet slot. */
struct mtl_plane {
  MTL_ADDR(void) addr; /* NULL in device memory without a CPU mapping */
  uint32_t stride;     /* bytes between rows (packet units: between slots) */
  uint32_t row_bytes;  /* valid bytes per row (packet units: slot capacity) */
  uint32_t rows;       /* rows (packet units: slots) */
  uint32_t reserved;
};

/* unit.flags, TX inputs (bits 0-31). NOT_BEFORE and EXACT exclude each other; UNIT_END
   only on packet units (-MTL_EINVAL otherwise). */
#define MTL_SUBMIT_DISCONTINUITY 0x1u /* media time jumps (seek, new clip) */
#define MTL_SUBMIT_RTP_TS 0x2u        /* send unit.rtp as the RTP timestamp (not AUTO) */
#define MTL_SUBMIT_NOT_BEFORE 0x4u    /* first packet not before launch_tai_ns */
#define MTL_SUBMIT_EXACT 0x8u         /* first packet at launch_tai_ns (non-compliant);
                                         needs MTL_SESSION_EXACT_LAUNCH */
#define MTL_SUBMIT_UNIT_END 0x10u     /* packet units: the last chunk of its unit */
#if defined(MTL_LATER)
/* SENDER mode, inline processors: RTP and the sender report's NTP come from unit.rtp and
   unit.media_tai_ns (a received SENDER_TIME unit); launch = submit + min_tx_delay_ns
   (Phase 7) */
#define MTL_SUBMIT_SENDER_TIME 0x20u
#endif
/* plane[] names caller memory (with stride and rows) that submit reads during the call:
   submit copies or converts it into the slot's own planes, the slot stays the admission
   and back-pressure token, and the result carries MTL_TXR_COPIED. -MTL_EINVAL only on a
   session created with MTL_SESSION_REQUIRE_DIRECT; with an asynchronous converter device,
   submit converts in the caller or returns -MTL_ENOTSUP. */
#define MTL_SUBMIT_SRC_PLANES 0x40u
#define MTL_SUBMIT_MASK 0xffffffffull /* the TX bits; a template contributes these */
/* unit.flags, RX outputs (bits 32-63) */
#define MTL_UNITF_INDEX_VALID 0x100000000ull
#define MTL_UNITF_TAI_VALID 0x200000000ull
#define MTL_UNITF_USED_REDUNDANCY 0x400000000ull /* complete thanks to the other leg */
#define MTL_UNITF_DISCONTINUITY 0x800000000ull
#define MTL_UNITF_FORMAT_CHANGED 0x1000000000ull /* the first unit of a published format
                                                   (detect ON; mtl_rx_detail.format_seq) */
#define MTL_UNITF_PARTIAL 0x2000000000ull      /* rows: more rows follow */
#define MTL_UNITF_SECOND_FIELD 0x4000000000ull /* interlaced: the second field (from the stream) */
#if defined(MTL_LATER)
#define MTL_UNITF_SENDER_TIME 0x8000000000ull  /* media_tai_ns is on the sender's clock
                                                  (mtl_ipmx.h, Phase 7) */
#endif
/* with DISCONTINUITY: the receiver relocked onto a restarted sender, media_index may go
   backwards (timing.md) */
#define MTL_UNITF_RELOCKED 0x10000000000ull

enum mtl_rx_status {
  MTL_RX_COMPLETE = 1,
  MTL_RX_INCOMPLETE = 2, /* lost packets; library pools read them as zero */
};

/* One unit. acquire and dequeue write every field up to struct_size but struct_size
   itself, so MTL_INIT it once; on failure they leave *u unchanged. TX: fill the planes,
   set the per-use fields (used, flags, media time, cookie, hold, launch), submit. RX:
   read, then release the lease. As a template (mtl_tx_write, mtl_tx_send_slot) a unit
   contributes media_index, media_tai_ns, cookie, hold, launch_tai_ns, meta and the TX
   bits of flags, and to mtl_tx_send_slot also used (the slot already holds the content);
   a received unit is a valid template. A template for a session without
   results carries cookie 0; a received unit's cookie is 0 unless it came from a provided
   destination (mtl_rx_provide), so a forwarder over a provide session sets it. */
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
  uint32_t used;     /* video frames: rows, TX 0 = the whole unit; rows units: rows ready,
                        0 legal (the slot is claimed before row 0 exists); packet units:
                        packets; ANC: entries of the packet table (0 = none: the empty
                        RTP packet that keeps the stream alive); audio, cvideo,
                        fastmeta: bytes, literal (audio 0 is -MTL_EINVAL; fastmeta 0 =
                        one RTP packet with no data item, ST 2110-41 §5.1) */
  uint64_t flags;    /* MTL_SUBMIT_* (TX, bits 0-31), MTL_UNITF_* (RX, bits 32-63) */
  uint32_t rtp;      /* TX: with MTL_SUBMIT_RTP_TS; RX: received */
  uint32_t missed_before; /* RX: units the pool could not take since the last one */
  int64_t media_index;  /* TX: MTL_MEDIA_INDEX; RX: with MTL_UNITF_INDEX_VALID */
  int64_t media_tai_ns; /* TX: MTL_MEDIA_TAI; RX: with MTL_UNITF_TAI_VALID */
  uint64_t cookie;      /* TX: returned in the result; non-zero on a session without
                           results: -MTL_EINVAL (COOKIE_WITHOUT_RESULTS), in every build;
                           RX: the destination's cookie on a provide session, else 0 */
  mtl_lease_h hold;     /* TX: an RX lease this unit reads from, kept until its result */
  int64_t launch_tai_ns; /* TX: with NOT_BEFORE or EXACT, and always with PKT_PACE_LAUNCH */
  uint64_t reserved[2];
};

/* The meta area of a slot, TX and RX alike. Frame and row units: records back to back,
   each a struct mtl_meta_hdr and its payload padded to 4 bytes, ended by kind NONE or the
   capacity; at most one record per (kind, tag); TX acquire writes a NONE header at the
   start, and mtl_meta_put()/mtl_meta_find() (mtl_util.h) append and look up records.
   Packet units: no header; the packet table starts at meta, one entry per slot, `used`
   entries valid (mtl_packet.h). TX: the meta area is validated and copied at submit, so
   later writes never reach the wire; plane bytes are read at send time. */
enum mtl_meta_kind { /* 1 is retired: ANC packet tables are plane 0 of their unit */
  MTL_META_NONE = 0,
  MTL_META_USER = 2, /* bytes of user meta follow (video, at most 1332 B) */
#if defined(MTL_LATER)
  MTL_META_RTCP_MIB = 3, /* TX: Media Info Blocks appended to this unit's sender report
                            only (HDR per field); never changes the block version
                            (Phase 7) */
#endif
};
struct mtl_meta_hdr {
  uint16_t kind; /* enum mtl_meta_kind */
  uint16_t tag_version;
  uint32_t count;
  uint32_t bytes; /* after the header */
  uint32_t tag;   /* USER: 0 = untagged */
};
/* ---- ANC units (ST 2110-40, RFC 8331; contract.md §5.7) ------------------------------ */

/* RFC 8331 §2.1 location codes (0x7FE, 0x7FD and 0xFFE-0xFFC are the RFC's other codes,
   carried as they are) */
#define MTL_ANC_LINE_ANY 0x7FFu    /* Line_Number: no specific line */
#define MTL_ANC_HOFFSET_ANY 0xFFFu /* Horizontal_Offset: no specific position */
/* mtl_anc_packet.flags */
#define MTL_ANCF_C 0x01u            /* the C bit: the colour-difference data channel */
#define MTL_ANCF_S 0x02u            /* the S bit: stream carries StreamNum */
#define MTL_ANCF_NEW_RTP 0x04u      /* this ANC packet starts an RTP packet; RX sets it on the
                                       first entry of each RTP packet it received */
#define MTL_ANCF_AS_IS 0x08u        /* the exact line is out of raster order or outside the
                                       unit's field: TX sends it so, timed as unlocated; RX
                                       sets it on such entries of the finished unit */
#define MTL_ANCF_GAP_BEFORE 0x10u   /* RX: ANC packets were skipped, truncated or lost (an RTP
                                       sequence gap left when the unit was dequeued) just before
                                       this entry; TX ignores it */
#define MTL_ANCF_PARITY_ERR 0x20u   /* RX, RAW: the DID, SDID or Data_Count parity is wrong;
                                       TX ignores it */
#define MTL_ANCF_CHECKSUM_ERR 0x40u /* RX, RAW: the Checksum_Word is wrong; TX ignores it */
/* One ANC packet, a row of plane 0. Outside RAW, MTL writes every derived RFC 8331 field
   from it: DID, SDID or DBN and Data_Count with b8/b9, the 8-bit words' b8/b9, the checksum
   and word_align; in every mode Length, ANC_Count, F, the marker, the sequence numbers and
   the timestamp of each RTP packet. Every entry RX delivers is a valid TX entry. */
struct mtl_anc_packet {
  uint8_t did;        /* b7-b0 of the DID: 01h-FFh, 00h is reserved (ST 291-1 §6.1); 80h
                         and above is Type 1. RAW: the low byte of the run's first word */
  uint8_t sdid;       /* Type 2: the SDID, 01h-FFh (§6.2); Type 1: the DBN, 00h = inactive
                         (§6.4) */
  uint8_t udw_count;  /* Data_Count: user data words, 0-255 (§6.5) */
  uint8_t flags;      /* MTL_ANCF_* */
  uint16_t line;      /* Line_Number as on the wire: the SDI line, or an RFC 8331 code */
  uint16_t hoffset;   /* Horizontal_Offset: 10-bit words after SAV (0 is a real position),
                         or an RFC 8331 code */
  uint8_t stream;     /* StreamNum, 0-127, with MTL_ANCF_S */
  uint8_t reserved;   /* 0 */
  uint16_t rtp_index; /* RX: the sequence distance of its RTP packet from the one after the
                         previous unit's marker (from the unit's first received packet when
                         that is unknown; 65535: that or later); TX ignores it */
  uint32_t udw_offset; /* its first word: a row of plane 1 */
};

/* ---- Data path ------------------------------------------------------------------------ */

/* TX: a writable slot. 0, or -MTL_EAGAIN (none free by the timeout: status.blocked_on says
   why), -MTL_ECANCELED (with a timeout), -MTL_ESHUTDOWN, -MTL_EIO, ... WT (Waiting,
   below). (MS1) */
MTL_API_WT(1) int mtl_tx_acquire(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);
/* Hands the unit to MTL; exactly one result follows (if results are on). The results
   ring holds pool_count entries and acquire reserves one, so producing a result never
   waits. A failed first submit returns the slot to the pool without a result, except
   -MTL_EBADF and -MTL_ESTALE, which change no state. Rows units: submit the same lease
   again with a larger `used` to publish more rows; once a submit was accepted, a later
   failing submit ends the unit where its rows stopped (option tx.rows_late), consumes the
   lease, and the unit's one result follows when its packets have left. DPC. (MS1) */
MTL_API_DPC(1) int mtl_tx_submit(mtl_session_h s, const struct mtl_unit* u);

/* RX: a received unit, in delivery order. Missing packets read as zero (library pools)
   and `status` says so. Video and cvideo library pools: the MTL_RX_TAIL_BYTES after the
   unit (video: plane 0 + the sum of stride x rows; cvideo: plane 0 + used) are zero when
   it returns. WT (Waiting, below); DPC when it works on the unit in the caller: packet
   units without MTL_PKT_RX_LEND (mtl_packet.h), which it copies; a video.app_format
   conversion; ANC units, which it decodes; the zero fill of a library-pool unit with lost
   packets (MS2). It claims the unit first and works outside every lock, so concurrent
   dequeues of one session work in parallel. (MS1) */
MTL_API_WT(1) int mtl_rx_dequeue(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);

/* Returns a lease to s. TX: an acquired, unsubmitted one, with no result (a submitted one
   is -MTL_ESTALE); a call waiting in acquire is woken (R6). RX: a dequeued one, from any
   thread, in any order. DP. (MS1) */
MTL_API_DP(1) int mtl_release(mtl_session_h s, mtl_lease_h lease);
static inline int mtl_tx_release(mtl_session_h s, mtl_lease_h lease) {
  return mtl_release(s, lease);
}
static inline int mtl_rx_release(mtl_session_h s, mtl_lease_h lease) {
  return mtl_release(s, lease);
}

/* What happened to one submitted TX unit. */
enum mtl_tx_status {
  MTL_TX_ON_TIME = 1,
#if defined(MTL_LATER)
  MTL_TX_LATE = 2,    /* sent late within tx.late_tolerance_ns (MTL_LATE_SEND_LATE, Phase 7) */
#endif
  MTL_TX_DROPPED = 3, /* not sent; `reason` says why; its index stays empty on the wire */
  MTL_TX_FLUSHED = 4, /* stop(FLUSH), discard, close, abort, ERROR, before start */
  MTL_TX_FAILED = 5,  /* device or queue failure; `error` and `reason` */
};
/* mtl_tx_result.flags */
#define MTL_TXR_MEDIA_VALID 0x1u  /* media_index, media_tai_ns */
#define MTL_TXR_MARGIN_VALID 0x2u /* margin_ns */
#define MTL_TXR_SENT_VALID 0x4u   /* sent_tai_ns */
#define MTL_TXR_SENT_HW 0x8u      /* sent_tai_ns is a NIC timestamp */
#define MTL_TXR_ESTIMATED 0x10u   /* the time base was not locked */
#define MTL_TXR_SNAPPED 0x20u     /* TAI media time moved to the grid */
#define MTL_TXR_DEFERRED 0x40u    /* AUTO unit moved to a later index (ON_TIME, MTL_LATE_DEFER) */
#define MTL_TXR_COPIED 0x80u      /* this unit took a copy path */
#define MTL_TXR_PKT_SHORT 0x100u  /* packet units: fewer packets than declared */
/* The core result record; mtl_observe.h has the full one (mtl_tx_reap_full). */
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
/* The TX results of a session: up to max in submission order, each rec_size bytes apart
   (a larger record is filled further, mtl_observe.h). A count >= 1, or -MTL_EAGAIN when
   there is none; a reap that frees result space wakes a call waiting in acquire (R6). The
   typed wrappers pass the size. WT (Waiting, below). (MS1) */
MTL_API_WT(1) int mtl_reap(struct mtl_object o, void* rec, size_t rec_size, uint32_t max,
                           int64_t timeout_ns);
static inline int mtl_tx_reap(mtl_session_h s, struct mtl_tx_result* r, uint32_t max,
                              int64_t timeout_ns) {
  return mtl_reap(MTL_OBJ_OF_SESSION(s), r, sizeof(*r), max, timeout_ns);
}

/* ---- Waiting ----------------------------------------------------------------------------- */

#define MTL_WAIT_ACQUIRE 0x1u /* TX: acquire would succeed */
#define MTL_WAIT_DEQUEUE 0x2u /* RX: dequeue would succeed */
#define MTL_WAIT_RESULTS 0x4u /* TX: reap would return a result */
#define MTL_WAIT_EVENTS 0x8u  /* events are pending (mtl_events.h) */
#if defined(MTL_LATER)
#define MTL_WAIT_RTCP 0x20u    /* RX: mtl_rtcp_read() would return a report (mtl_ipmx.h,
                                  Phase 7) */
#endif
/* Wait targets. With timeout 0 a data call (acquire, dequeue, reap, read, wait) tries once
   and changes no wait state. With a timeout it sleeps on its object: any number of threads
   may block on one object, on one target or on different ones, and a wake reaches every
   thread blocked on its target and no other (beyond 127 on one target, the others re-check
   every 1 ms). It returns when its target is ready, its timeout passes, it is interrupted,
   or the state ends it (contract.md §7.4). A call with a timeout is not ended by
   pthread_cancel: end a wait with mtl_interrupt(), and never longjmp out of an MTL call.
   An event loop waits on a queue (below). */
/* Waits on a session, or on an instance (MTL_WAIT_EVENTS only: its port, time, scheduler,
   health and manager events). > 0: the ready subset of mask, consuming nothing. Nothing
   ready by the timeout: -MTL_EAGAIN. An interrupted target of mask returns -MTL_ECANCELED
   with any timeout. -MTL_ESHUTDOWN, -MTL_EIO, ...; mask 0 or a bit not declared outside
   MTL_LATER: -MTL_EINVAL (0 = "every target" is mtl_interrupt's only); a declared bit of a
   later milestone: -MTL_ENOTSUP (R1). WT. (MS1) */
MTL_API_WT(1) int mtl_wait(struct mtl_object o, uint64_t mask, int64_t timeout_ns);
static inline int mtl_session_wait(mtl_session_h s, uint64_t mask, int64_t timeout_ns) {
  return mtl_wait(MTL_OBJ_OF_SESSION(s), mask, timeout_ns);
}

/* ---- Queues (MS2) -------------------------------------------------------------------- */

/* A queue tells an event loop, or a pool of threads, which of its objects changed, through
   one descriptor for any number of sessions and the instance. */
typedef struct mtl_queue_h {
  uint64_t id;
} mtl_queue_h;
#define MTL_OBJ_OF_QUEUE(q) mtl_obj(MTL_OBJ_QUEUE, 0, (q).id)
/* mtl_ready.fired: o's state changed since the arm (start, stop, the end of DRAIN or FLUSH,
   ERROR, a recovery); mtl_session_get_status says to what. o stays attached. */
#define MTL_READY_STATE 0x80000000u
/* One report. */
struct mtl_ready {
  struct mtl_object o; /* the session or the instance */
  uint64_t user;       /* from the last mtl_queue_arm of o on this queue */
  uint32_t fired;      /* the armed MTL_WAIT_* targets whose change reported o, and
                          MTL_READY_STATE; 0 is possible: try every armed target */
  uint32_t reserved;   /* 0 */
};
/* Creates a queue of mt. native not NULL: the queue's descriptor, written to *native (a
   Linux eventfd, poll POLLIN; from MS2a a Windows manual-reset event HANDLE); the
   application never reads or writes it, and removes it from its event loop before
   mtl_queue_close(q). MTL keeps the descriptor open while the process runs (a forked
   child closes it) and gives it to a later queue, so their number is bounded by the peak
   number of live queues. native NULL: a queue without a descriptor, for a consumer that
   polls it on its own timer: mtl_queue_wait on it takes timeout 0, and it never costs a
   wake-up. flags: 0. -MTL_ENOSPC without a descriptor (DESCRIPTOR_LIMIT). CP. (MS2) */
MTL_API_CP(2) int mtl_queue_create(mtl_instance_h mt, uint32_t flags, mtl_queue_h* out,
                                   intptr_t* MTL_NULLABLE native);
/* Arms targets (MTL_WAIT_*) of o, a session or the instance of q, on q: the first change of
   one of them, or of o's state, reports o once, with user; a target ready now reports o at
   once. A report disarms o on q: serve it with timeout-0 calls, then arm it again with the
   targets you want next (a sender limited by its source arms MTL_WAIT_ACQUIRE only while a
   frame waits). 0: armed. 1: a report of o is already on its way; it carries this user,
   and o is armed again only by the next call after it. targets 0 detaches o and returns
   once every mtl_queue_wait that was handing out a report of o has returned: no report
   carrying its user is returned after that, nor after o's close returned, so user may be
   freed then. A target armed on another queue: -MTL_EBUSY; o on 4 queues: -MTL_ENOSPC; o
   closing or closed: -MTL_ESHUTDOWN (or -MTL_EBADF, R4) and o is detached. Arming succeeds
   in every other state. No syscall, except one write() when it reports o at once to a
   sleeping queue. DP. (MS2) */
MTL_API_DP(2) int mtl_queue_arm(mtl_queue_h q, struct mtl_object o, uint64_t targets,
                                uint64_t user);
/* Up to max reports, each r_size bytes apart, oldest first, each object once. A count >= 1;
   -MTL_EAGAIN: nothing by the timeout, and q's descriptor is armed (timeout 0 costs one
   read() of it): sleep on the descriptor only after -MTL_EAGAIN. On a queue without a
   descriptor: timeout 0 only (else -MTL_EINVAL), and nothing is armed. -MTL_ECANCELED
   while q or its instance is interrupted, with any timeout; -MTL_ESHUTDOWN for a wait in
   progress when q closes. Any number of threads may call it; each report goes to one of
   them. WT; with timeout 0 one read() and at most one write() of the descriptor. (MS2) */
MTL_API_WT(2) int mtl_queue_wait(mtl_queue_h q, struct mtl_ready* r, size_t r_size,
                                 uint32_t max, int64_t timeout_ns);
/* Detaches every object and ends the waits in progress on q with -MTL_ESHUTDOWN; later
   calls on q are -MTL_EBADF. 0 for a null handle. CP. */
static inline int mtl_queue_close(mtl_queue_h q) {
  return mtl_close(MTL_OBJ_OF_QUEUE(q), 0);
}
/* on = 1: every mtl_queue_wait on q returns -MTL_ECANCELED until on = 0. AS with on = 1, CP
   with on = 0. */
static inline int mtl_queue_interrupt(mtl_queue_h q, int on) {
  return mtl_interrupt(MTL_OBJ_OF_QUEUE(q), on ? MTL_INTR_ON : MTL_INTR_OFF, 0);
}
/* on = 1: every wait on s returns -MTL_ECANCELED until on = 0 (GStreamer unlock and
   unlock_stop; mtl_interrupt() limits it to some targets); on a closing session it does
   nothing and returns 0. AS with on = 1, CP with on = 0. */
static inline int mtl_session_interrupt(mtl_session_h s, int on) {
  return mtl_interrupt(MTL_OBJ_OF_SESSION(s), on ? MTL_INTR_ON : MTL_INTR_OFF, 0);
}

/* ---- Size checks (64-bit targets) ---------------------------------------------------------- */

MTL_SIZE_CHECK(mtl_error_info, 184);
MTL_SIZE_CHECK(mtl_instance_h, 8);
MTL_SIZE_CHECK(mtl_session_h, 8);
MTL_SIZE_CHECK(mtl_lease_h, 8);
MTL_SIZE_CHECK(mtl_queue_h, 8);
MTL_SIZE_CHECK(mtl_ready, 32);
MTL_SIZE_CHECK(mtl_region_h, 8);
#if defined(MTL_LATER)
MTL_SIZE_CHECK(mtl_timeline_h, 8);
#endif
MTL_SIZE_CHECK(mtl_object, 16);
MTL_SIZE_CHECK(mtl_rational, 16);
MTL_SIZE_CHECK(mtl_option, 24);
MTL_SIZE_CHECK(mtl_port_spec, 160);
MTL_SIZE_CHECK(mtl_instance_params, 120);
MTL_SIZE_CHECK(mtl_flow, 96);
MTL_SIZE_CHECK(mtl_raster, 32);
MTL_SIZE_CHECK(mtl_video_config, 128);
MTL_SIZE_CHECK(mtl_cvideo_config, 128);
MTL_SIZE_CHECK(mtl_audio_config, 128);
MTL_SIZE_CHECK(mtl_anc_config, 128);
MTL_SIZE_CHECK(mtl_fastmeta_config, 128);
MTL_SIZE_CHECK(mtl_rtp_config, 128);
MTL_SIZE_CHECK(mtl_packet_config, 64);
MTL_SIZE_CHECK(mtl_session_config, 1232);
MTL_SIZE_CHECK(mtl_leg_info, 28);
MTL_SIZE_CHECK(mtl_session_info, 288);
MTL_SIZE_CHECK(mtl_when, 48);
MTL_SIZE_CHECK(mtl_leg_status, 16);
MTL_SIZE_CHECK(mtl_session_status, 112);
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
