/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_reasons.h - reason codes of the unified MTL API, revision 0.2. From task H1b the enum
 * below is generated from lib/src/unified/reasons.def, which also builds mtl_reason_name()
 * and the G-57 trigger list; contract.md §8.3 is the same table with each reason's codes
 * and trigger. Edit the .def, never the enum (doc/unified-api/README.md §1).
 *
 * One vocabulary for mtl_error_info.reason, status.reason/.error_reason, event reasons and
 * TX result reasons. Grouped by hundreds so each group can grow; values are frozen once
 * published, and the reasons of later items are declared under MTL_LATER with their
 * values kept. Include it to branch on a reason; mtl_reason_name() is enough to log one.
 */

#ifndef MTL_EXPERIMENTAL_MTL_REASONS_H
#define MTL_EXPERIMENTAL_MTL_REASONS_H

/* BEGIN TABLE reason-enum: lib/src/unified/reasons.def; gen_api_doc.py writes this region
   once that file exists (README §1) */
enum mtl_reason {
  MTL_REASON_NONE = 0,
  /* 1-99: lifecycle */
  MTL_REASON_APP_REQUEST = 1,
  MTL_REASON_START_INSTANT = 2,
  MTL_REASON_DRAIN_COMPLETE = 3,
  MTL_REASON_DRAIN_TIMEOUT = 4,
  MTL_REASON_CMD_TIMEOUT = 5, /* ERROR: a command was not acknowledged in time */
  MTL_REASON_FORCED = 6,      /* ERROR from mtl_debug_inject() */
  MTL_REASON_WRONG_STATE = 7,
  MTL_REASON_START_SET = 8, /* a session of the same start failed */
  MTL_REASON_CLOSE_IN_PROGRESS = 9,
  MTL_REASON_INSTANCE_SHUTDOWN = 10, /* closed by the instance's close or shutdown */
  MTL_REASON_FORKED = 11,            /* a call in a forked child (mtl.h R8) */
#if defined(MTL_LATER)
  MTL_REASON_UPDATE_COMMITTING = 12, /* cancel came too late: the update applies (Phase 7) */
#endif
  /* 100-199: device, port, network */
  MTL_REASON_TX_QUEUE_FATAL = 100,
  MTL_REASON_TX_QUEUE_HANG = 101,
  MTL_REASON_LINK_DOWN = 102,
  MTL_REASON_PORT_RESET = 103,
  MTL_REASON_DEVICE_GONE = 104,
  MTL_REASON_BACKEND_FAILED = 105,
  MTL_REASON_PORT_NOT_OPEN = 106, /* -MTL_EINVAL: a flow or option names a port not opened */
  MTL_REASON_WAITING_NEIGHBOUR = 107, /* also the TX drop reason "no neighbour" */
  MTL_REASON_JOIN_FAILED = 108,
  MTL_REASON_LEG_DISABLED = 109,
  MTL_REASON_MANAGER_LOST = 110,
  MTL_REASON_SCHED_OVERLOAD = 111,
  MTL_REASON_SCHED_STALLED = 112,
  MTL_REASON_WORKER_STALLED = 113,
  MTL_REASON_QUEUE_QUARANTINED = 114, /* not quiesced: its memory is never freed (deployment.md) */
  MTL_REASON_MCAST_FILTERS = 115,     /* the VF's multicast filter budget is used up */
  MTL_REASON_DETECT_FAILED = 116,     /* status.format_reason: detection cannot identify the
                                         stream's raster; it keeps trying */
  /* 200-299: configuration and arguments (detail and field name the culprit) */
  MTL_REASON_INVALID_ARGUMENT = 200,
  MTL_REASON_UNKNOWN_BITS = 201,
  MTL_REASON_NONZERO_TAIL = 202, /* bytes beyond the known size, or struct_size 0 */
  MTL_REASON_FIELD_REQUIRED = 203,
  MTL_REASON_OTHER_ESSENCE = 204, /* a non-zero member for another essence */
  MTL_REASON_INSTANCE_MISMATCH = 205, /* a shared open asks for an incompatible setting */
  MTL_REASON_OPTION_UNKNOWN = 206,
  MTL_REASON_OPTION_RANGE = 207,
  MTL_REASON_OPTION_STATE = 208, /* the key cannot change in this state */
  MTL_REASON_COOKIE_WITHOUT_RESULTS = 209,
  MTL_REASON_MEDIA_TIME_BACKWARDS = 210,
  MTL_REASON_RTP_TS_AUTO = 211, /* MTL_SUBMIT_RTP_TS with media mode AUTO */
  MTL_REASON_LAYOUT_MISMATCH = 212,
  MTL_REASON_STRIDE_MISMATCH = 213,
  MTL_REASON_ACCESS_MISMATCH = 214,
  MTL_REASON_UNALIGNED = 215,
  MTL_REASON_SPAN = 216, /* a slot outside its region */
  MTL_REASON_MIXED_BACKING = 217,
  MTL_REASON_POOL_TOO_SMALL = 218,
  MTL_REASON_HOLD_REQUIRED = 219, /* a pool over another session's pool needs hold */
  MTL_REASON_NAME_EXISTS = 220,
#if defined(MTL_LATER)
  MTL_REASON_TIMELINE_MISMATCH = 221, /* a named timeline created again with another config */
#endif
  MTL_REASON_START_SET_MIXED = 222, /* a start array mixes directions, or holds an AUTO TX
                                       session */
  MTL_REASON_PORT_CHANGE_NEEDS_STOP = 224,
#if defined(MTL_LATER)
  MTL_REASON_GRID_MISMATCH = 225, /* a unit period that does not fit a timeline's grid */
#endif
  MTL_REASON_PKT_CONFIG = 226, /* packet sizes or counts do not fit the port or pacing */
  MTL_REASON_LIBRARY_THREAD = 227,   /* -MTL_EDEADLK: open, close or shutdown from a library
                                        thread */
  MTL_REASON_NOT_IMPLEMENTED = 228, /* -MTL_ENOTSUP: a known item this library does not
                                       implement yet; field names it (mtl.h R1) */
  MTL_REASON_RASTER_MISMATCH = 229, /* an ANC or grid-fastmeta raster that differs from the
                                       first video session of its start */
#if defined(MTL_LATER)
  MTL_REASON_RX_TIMELINE_LAZY = 230, /* -MTL_EINVAL: an RX session on an AT_START timeline */
#endif
  MTL_REASON_BACKEND_REMOVED = 231,  /* -MTL_ENOTSUP: a removed port prefix ("dpdk_af_xdp:",
                                        "dpdk_af_packet:"); detail names the replacement */
  MTL_REASON_OPTION_WITHDRAWN = 232, /* -MTL_ENOTSUP: a provisional key this library no
                                        longer has (mtl_options.h) */
  MTL_REASON_FIELD_RATE = 233,       /* -MTL_EINVAL: an interlaced or PsF raster.fps above 30
                                        frames per second, a field rate given as the frame
                                        rate */
  MTL_REASON_NOT_APPLICABLE = 234,   /* -MTL_EINVAL: a session key whose essence, direction
                                        or unit mask excludes the session, an instance or
                                        port key on a session (mtl_options.h), or a flag or
                                        value outside the essences and directions it
                                        applies to; field names it */
  /* 300-399: capacity and memory */
  MTL_REASON_CAPACITY_TX_QUEUES = 300,
  MTL_REASON_CAPACITY_RX_QUEUES = 301,
  MTL_REASON_CAPACITY_RL_QUEUES = 302,
  MTL_REASON_CAPACITY_SCHED_QUOTA = 303,
  MTL_REASON_CAPACITY_LCORES = 304,
  MTL_REASON_CAPACITY_SESSIONS = 305,
  MTL_REASON_REGION_BUDGET = 306,
  MTL_REASON_CODESTREAM_OVERSIZE = 307,
  MTL_REASON_HUGEPAGES = 308,
  MTL_REASON_NUMA_MISMATCH = 309,
  MTL_REASON_DIRECT_IMPOSSIBLE = 310,
  MTL_REASON_PACING_UNAVAILABLE = 311,
  MTL_REASON_POOL_COUNT_MAX = 312,
  MTL_REASON_RX_RING_BUDGET = 313, /* packet RX: NIC buffers the session may hold */
  MTL_REASON_PROVIDE_FULL = 314,   /* mtl_rx_provide beyond pool_count destinations held */
  /* 400-499: timing */
  MTL_REASON_BEYOND_HORIZON = 400,
  MTL_REASON_START_IN_PAST = 401,
  MTL_REASON_LAUNCH_IN_PAST = 402,
  MTL_REASON_TIMING_SHORTFALL = 403,
  MTL_REASON_LINK_OFFSET_BUDGET = 404,
  MTL_REASON_TIME_ESTIMATED = 405,
  MTL_REASON_TIME_STEP = 406,
#if defined(MTL_LATER)
  MTL_REASON_OFF_GRID_PHASE = 407, /* status.timing_reason: a LOCKED_PHASE session relocked,
                                      or drops off-grid units (MTL_OFF_GRID_DROP) */
#endif
  /* 500-599: why a TX unit was not on time (mtl_tx_result.reason) */
  MTL_REASON_TOO_LATE = 500,
#if defined(MTL_LATER)
  MTL_REASON_WOULD_OVERLAP = 501, /* bounded SEND_LATE, MTL_MEDIA_SENDER (Phase 7) */
#endif
  MTL_REASON_SNAP_COLLISION = 502,  /* TAI: two units snapped to one media index */
  MTL_REASON_DUPLICATE_INDEX = 503, /* INDEX: an index equal to the last one */
  MTL_REASON_BEHIND = 504,          /* INDEX: an index smaller than the last one */
#if defined(MTL_LATER)
  MTL_REASON_OFF_GRID = 505, /* LOCKED_PHASE: a TAI media time off the grid */
#endif
  MTL_REASON_RECOVERY = 506,
  MTL_REASON_INVALID_PAYLOAD = 507,
  MTL_REASON_ENCODER_FAILED = 508,
  MTL_REASON_REJECTED_AT_PICKUP = 509,
  MTL_REASON_STOP_FLUSH = 510,
  MTL_REASON_DISCARD = 511,
  MTL_REASON_CLOSE = 513,
  MTL_REASON_BEFORE_START = 514,
  MTL_REASON_SESSION_ERROR = 515,
  MTL_REASON_PKT_COUNT = 516,   /* packet units: more packets than packets_per_unit */
  MTL_REASON_PKT_INVALID = 517, /* packet units: VALIDATE found a malformed packet */
  MTL_REASON_STOP_TIMEOUT = 518, /* flushed because a DRAIN stop or close missed its deadline */
#if defined(MTL_LATER)
  MTL_REASON_NO_KEY = 519,       /* encrypted session without a key for the unit (mtl_ipmx.h,
                                    Phase 7) */
#endif
  MTL_REASON_ABORTED = 520,      /* mtl_instance_abort() */
  /* 600-699: the environment: devices, privileges, limits (checked at open, deployment.md);
     each with the code mtl_instance_open returns (VF_UNTRUSTED: mtl_session_create) */
  MTL_REASON_NO_IOMMU = 600,          /* -MTL_ENOTSUP: no-IOMMU or PA mode without
                                         instance.allow_noiommu */
  MTL_REASON_DEVICE_NODE_MISSING = 601, /* -MTL_ENODEV: /dev/vfio/... not in the container */
  MTL_REASON_DRIVER_MISMATCH = 602,   /* -MTL_ENODEV: the device is not bound to the expected
                                         driver */
  MTL_REASON_HUGEPAGES_LIMIT = 603,   /* -MTL_ENOMEM: the cgroup hugetlb limit or the free
                                         pages */
  MTL_REASON_MEMLOCK_LIMIT = 604,     /* -MTL_ENOMEM: RLIMIT_MEMLOCK without CAP_IPC_LOCK */
  MTL_REASON_CAPABILITY_MISSING = 605, /* -MTL_ENOTSUP: detail names it */
  MTL_REASON_CPU_NOT_ALLOWED = 606,   /* -MTL_EINVAL: a CPU outside the affinity mask */
  MTL_REASON_CPU_SHARED = 607,        /* -MTL_ENOSPC: busy-poll CPUs under a CFS quota or
                                         shared, with MTL_CPU_SHARED_REFUSE */
  MTL_REASON_PORT_ENV_UNSET = 608,    /* -MTL_EINVAL: "env:VAR" unset, or #n out of range */
  MTL_REASON_RUNTIME_DIR = 609,       /* -MTL_EINVAL: instance.runtime_dir missing or not
                                         writable */
  MTL_REASON_MANAGER_REQUIRED = 610,  /* -MTL_ENOTSUP: the configuration needs MtlManager */
  MTL_REASON_XSK_UNAVAILABLE = 611,   /* -MTL_ENODEV: AF_XDP: no socket or map from the node
                                         agent */
  MTL_REASON_VF_UNTRUSTED = 612,      /* -MTL_ENOTSUP at create: the request needs a trusted
                                         VF */
  MTL_REASON_TIME_SOURCE_UNAVAILABLE = 613, /* -MTL_ENOTSUP: the named time source cannot be
                                               read */
  MTL_REASON_CLOCK_NOT_OWNED = 614,   /* -MTL_EINVAL: PTP_BUILTIN asked to steer a PHC MTL
                                         does not own */
  MTL_REASON_DESCRIPTOR_LIMIT = 615,  /* -MTL_ENOSPC: no descriptor for a queue
                                         (RLIMIT_NOFILE or the system's file limit) */
};
/* END TABLE reason-enum */

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_REASONS_H */
