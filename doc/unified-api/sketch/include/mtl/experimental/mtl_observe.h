/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* A hand-formatted design sketch: formatted when it moves to include/ (milestone M0). */
/* clang-format off */
/*
 * mtl_observe.h - stats, diagnostics, logging and capture for the unified MTL API,
 * revision 0.2. A media loop never needs this header; exporters, consoles, tests and the
 * entry points of a containerised service (health probes, bounded shutdown) do.
 *
 * Every number MTL can report about an object (session, port, scheduler, instance) is a
 * named int64_t value in one registry: "tx.units_late", "rx.pkts_lost{leg=1}",
 * "port.rx_missed", "sched.busy_pct_x100", "info.trs_ps". Names are the stable contract
 * (the key catalogue is in contract.md); labels follow Prometheus syntax. List the schema once,
 * then read values in bulk: mtl_stat_read() is DP and never takes a lock a tasklet takes.
 * A value a backend cannot produce is absent from the schema, never a silent zero.
 * Counters are cumulative for the life of the object. The schema can grow (a leg added,
 * a reason seen for the first time): its generation changes, and a bulk read against an
 * old generation fails with -MTL_ESTALE, so the reader lists again.
 */

#ifndef MTL_EXPERIMENTAL_MTL_OBSERVE_H
#define MTL_EXPERIMENTAL_MTL_OBSERVE_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- Stats registry ---------------------------------------------------------------- */

enum mtl_stat_kind {
  MTL_STAT_COUNTER = 1,
  MTL_STAT_GAUGE = 2,
  MTL_STAT_CONST = 3, /* granted configuration and capabilities ("info.*", "caps.*") */
  MTL_STAT_HIST = 4,  /* 4 + MTL_HIST_BUCKETS slots: count, sum, min, max, buckets */
};
enum mtl_stat_unit {
  MTL_SU_COUNT = 1,
  MTL_SU_NS = 2,
  MTL_SU_PS = 3,
  MTL_SU_BYTES = 4,
  MTL_SU_PACKETS = 5,
  MTL_SU_SAMPLES = 6,
  MTL_SU_PCT_X100 = 7,
  MTL_SU_MBPS = 8,
  MTL_SU_TAI_NS = 9, /* INT64_MIN = never */
  MTL_SU_ENUM = 10,
  MTL_SU_MASK = 11,
};
#define MTL_HIST_BUCKETS 16
#define MTL_STAT_ESTIMATE 0x1u /* inexact on this backend, e.g. packet loss */
#define MTL_STAT_CACHED 0x2u   /* polled off the tasklet; "<scope>.sampled_tai_ns" says when */
/* Output value type of the schema. */
struct mtl_stat_desc {
  char name[48];  /* "tx.units_dropped" */
  char label[48]; /* "reason=too_late", "leg=1", "essence=fastmeta,dir=tx", "" */
  uint32_t slot;  /* index of the first value in mtl_stat_read() */
  uint16_t width; /* slots: 1, or 4 + MTL_HIST_BUCKETS */
  uint16_t kind;  /* enum mtl_stat_kind */
  uint16_t unit;  /* enum mtl_stat_unit */
  uint16_t flags; /* MTL_STAT_* */
  uint32_t reserved;
  int64_t bucket_width; /* HIST: 0 = log2, else linear width in `unit` */
};
/* The schema of o; *n = total, at most cap written; *gen = the schema generation. CP. */
MTL_API_CP int mtl_stat_list(struct mtl_object o, struct mtl_stat_desc* d, size_t desc_size,
                             uint32_t cap, uint32_t* n, uint64_t* gen);
/* "name" or "name{label}" to its slot. CP. */
MTL_API_CP int mtl_stat_find(struct mtl_object o, const char* key, uint32_t* slot);
/* count values from slot first, one snapshot, if the schema is still at gen; the count
   written, or -MTL_ESTALE after a schema change. DP. */
MTL_API_DP int mtl_stat_read(struct mtl_object o, uint64_t gen, uint32_t first,
                             uint32_t count, int64_t* values,
                             int64_t* MTL_NULLABLE snapshot_tai_ns);
/* One value by name. CP. */
MTL_API_CP int mtl_stat_get(struct mtl_object o, const char* key, int64_t* value);

/* Values of enumerated keys. */
enum mtl_backend { /* "caps.backend" */
  MTL_BACKEND_DPDK_PMD = 1,
  MTL_BACKEND_AF_XDP = 2,
  MTL_BACKEND_KERNEL_SOCKET = 3,
  MTL_BACKEND_NULL = 4,
};
enum mtl_simd { /* "instance.simd_level"; not mtl_simd_level, the legacy tag */
  MTL_SIMD_NONE = 1,
  MTL_SIMD_AVX2 = 2,
  MTL_SIMD_AVX512 = 3,
  MTL_SIMD_AVX512_VBMI2 = 4,
};

/* ---- Full result records ---------------------------------------------------------------- */

/* TX: pass sizeof(struct mtl_tx_result_full) to mtl_tx_reap()/mtl_queue_reap(). */
#define MTL_TXF_ENQUEUED_VALID 0x1u
#define MTL_TXF_OBSERVED_LEG0 0x2u /* NIC timestamps per leg */
#define MTL_TXF_OBSERVED_LEG1 0x4u
#define MTL_TXF_OBSERVED_LAST 0x8u
struct mtl_tx_result_full {
  struct mtl_tx_result r;
  uint32_t detail_flags; /* MTL_TXF_* */
  uint32_t path;         /* enum mtl_path */
  int64_t submitted_tai_ns;
  int64_t deadline_tai_ns;
  int64_t scheduled_tai_ns; /* the first packet's planned launch */
  int64_t enqueued_tai_ns;  /* software: the burst of packet 0 */
  int64_t observed_first_tai_ns[MTL_MAX_LEGS];
  int64_t observed_last_tai_ns;
  int64_t pickup_slack_ns; /* deadline - pickup */
  int32_t snap_error_ns;
  int32_t max_packet_lateness_ns;
  uint32_t slots_skipped_before;
  uint32_t samples_padded;   /* audio */
  uint32_t samples_dropped;  /* audio */
  uint32_t samples_inserted; /* audio */
  uint32_t pkts_sent[MTL_MAX_LEGS];
  uint32_t leg_reason[MTL_MAX_LEGS]; /* mtl_reasons.h */
  uint32_t pkts_dma;
  uint32_t pkts_copied_partial;
};
enum mtl_path {
  MTL_PATH_DIRECT = 1,
  MTL_PATH_CONVERT = 2,
  MTL_PATH_COPY = 3,
  MTL_PATH_COPY_CONVERT = 4,
  MTL_PATH_DIRECT_DMA = 5, /* RX placed by a DMA engine */
};

/* RX: detail of a unit the caller holds (lease from mtl_rx_dequeue). */
#define MTL_RXF_ARRIVAL_LEG0 0x1u
#define MTL_RXF_ARRIVAL_LEG1 0x2u
#define MTL_RXF_HW_ARRIVAL 0x4u
#define MTL_RXF_PRESENTATION 0x8u
#define MTL_RXF_LATE_FOR_PRESENTATION 0x10u
struct mtl_rx_detail {
  uint32_t flags; /* MTL_RXF_* */
  uint32_t path;  /* enum mtl_path */
  int64_t arrival_first_tai_ns[MTL_MAX_LEGS];
  int64_t arrival_last_tai_ns[MTL_MAX_LEGS];
  int64_t presentation_tai_ns; /* media + rx.link_offset_ns */
  int64_t delivered_tai_ns;
  int32_t media_phase_ticks;
  uint32_t units_missing_before; /* stream gap by index difference */
  uint32_t pkts_expected;
  uint32_t pkts_received[MTL_MAX_LEGS];
  uint32_t pkts_recovered; /* filled by the other leg */
  uint32_t missing_ranges; /* count for mtl_rx_get_missing() */
  uint32_t format_changed; /* detected-property mask */
  uint32_t pkts_dma;
  uint32_t marker_seen;    /* 1 = the unit's last packet carried the marker */
  uint64_t bytes_received;
  uint32_t seq_discont[MTL_MAX_LEGS]; /* sequence discontinuities per leg */
};
MTL_API_DP int mtl_rx_get_detail(mtl_session_h s, mtl_lease_h lease,
                                 struct mtl_rx_detail* d, size_t size);
struct mtl_rx_missing_range {
  uint32_t first_pkt;
  uint32_t pkt_count;
};
/* The missing packet ranges of a held unit; count written >= 0. DPC. */
MTL_API_DPC int mtl_rx_get_missing(mtl_session_h s, mtl_lease_h lease,
                                   struct mtl_rx_missing_range* r, uint32_t max);

/* ST 2110-21 timing results of a held RX unit, per leg (rx.timing_parser on). Audio
   sessions report DPVR, IPT and TSDF over the unit instead of the video measures. */
enum mtl_compliance {
  MTL_COMPLIANT_NARROW = 1,
  MTL_COMPLIANT_WIDE = 2,
  MTL_NOT_COMPLIANT = 3,
};
struct mtl_rx_timing_result {
  uint32_t compliance; /* enum mtl_compliance */
  uint32_t failed_cause; /* mtl_reasons.h, 0 if compliant */
  uint32_t pkts;
  int32_t cinst_min;
  int32_t cinst_max;
  int32_t cinst_avg_x100;
  int32_t vrx_min;
  int32_t vrx_max;
  int32_t vrx_avg_x100;
  int32_t rtp_offset_ticks;
  int64_t fpt_ns; /* first packet time from the frame start */
  int64_t latency_ns;
  int64_t ipt_max_ns;    /* inter-packet time (audio and video) */
  int64_t dpvr_max_ns;   /* audio: delay of packets versus RTP */
  int64_t tsdf_ns;       /* audio: timestamped delay factor */
  int32_t rtp_delta_ticks; /* versus the previous unit */
  uint32_t reserved;
};
MTL_API_DP int mtl_rx_get_timing(mtl_session_h s, mtl_lease_h lease, uint32_t leg,
                                 struct mtl_rx_timing_result* t, size_t size);

/* ---- Health and shutdown (deployment.md) ----------------------------------------------------------- */

/* Liveness: the process cannot recover by itself; restarting it may help. */
#define MTL_HEALTH_SCHED_STALLED 0x1u  /* a scheduler loop is older than instance.stall_ns */
#define MTL_HEALTH_WORKER_STALLED 0x2u /* the admin worker missed its heartbeat outside a
                                          bounded device step (a port reset is progress) */
#define MTL_HEALTH_SESSION_LOST 0x4u   /* a started session lost every leg to removed ports,
                                          or every port is gone */
#define MTL_HEALTH_DEVICE_FAULT 0x8u   /* a queue was quarantined; its memory is never freed */
#define MTL_HEALTH_LIVENESS 0xffu
/* Readiness: the instance cannot carry media now; may clear by itself. */
#define MTL_HEALTH_STARTING 0x100u      /* phase below MTL_PHASE_READY */
#define MTL_HEALTH_NO_LINK 0x200u       /* no port has a link */
#define MTL_HEALTH_TIME_UNLOCKED 0x400u /* time state ACQUIRING or LOST (FREERUN and HOLDOVER
                                           are ready: IPMX runs without PTP) */
#define MTL_HEALTH_MANAGER_LOST 0x800u  /* MtlManager is gone (AF_XDP stops receiving) */
#define MTL_HEALTH_SHUTTING_DOWN 0x1000u /* close or shutdown has begun */
#define MTL_HEALTH_READINESS 0xffffu    /* the liveness bits and these */
/* Information only, in no mask: a port's link is down or removed, or a started session is
   in ERROR or has an enabled leg not resolved or joined. The counts and detail say which. */
#define MTL_HEALTH_DEGRADED 0x10000u
enum mtl_phase { /* how far the instance got */
  MTL_PHASE_LINKS = 1, /* devices up, waiting for links */
  MTL_PHASE_TIME = 2,  /* links up, time base acquiring */
  MTL_PHASE_READY = 3,
  MTL_PHASE_SHUTDOWN = 4,
};
/* Output; the size argument versions it. */
struct mtl_health {
  uint32_t flags;  /* MTL_HEALTH_* */
  uint32_t phase;  /* enum mtl_phase */
  uint32_t reason; /* of the first bit set, mtl_reasons.h */
  uint32_t time_state; /* enum mtl_time_state */
  uint32_t ports;
  uint32_t ports_up;
  uint32_t scheds;
  uint32_t scheds_stalled;
  uint32_t sessions_started;
  uint32_t sessions_error;
  int64_t oldest_loop_age_ns; /* the slowest scheduler's last loop */
  int64_t time_error_ns;      /* estimated error of the time base; INT64_MAX = unknown */
  char detail[96];            /* "port 1 link down; time ACQUIRING"; "" if it changed while
                                 being copied */
};
/* The flags (>= 0); h may be NULL. Liveness never depends on packets, links or PTP lock,
   so a grandmaster outage or a switch reboot never restarts a pod. Lock-free from any
   thread at any rate, even when the control plane is wedged: a consistent snapshot,
   never torn. During close: SHUTTING_DOWN (SCHED_STALLED masked); after it:
   -MTL_ESHUTDOWN. DP. */
MTL_API_DP int mtl_instance_get_health(mtl_instance_h mt, struct mtl_health* MTL_NULLABLE h,
                                       size_t size);

#define MTL_SHUTDOWN_ALL_REFERENCES 0x1u /* a shared instance: for every component; their
                                            handles then get -MTL_ESHUTDOWN, close 0 */
#define MTL_SHUTDOWN_DRAIN 0x2u /* TX: send every queued unit, until the deadline minus the
                                   time the later steps need */
/* Output; the size argument versions it. The outcome is the return value. */
struct mtl_shutdown_report {
  uint32_t references_left; /* > 0: only this reference was dropped */
  uint32_t reason;          /* of the first step that did not finish */
  int64_t elapsed_ns;
  int64_t quiesced_ns;        /* from the call until the last device stopped */
  uint32_t sessions_closed;   /* still open at the call; closed by it */
  uint32_t sessions_retiring;
  uint32_t units_flushed;
  uint32_t results_discarded; /* results no reader or dispatch thread took */
  uint32_t groups_left;       /* IGMP/MLD leaves sent */
  uint32_t leases_out;
  uint32_t regions_referenced;
  uint32_t ports_unquiesced;  /* bit per port */
  uint32_t threads_unjoined;  /* dispatch, log or codec threads still in foreign code */
  uint32_t reserved0;
  uint64_t bytes_kept;        /* library memory under leases still out */
  char summary[128]; /* one line for a termination message */
};
/* mtl_instance_close() with flags and a report: the same steps and return values. Without
   MTL_SHUTDOWN_ALL_REFERENCES a reference that is not the last only drops itself. Any
   thread but a library thread (-MTL_EDEADLK there, mt not consumed); consumes mt.
   mtl_instance_abort() during the call (a second SIGTERM) skips to the hard stop. r (may
   be NULL) is filled whatever the call returns. CP. */
MTL_API_CP int mtl_instance_shutdown(mtl_instance_h mt, uint64_t flags, int64_t timeout_ns,
                                     struct mtl_shutdown_report* MTL_NULLABLE r, size_t size);

/* ---- Enumeration and ports ----------------------------------------------------------- */

/* Every live session, closing ones included; *n = total. CP. */
MTL_API_CP int mtl_instance_list_sessions(mtl_instance_h mt, mtl_session_h* s,
                                          uint32_t cap, uint32_t* n);
/* A port by name, BDF or IP; its caps, status and counters are "caps.*", "port.*". CP. */
MTL_API_CP int mtl_port_find(mtl_instance_h mt, const char* name, uint32_t* port);
/* The port as granted: address, prefix and gateway in use (a DHCP lease included). CP. */
MTL_API_CP int mtl_port_get_spec(mtl_instance_h mt, uint32_t port, struct mtl_port_spec* out,
                              size_t size);
/* "26.09.0 (git ..., gcc ...)". AS. */
MTL_API_AS const char* mtl_version_string(void);

/* ---- Logging ------------------------------------------------------------------------- */

/* Library log lines go to fn instead of stderr, process-wide; fn runs on one library
   thread, never on a tasklet (lines are queued, and dropped with a count when fn is too
   slow). fn may not call mtl_log_set_sink or an instance's close or shutdown
   (-MTL_EDEADLK). fn NULL restores the default
   and waits for a running fn, so `user` may be freed after it returns. Each instance's
   level is its option log.level. CP. */
typedef void (*mtl_log_fn)(void* user, uint32_t level, const char* line);
MTL_API_CP int mtl_log_set_sink(mtl_log_fn fn, void* user, const char* MTL_NULLABLE prefix);

/* ---- Packet capture -------------------------------------------------------------------- */

struct mtl_capture_params {
  uint32_t struct_size;
  uint32_t max_pkts; /* 0 = until stopped */
  const char* path;  /* pcapng file */
  uint64_t legs;     /* bit per leg; 0 = all */
  uint64_t reserved[4];
};
/* Copies a session's packets to a file from a library worker; MTL_EVENT_CAPTURE_DONE
   when it ends. CP. */
MTL_API_CP int mtl_session_capture(mtl_session_h s, const struct mtl_capture_params* p);
MTL_API_CP int mtl_session_capture_stop(mtl_session_h s);

MTL_SIZE_CHECK(mtl_stat_desc, 120);
MTL_SIZE_CHECK(mtl_health, 152);
MTL_SIZE_CHECK(mtl_shutdown_report, 200);
MTL_SIZE_CHECK(mtl_tx_result_full, 216);
MTL_SIZE_CHECK(mtl_rx_detail, 112);
MTL_SIZE_CHECK(mtl_rx_missing_range, 8);
MTL_SIZE_CHECK(mtl_rx_timing_result, 88);
MTL_SIZE_CHECK(mtl_capture_params, 56);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_OBSERVE_H */
