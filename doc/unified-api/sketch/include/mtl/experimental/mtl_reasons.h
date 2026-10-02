/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_reasons.h - reason codes of the unified MTL API, revision 0.2. The implementation
 * generates this file, mtl_reason_name() and the test trigger list from one table; the
 * table, with each reason's trigger, is contract.md §8.3.
 *
 * One vocabulary for mtl_error_info.reason, status.reason/.error_reason, event reasons and
 * TX result reasons. Grouped by hundreds so each group can grow; values are frozen once
 * published. Include it to branch on a reason; mtl_reason_name() is enough to log one.
 */

#ifndef MTL_EXPERIMENTAL_MTL_REASONS_H
#define MTL_EXPERIMENTAL_MTL_REASONS_H

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
  MTL_REASON_UPDATE_COMMITTING = 12, /* cancel came too late: the update applies */
  /* 100-199: device, port, network */
  MTL_REASON_TX_QUEUE_FATAL = 100,
  MTL_REASON_TX_QUEUE_HANG = 101,
  MTL_REASON_LINK_DOWN = 102,
  MTL_REASON_PORT_RESET = 103,
  MTL_REASON_DEVICE_GONE = 104,
  MTL_REASON_BACKEND_FAILED = 105,
  MTL_REASON_PORT_NOT_OPEN = 106,
  MTL_REASON_WAITING_NEIGHBOUR = 107, /* also the TX drop reason "no neighbour" */
  MTL_REASON_JOIN_FAILED = 108,
  MTL_REASON_LEG_DISABLED = 109,
  MTL_REASON_MANAGER_LOST = 110,
  MTL_REASON_SCHED_OVERLOAD = 111,
  MTL_REASON_SCHED_STALLED = 112,
  MTL_REASON_WORKER_STALLED = 113,
  MTL_REASON_QUEUE_QUARANTINED = 114, /* not quiesced: its memory is never freed (deployment.md) */
  MTL_REASON_MCAST_FILTERS = 115,     /* the VF's multicast filter budget is used up */
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
  MTL_REASON_TIMELINE_MISMATCH = 221,
  MTL_REASON_START_SET_MIXED = 222, /* a start array mixes timelines or directions, or
                                       holds an AUTO TX session */
  MTL_REASON_BUSY_LOOP_THREAD = 223, /* -MTL_EDEADLK */
  MTL_REASON_LIBRARY_THREAD = 227,   /* -MTL_EDEADLK: close from a thread it would join */
  MTL_REASON_PORT_CHANGE_NEEDS_STOP = 224,
  MTL_REASON_GRID_MISMATCH = 225,
  MTL_REASON_PKT_CONFIG = 226, /* packet sizes or counts do not fit the port or pacing */
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
  /* 400-499: timing */
  MTL_REASON_BEYOND_HORIZON = 400,
  MTL_REASON_START_IN_PAST = 401,
  MTL_REASON_LAUNCH_IN_PAST = 402,
  MTL_REASON_TIMING_SHORTFALL = 403,
  MTL_REASON_LINK_OFFSET_BUDGET = 404,
  MTL_REASON_TIME_ESTIMATED = 405,
  MTL_REASON_TIME_STEP = 406,
  /* 500-599: why a TX unit was not on time (mtl_tx_result.reason) */
  MTL_REASON_TOO_LATE = 500,
  MTL_REASON_WOULD_OVERLAP = 501,
  MTL_REASON_DUPLICATE_SLOT = 502,  /* TAI: two units snapped to one slot */
  MTL_REASON_DUPLICATE_INDEX = 503, /* INDEX: an index equal to the last one */
  MTL_REASON_BEHIND = 504,          /* INDEX: an index smaller than the last one */
  MTL_REASON_OFF_GRID = 505,
  MTL_REASON_RECOVERY = 506,
  MTL_REASON_INVALID_PAYLOAD = 507,
  MTL_REASON_ENCODER_FAILED = 508,
  MTL_REASON_REJECTED_AT_PICKUP = 509,
  MTL_REASON_STOP_FLUSH = 510,
  MTL_REASON_DISCARD = 511,
  MTL_REASON_WITHDRAWN = 512,
  MTL_REASON_CLOSE = 513,
  MTL_REASON_BEFORE_START = 514,
  MTL_REASON_SESSION_ERROR = 515,
  MTL_REASON_PKT_COUNT = 516,   /* packet units: more packets than packets_per_unit */
  MTL_REASON_PKT_INVALID = 517, /* packet units: VALIDATE found a malformed packet */
  MTL_REASON_STOP_TIMEOUT = 518, /* flushed because a DRAIN stop or close missed its deadline */
  MTL_REASON_NO_KEY = 519,       /* encrypted session without a key for the unit (mtl_crypto.h) */
  MTL_REASON_ABORTED = 520,      /* mtl_instance_abort() */
  /* 600-699: the environment: devices, privileges, limits (checked at open, deployment.md) */
  MTL_REASON_NO_IOMMU = 600,          /* no-IOMMU or PA mode without instance.allow_noiommu */
  MTL_REASON_DEVICE_NODE_MISSING = 601, /* /dev/vfio/... not in the container */
  MTL_REASON_DRIVER_MISMATCH = 602,   /* the device is not bound to the expected driver */
  MTL_REASON_HUGEPAGES_LIMIT = 603,   /* the cgroup hugetlb limit or the free pages */
  MTL_REASON_MEMLOCK_LIMIT = 604,     /* RLIMIT_MEMLOCK without CAP_IPC_LOCK */
  MTL_REASON_CAPABILITY_MISSING = 605, /* detail names it */
  MTL_REASON_CPU_NOT_ALLOWED = 606,   /* a CPU outside the affinity mask */
  MTL_REASON_CPU_SHARED = 607,        /* busy-poll CPUs under a CFS quota or shared */
  MTL_REASON_PORT_ENV_UNSET = 608,    /* "env:VAR" unset, or #n out of range */
  MTL_REASON_RUNTIME_DIR = 609,       /* instance.runtime_dir missing or not writable */
  MTL_REASON_MANAGER_REQUIRED = 610,  /* the configuration needs MtlManager */
  MTL_REASON_XSK_UNAVAILABLE = 611,   /* AF_XDP: no socket or map from the node agent */
  MTL_REASON_VF_UNTRUSTED = 612,      /* the request needs a trusted VF */
  MTL_REASON_TIME_SOURCE_UNAVAILABLE = 613, /* the named time source cannot be read */
  MTL_REASON_CLOCK_NOT_OWNED = 614,   /* PTP_BUILTIN asked to steer a PHC MTL does not own */
};

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_REASONS_H */
