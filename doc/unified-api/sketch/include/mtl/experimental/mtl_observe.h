/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_observe.h - stats, diagnostics, logging and capture for the unified MTL API,
 * revision 0.2. A media loop never needs this header; exporters, consoles, tests and the
 * entry points of a containerised service (health probes, bounded shutdown) do.
 *
 * Every number MTL can report about an object (session, port, scheduler, instance) is a
 * named int64_t value in one registry: "tx.units_dropped{reason=too_late}",
 * "rx.pkts_lost{leg=1}", "port.rx_missed", "sched.busy_pct_x100", "info.trs_ps". Names are
 * the stable contract (the key catalogue is in contract.md); labels follow Prometheus
 * syntax. List the schema once, then read values in bulk: mtl_stat_read() is DP and never
 * takes a lock a tasklet takes.
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
  MTL_STAT_HIST = 4,  /* 4 + MTL_HIST_BUCKETS values: count, sum, min, max, buckets */
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
  MTL_SU_ID = 12, /* an identifier: an EUI-64, or a MAC in the low 48 bits */
};
#define MTL_HIST_BUCKETS 16
#define MTL_STAT_ESTIMATE 0x1u /* inexact on this backend, e.g. packet loss */
#define MTL_STAT_CACHED 0x2u   /* polled off the tasklet; "<scope>.sampled_tai_ns" says when */
/* Output value type of the schema. */
struct mtl_stat_desc {
  char name[48];  /* "tx.units_dropped" */
  char label[48]; /* "reason=too_late", "leg=1", "essence=fastmeta,dir=tx", "" */
  uint32_t first; /* index of the first value in mtl_stat_read() */
  uint16_t width; /* values: 1, or 4 + MTL_HIST_BUCKETS */
  uint16_t kind;  /* enum mtl_stat_kind */
  uint16_t unit;  /* enum mtl_stat_unit */
  uint16_t flags; /* MTL_STAT_* */
  uint32_t reserved;
  int64_t bucket_width; /* HIST: 0 = log2 buckets, the layout of every histogram */
};
/* The schema of o; *n = total, at most cap written (d may be NULL with cap 0); *gen = the
   schema generation. CP. (MS2) */
MTL_API_CP(2) int mtl_stat_list(struct mtl_object o, struct mtl_stat_desc* MTL_NULLABLE d,
                                size_t desc_size, uint32_t cap, uint32_t* n, uint64_t* gen);
/* "name" or "name{label}" to the index of its first value. CP. (MS2) */
MTL_API_CP(2) int mtl_stat_find(struct mtl_object o, const char* key, uint32_t* first);
/* count values from index first, one snapshot, if the schema is still at gen; the count
   written, or -MTL_ESTALE after a schema change. DP. (MS2) */
MTL_API_DP(2) int mtl_stat_read(struct mtl_object o, uint64_t gen, uint32_t first,
                                uint32_t count, int64_t* values,
                                int64_t* MTL_NULLABLE snapshot_tai_ns);
/* One value by name (a histogram: its count): list, find and read, again after a schema
   change. CP. */
static inline int mtl_stat_get(struct mtl_object o, const char* key, int64_t* value) {
  int ret = -MTL_ESTALE;
  for (int tries = 0; ret == -MTL_ESTALE && tries < 4; tries++) {
    uint32_t n = 0, first = 0;
    uint64_t gen = 0;
    ret = mtl_stat_list(o, NULL, sizeof(struct mtl_stat_desc), 0, &n, &gen);
    if (ret >= 0) ret = mtl_stat_find(o, key, &first);
    if (ret >= 0) ret = mtl_stat_read(o, gen, first, 1, value, NULL);
  }
  return ret < 0 ? ret : 0;
}

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
enum mtl_manager_state { /* "instance.manager", MTL_EVENT_MANAGER_LOST */
  MTL_MANAGER_NONE = 1,      /* not configured: normal, nothing logged */
  MTL_MANAGER_CONNECTED = 2,
  MTL_MANAGER_LOST = 3,      /* sessions continue */
  MTL_MANAGER_RECONNECTING = 4, /* in the background, with back-off */
};

/* ---- Full result records ---------------------------------------------------------------- */

/* TX: the full result record, read with mtl_tx_reap_full(). */
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
  uint32_t indices_skipped_before; /* media indices left empty on the wire before it */
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
/* mtl_tx_reap() with full records. WT. */
static inline int mtl_tx_reap_full(mtl_session_h s, struct mtl_tx_result_full* r,
                                   uint32_t max, int64_t timeout_ns) {
  return mtl_reap(MTL_OBJ_OF_SESSION(s), r, sizeof(*r), max, timeout_ns);
}

/* ST 2110-21 timing results of a held RX unit, per leg (rx.timing_parser on), in
   mtl_rx_detail.timing[], with the names and formulas of RP 2110-25 §4. The VRX read
   schedule starts at TVD = N x TFRAME + TROFFSET, TROFFSET the config's troffset_us (the
   sender's TROFF), else TRODEFAULT (RP 2110-25 §4.9.2). Audio sessions report DPVR, IPT
   and TSDF over the unit instead of the video measures. */
enum mtl_compliance {
  MTL_COMPLIANT_NARROW = 1, /* type N: gapped read schedule */
  MTL_COMPLIANT_WIDE = 2,   /* type W: linear */
  MTL_NOT_COMPLIANT = 3,
  MTL_COMPLIANT_NARROW_LINEAR = 4, /* type NL: linear, with the N limits */
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
  int32_t rtp_offset_ticks; /* RTPOFFSET (§4.8.4) */
  int64_t fpt_ns; /* FPT = TPA0 - TCF, TCF = N x TFRAME with N = round(TPA0 / TFRAME), so
                     within +-1/2 frame (§4.4, §4.8.3); EBU LIST and the legacy parser take
                     the floor (st_rx_timing_parser.c:21), which a binding converts */
  int64_t latency_ns;    /* VL = TPA0 - RTP time (§4.8.5) */
  int64_t ipt_max_ns;    /* inter-packet time (audio and video) */
  int64_t dpvr_max_ns;   /* audio: delay of packets versus RTP */
  int64_t tsdf_ns;       /* audio: timestamped delay factor */
  int32_t rtp_delta_ticks; /* versus the previous unit */
  uint32_t vrx_underflow;  /* reads that found the buffer empty (VRX_UNDERFLOW, §4.9.2) */
  uint32_t vrx_missing;    /* packets absent at their read time (VRX_PACKET_MISSING) */
  uint32_t reserved;
  int64_t margin_ns;       /* TROFFSET - FPT (§4.8.6) */
  int64_t gap_ns;          /* this unit's first packet minus the previous unit's last;
                              negative = reordering (§4.8.7) */
};

/* RX: detail of a unit the caller holds (lease from mtl_rx_dequeue). */
#define MTL_RXF_ARRIVAL_LEG0 0x1u
#define MTL_RXF_ARRIVAL_LEG1 0x2u
#define MTL_RXF_HW_ARRIVAL 0x4u
#define MTL_RXF_PRESENTATION 0x8u
#define MTL_RXF_LATE_FOR_PRESENTATION 0x10u
#define MTL_RXF_TIMING_LEG0 0x20u /* timing[0] is valid (rx.timing_parser) */
#define MTL_RXF_TIMING_LEG1 0x40u
#define MTL_RXF_INCOMPLETE_BY_DUE 0x80u /* force-completed at its due time, not by a stop */
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
  uint32_t missing_ranges; /* runs of lost packets */
  uint32_t format_seq;     /* video: the unit's format, 1 for the first published after a
                              start, + 1 for each later one (MTL_UNITF_FORMAT_CHANGED on
                              its first unit) */
  uint32_t pkts_dma;
  uint32_t marker_seen;    /* 1 = the unit's last packet carried the marker */
  uint64_t bytes_received;
  uint32_t seq_discont[MTL_MAX_LEGS]; /* sequence discontinuities per leg */
  struct mtl_rx_timing_result timing[MTL_MAX_LEGS]; /* with MTL_RXF_TIMING_LEG* */
  struct mtl_raster raster; /* video: the unit's raster (detected, or the configured one) */
  uint32_t packing;         /* video: enum mtl_packing of the stream */
  uint32_t fps_approx;      /* 1: raster.fps matched no named rate (rx.detected.fps_approx) */
  uint32_t anc_skipped;   /* ANC: corrupt packets skipped, every cause together (the
                             anc.pkts_skipped{cause} stats key counts each cause) */
  uint32_t anc_truncated; /* ANC: packets beyond the table or the user data words */
};
/* The detail of a unit the caller holds. DP. (MS2) */
MTL_API_DP(2) int mtl_rx_get_detail(mtl_session_h s, mtl_lease_h lease,
                                    struct mtl_rx_detail* d, size_t size);

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
                                           are ready: media may run without PTP) */
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
   -MTL_ESHUTDOWN. DP. (MS3) */
MTL_API_DP(3) int mtl_instance_get_health(mtl_instance_h mt, struct mtl_health* MTL_NULLABLE h,
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
  uint32_t results_discarded; /* results no reader took */
  uint32_t groups_left;       /* IGMP/MLD leaves sent */
  uint32_t leases_out;
  uint32_t regions_referenced;
  uint32_t ports_unquiesced;  /* bit per port */
  uint32_t threads_unjoined;  /* log or codec threads still in foreign code */
  uint32_t reserved0;
  uint64_t bytes_kept;        /* library memory under leases still out */
  char summary[128]; /* one line for a termination message */
};
/* mtl_instance_close() with flags and a report: the same steps and return values. Without
   MTL_SHUTDOWN_ALL_REFERENCES a reference that is not the last only drops itself. Any
   thread but a library thread (-MTL_EDEADLK there, without effect). mtl_instance_abort()
   during the call (a second SIGTERM) skips to the hard stop. r (may be NULL) is filled
   whatever the call returns. CP. (MS3) */
MTL_API_CP(3) int mtl_instance_shutdown(mtl_instance_h mt, uint64_t flags, int64_t timeout_ns,
                                        struct mtl_shutdown_report* MTL_NULLABLE r, size_t size);

/* ---- Enumeration and ports ----------------------------------------------------------- */

/* Every live session, closing ones included; *n = total. CP. (MS2) */
MTL_API_CP(2) int mtl_instance_list_sessions(mtl_instance_h mt, mtl_session_h* s,
                                             uint32_t cap, uint32_t* n);
/* The port as granted: address, prefix and gateway in use (a DHCP lease included).
   -MTL_EINVAL (field "port") beyond the last port. CP. (MS2) */
MTL_API_CP(2) int mtl_port_get_spec(mtl_instance_h mt, uint32_t port, struct mtl_port_spec* out,
                                 size_t size);
/* A port by its name (PCI BDF, "kernel:<ifname>", ...) or its IPv4 address in dotted
   form; its caps, status and counters are "caps.*", "port.*". -MTL_EINVAL if none
   matches. CP. */
static inline int mtl_port_find(mtl_instance_h mt, const char* name, uint32_t* port) {
  uint8_t ip[4] = {0, 0, 0, 0};
  int is_ip = 1;
  uint32_t part = 0, value = 0, digits = 0;
  for (const char* c = name; is_ip; c++) { /* "a.b.c.d" */
    if (*c >= '0' && *c <= '9' && digits < 3) {
      value = value * 10 + (uint32_t)(*c - '0');
      digits++;
    } else if (digits && value <= 255 && (part < 3 ? *c == '.' : *c == 0)) {
      ip[part++] = (uint8_t)value;
      value = digits = 0;
      if (*c == 0) break;
    } else {
      is_ip = 0;
    }
  }
  for (uint32_t p = 0;; p++) {
    struct mtl_port_spec spec;
    int ret = mtl_port_get_spec(mt, p, &spec, sizeof(spec));
    if (ret < 0) return ret;
    size_t i = 0;
    while (i < sizeof(spec.name) && spec.name[i] && spec.name[i] == name[i]) i++;
    int match = i < sizeof(spec.name) && spec.name[i] == name[i];
    if (is_ip && spec.ip_family == 0)
      match = match || (spec.sip[0] == ip[0] && spec.sip[1] == ip[1] &&
                        spec.sip[2] == ip[2] && spec.sip[3] == ip[3]);
    if (match) {
      *port = p;
      return 0;
    }
  }
}

/* ---- Logging ------------------------------------------------------------------------- */

typedef struct mtl_log_sink_h {
  uint64_t id;
} mtl_log_sink_h;
/* One log line, valid during the sink call only (r_size versions it). */
struct mtl_log_record {
  uint32_t severity; /* enum mtl_severity (mtl.h) */
  uint32_t dropped;  /* lines this sink would have taken that were lost since its previous
                        record (exact for an instance's rings; process-wide lines lost are
                        counted for every sink whose level takes them) */
  struct mtl_object origin; /* the session, port, scheduler or instance the line is about;
                               kind 0 = the process (EAL, DPDK, lines before any open). An
                               instance, port or scheduler names its instance by the
                               instance's identity (stat instance.identity), the same for
                               every reference of a shared instance */
  char origin_name[MTL_NAME_MAX]; /* the session or port name, "sched<N>", the instance's
                                     first port name; "" for the process; readable after
                                     the origin retired */
  int64_t realtime_ns; /* CLOCK_REALTIME when the line was produced */
  const char* text;    /* NUL-terminated, without prefix or newline; at most 511 bytes,
                          a longer line ends in "..." */
  uint32_t text_len;
  uint32_t reserved;
};
typedef void (*mtl_log_fn)(void* priv, const struct mtl_log_record* r, size_t r_size);
struct mtl_log_sink_params {
  uint32_t struct_size;
  uint32_t min_severity;      /* enum mtl_severity; 0 = MTL_SEV_INFO */
  mtl_log_fn MTL_NULLABLE fn; /* NULL = the library's stderr writer, legacy format (D-110) */
  void* MTL_NULLABLE priv;
  mtl_instance_h instance; /* null = every instance and the process; else the lines of that
                              instance (any reference to it), its ports, schedulers and
                              sessions */
  const char* MTL_NULLABLE prefix; /* fn NULL: prepended to every line; copied */
  uint64_t reserved[5];
};
/* Adds a log sink, with its own level and filter; several may exist, also before any open
   (EAL lines of an EAL the library starts). With no sink, a control-plane line is written
   to stderr by its caller, in the legacy format, at the process threshold
   (mtl_instance_params.log_level, or a bridged legacy instance's level); tasklet lines go
   through the log thread. Once a sink exists, lines reach sinks only (fn NULL keeps
   stderr) and the production threshold is the lowest min_severity among the sinks whose
   filter matches; on a bridged instance the legacy level follows it, and when the last
   sink goes both return to the stderr threshold. Every fn runs on the one process-wide log thread, one call at a time
   across all sinks, never on a tasklet or an application thread; the lines of one
   producing thread arrive in order. fn may call any function but instance open, close and
   shutdown (-MTL_EDEADLK, LIBRARY_THREAD); lines its calls produce are dropped and counted,
   never delivered. When the rings are full, lines are dropped and counted (r->dropped).
   The log thread runs while a sink or an instance exists; an instance's close hands its
   queued lines to the sinks before it returns. In a forked child the sinks are never
   called and add is -MTL_EBADF (FORKED). CP. (MS2) */
MTL_API_CP(2) int mtl_log_add_sink(const struct mtl_log_sink_params* p, mtl_log_sink_h* out);
/* Removes a sink. 0: fn is not running and is never called again, so priv may be freed;
   MTL_RETIRING: fn was still running at the deadline, call again to poll. From inside fn
   it never waits: 0 for another sink, MTL_RETIRING for its own (the running call is its
   last). 0 for a null handle; in a forked child it drops the handle. CP. */
static inline int mtl_log_remove_sink(mtl_log_sink_h h, int64_t timeout_ns) {
  return mtl_close(mtl_obj(MTL_OBJ_LOG_SINK, 0, h.id), timeout_ns);
}

/* ---- Packet capture -------------------------------------------------------------------- */

struct mtl_capture_params {
  uint32_t struct_size;
  uint32_t max_pkts; /* 0 = until stopped */
  const char* path;  /* pcapng file */
  uint64_t legs;     /* bit per leg; 0 = all */
  uint64_t reserved[4];
};
/* Copies a session's packets to a file from a library worker; MTL_EVENT_CAPTURE_DONE
   when it ends. p NULL stops a running capture. CP. (MS4) */
MTL_API_CP(4) int mtl_session_capture(mtl_session_h s,
                                      const struct mtl_capture_params* MTL_NULLABLE p);

MTL_SIZE_CHECK(mtl_stat_desc, 120);
MTL_SIZE_CHECK(mtl_health, 152);
MTL_SIZE_CHECK(mtl_shutdown_report, 200);
MTL_SIZE_CHECK(mtl_tx_result_full, 216);
MTL_SIZE_CHECK(mtl_rx_detail, 384);
MTL_SIZE_CHECK(mtl_log_sink_h, 8);
MTL_SIZE_CHECK(mtl_log_record, 112);
MTL_SIZE_CHECK(mtl_log_sink_params, 80);
MTL_SIZE_CHECK(mtl_rx_timing_result, 112);
MTL_SIZE_CHECK(mtl_capture_params, 56);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_OBSERVE_H */
