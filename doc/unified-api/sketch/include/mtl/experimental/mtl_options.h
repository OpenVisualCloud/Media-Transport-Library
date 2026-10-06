/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */
/* Hand-formatted: clang-format stays off so the milestone tags keep their place
   (check.sh lint 5). */
/* clang-format off */
/*
 * mtl_options.h - option keys of the unified MTL API, revision 0.2. The library's option
 * table (lib/src/unified/mtl_options.def) is checked against this header by a U test (task
 * H1b); the keys and their rules are described in contract.md §12.
 *
 * An option is a tuning knob that most applications never touch. It is absent by default,
 * and absent means the documented default; a present option is literal (0 means 0). Pass
 * options at open or create in an array (mtl_instance_params.options,
 * mtl_session_config.options), or change them later with mtl_set_options() where the key
 * allows it. Programs use the MTL_OPT_* constants. Every key also has a name
 * ("rx.skew_budget_ns"), enumerable with mtl_option_list(), so GStreamer properties, FFmpeg
 * AVOptions and bindings can expose every knob without code per knob.
 *
 * Each line: key  "name"  value (default when absent)  [when it may change: C = at create
 * only, S = also when STOPPED, R = also while running, at the next unit boundary (Phase
 * 7: also at the boundary of the mtl_session_update() whose config carries it; until then
 * a changed R key in an update is -MTL_ENOTSUP)], then P for a provisional key. Keys
 * marked "per port", "per leg" or "per scheduler" take a scope (0 = all, else MTL_INDEX(i)).
 * Option names and stats names (mtl_observe.h) are separate registries. A session key set
 * on the instance (mtl_instance_params.options, or mtl_set_option on the instance) is the
 * default for that instance's sessions, as legacy instance-wide settings were (so there
 * is no instance twin of a session key: session.tx_queue and session.migrate on the
 * instance replace the legacy shared TX queue flag and the video migrate flags).
 *
 * Stability (contract.md §12.6). A key is stable unless its line ends with P. A stable key
 * freezes with the API at MTL_1.0: later it may become an accepted no-op
 * (MTL_OPTION_DEPRECATED), never an error, and its name, number and meaning never change.
 * A provisional key (MTL_OPTION_PROVISIONAL) keeps its number and name, never reused, but
 * its range, default and meaning may change at any release, and it may be withdrawn: then
 * it is -MTL_ENOTSUP (OPTION_WITHDRAWN) wherever it is passed, never ignored, and its
 * MTL_OPT_* constant stays. Promotion clears P without a rename; no name prefix marks a
 * tier. A key that leaves MTL_LATER, or is added later, starts provisional. Keys of
 * Phase 7 are declared under MTL_LATER; every other key is known from the start and is
 * -MTL_ENOTSUP (NOT_IMPLEMENTED) until its milestone. A stream parameter the SDP carries is
 * a typed field, not a key (contract.md §12.6).
 */

#ifndef MTL_EXPERIMENTAL_MTL_OPTIONS_H
#define MTL_EXPERIMENTAL_MTL_OPTIONS_H

#include "mtl.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum mtl_option_key {
  /* Retired before MS1, never reused: 106 (now mtl_session_config.tsmode), 112 and 214
     (troffset_us of the video and rtp configs), 304 (the field
     mtl_session_config.max_udp_payload; the instance default is 2028), 700
     (mtl_anc_config.timing_model). */
  /* 100-199 TX timing (timing.md) */
  MTL_OPT_LATE_POLICY = 100, /* "tx.late_policy" MTL_LATE_* (AUTO: DEFER; INDEX, TAI: DROP)
                                S */
#if defined(MTL_LATER)
  MTL_OPT_LATE_TOLERANCE_NS = 101, /* "tx.late_tolerance_ns" the bound of SEND_LATE
                                      (min(TROFFSET, blanking)) S (Phase 7) */
#endif
  MTL_OPT_UNDERRUN_POLICY = 102,   /* "tx.underrun_policy" MTL_UNDERRUN_* (by essence) S */
  MTL_OPT_SNAP_MODE = 103,         /* "tx.snap_mode" MTL_SNAP_* (NEAREST) S */
  MTL_OPT_SNAP_TOLERANCE_NS = 104, /* "tx.snap_tolerance_ns" (by snap mode) S */
#if defined(MTL_LATER)
  MTL_OPT_OFF_GRID_POLICY = 105,   /* "tx.off_grid_policy" MTL_OFF_GRID_* (RELOCK) S (later) */
#endif
  MTL_OPT_HORIZON_NS = 107,        /* "tx.horizon_ns" (1 s from the start instant) S */
  MTL_OPT_INDEX_OFFSET = 108,      /* "tx.index_offset" whole-unit lip-sync trim (0) R */
  MTL_OPT_LINK_OFFSET_BUDGET_NS = 109, /* "tx.link_offset_budget_ns" (none) S */
  MTL_OPT_ROWS_LATE = 110,         /* "tx.rows_late" MTL_ROWS_* (TRUNCATE) S */
  MTL_OPT_EPOCH_TICK = 111,        /* "session.epoch_tick" bool: MTL_EVENT_EPOCH_TICK (0) S */
#if defined(MTL_LATER)
  MTL_OPT_PRECEDE = 113,           /* "tx.precede" str: a session name of the same start;
                                      unit k shares its queue, takes its unit k's RTP and
                                      media time, and goes out just before it (IPMX
                                      InfoFrames, port + 3) C (Phase 7) */
#endif
  MTL_OPT_RTP_TRIM_NS = 114,       /* "tx.rtp_trim_ns" AUTO and INDEX: RTP = floor((M + this)
                                      x rate) with the launch unchanged; the legacy
                                      rtp_timestamp_delta_us x 1000; video, cvideo and ANC:
                                      |value| < TFRAME (ST 2110-10 §7.6.3), else
                                      OPTION_RANGE (0) S P */
  /* 200-299 RX timing and delivery */
  MTL_OPT_RX_INCOMPLETE = 200,     /* "rx.incomplete" MTL_RX_DELIVER/DISCARD (DELIVER) S */
#if defined(MTL_LATER)
  MTL_OPT_RX_MEDIACLK = 201,       /* "rx.mediaclk" MTL_MEDIACLK_* (by profile) R (Phase 7) */
#endif
  MTL_OPT_RX_RTP_OFFSET = 202,     /* "rx.rtp_offset" SDP mediaclk:direct= ticks (0) R */
  MTL_OPT_RX_LINK_OFFSET_NS = 203, /* "rx.link_offset_ns" presentation = media + this;
                                      MTL_LINK_OFFSET_AUTO: the measured minimum (none:
                                      no presentation time) R */
  MTL_OPT_RX_FLUSH_OFFSET_NS = 204, /* "rx.flush_offset_ns" (skew budget, or 1 ms) S */
  MTL_OPT_RX_SKEW_BUDGET_NS = 205, /* "rx.skew_budget_ns" ST 2022-7 (10 ms) S */
  MTL_OPT_RX_SIGNAL_TIMEOUT_NS = 206, /* "rx.signal_timeout_ns" (max(4 units, 20 ms)) S */
  MTL_OPT_RX_BURST = 207,          /* "rx.burst" packets (128) C P */
  MTL_OPT_RX_THREADS = 208,        /* "rx.threads" (auto: 2 above 40 Gb/s); today's engine
                                      rejects 2 with two legs or with rows units C P */
  MTL_OPT_RX_TIMING_PARSER = 209,  /* "rx.timing_parser" bool: ST 2110-21 checks (0) S */
  MTL_OPT_RX_CONVERT_PER_PACKET = 211, /* "rx.convert_per_packet" bool: convert each packet
                                      on the RX tasklet as it lands (library code only), the
                                      one exception to conversion off the tasklets (0) C P */
  MTL_OPT_RX_ROWS_STEP = 212,      /* "rx.rows_step" rows between row wake-ups (64) S */
#if defined(MTL_LATER)
  MTL_OPT_RX_JOIN_LEAD_NS = 213,   /* "rx.join_lead_ns" a scheduled update joins at
                                      max(call, t - lead) (at the call) R (Phase 7) */
#endif
  /* 300-399 queues, placement, waking */
  MTL_OPT_TX_QUEUE = 300,          /* "session.tx_queue" MTL_TXQ_* (by essence); each session
                                      takes its own queue at create, so components that share
                                      an instance leave the port's queue counts 0 C */
  MTL_OPT_NUMA = 301,              /* "session.numa" node (the first port's) C */
  MTL_OPT_SRC_PORT_MODE = 302,     /* "session.src_port_mode" MTL_SRC_PORT_* (FIXED) S */
  MTL_OPT_MIGRATE = 306,           /* "session.migrate" bool: the scheduler may move the session
                                      to balance load, as today (0); set on the instance,
                                      the default of its sessions C P */
  MTL_OPT_TX_HANG_DETECT_NS = 305, /* "session.tx_hang_detect_ns" (library default) C P */
  /* 1000-1009 NACK retransmission (video and cvideo; MTL's own, both ends MTL; the legacy
     "rtcp" options) */
  MTL_OPT_RTX = 1000,              /* "rtx.enable" bool (0) C P */
  MTL_OPT_RTX_BUFFER_PKTS = 1001,  /* "rtx.buffer_pkts" TX retransmit buffer (ring size) C P */
  MTL_OPT_RTX_NACK_INTERVAL_US = 1002, /* "rtx.nack_interval_us" RX S P */
  MTL_OPT_RTX_SEQ_BITMAP = 1003,   /* "rtx.seq_bitmap_bytes" RX window / 8 C P */
  MTL_OPT_RTX_SEQ_SKIP = 1004,     /* "rtx.seq_skip_window" RX S P */
#if defined(MTL_LATER)
  /* 1010-1019 RTCP sender reports and the Info Block (mtl_ipmx.h); NACKs share the port */
  MTL_OPT_RTCP_SR = 1010,          /* "rtcp.sr" bool: TX sender reports (0; the IPMX profile sets
                                      it) C (MS5) */
  MTL_OPT_RTCP_CNAME = 1011,       /* "rtcp.cname" str (the session name @ the port IP) C
                                      (MS5) */
  MTL_OPT_RTCP_DST_PORT = 1012,    /* "rtcp.dst_port" (the leg's udp_port + 1) S (MS5) */
  MTL_OPT_RTCP_RX = 1013,          /* "rtcp.rx" bool: receive sender reports (by profile) C
                                      (Phase 7) */
  /* 1020-1029 payload encryption (mtl_ipmx.h); none is secret, keys never are options
     (Phase 7) */
  MTL_OPT_CRYPTO_WORKERS = 1020,   /* "crypto.workers" library threads that cipher between
                                      submit and pick-up; 0 = in the caller (0) C */
  MTL_OPT_CRYPTO_SCHEME = 1021,    /* "crypto.scheme" MTL_CRYPTO_PEP_* (absent: none) R */
  MTL_OPT_CRYPTO_MODE = 1022,      /* "crypto.mode" MTL_CRYPTO_AES* (AES128_CTR) R */
  MTL_OPT_CRYPTO_EXT_ID_FULL = 1023, /* "crypto.ext_id_full" RFC 8285 ID 1-14, required R */
  MTL_OPT_CRYPTO_EXT_ID_SHORT = 1024, /* "crypto.ext_id_short" RFC 8285 ID 1-14, required R */
  MTL_OPT_CRYPTO_CLEAR_BYTES = 1025, /* "crypto.clear_bytes" (the payload header) R */
  MTL_OPT_CRYPTO_IV = 1026,        /* "crypto.iv" the 64-bit base IV, required R */
  MTL_OPT_CRYPTO_SUBSTREAM = 1027, /* "crypto.substream" 0..1023 (0) R */
  MTL_OPT_CRYPTO_IN_PLACE = 1028,  /* "crypto.in_place" bool: TX may overwrite attached
                                      memory with ciphertext (0) C */
  /* 1030-1039 profiles */
  MTL_OPT_PROFILE = 1030,          /* "session.profile" MTL_PROFILE_* (ST2110): the zero
                                      defaults and compliance labels of a profile C
                                      (Phase 7) */
#endif
  /* 400-499 capability requests */
  MTL_OPT_PACING = 400,            /* "caps.pacing" enum mtl_pacing (mtl.h): the class
                                      asked for (any class) C */
  MTL_OPT_PACING_REQ = 401,        /* "caps.pacing_req" MTL_REQ_PREFER or MTL_REQ_REQUIRE:
                                      REQUIRE fails create (PACING_UNAVAILABLE) instead of
                                      falling back; without caps.pacing on the session or as
                                      an instance default -MTL_EINVAL (PREFER) C */
  MTL_OPT_DMA = 402,               /* "caps.dma" MTL_REQ_* (library choice) C */
  MTL_OPT_HW_TIMESTAMPS = 403,     /* "caps.hw_timestamps" MTL_REQ_*: needs the instance's
                                      MTL_INSTANCE_HW_TIMESTAMP (library choice) C */
  MTL_OPT_TX_COPY = 404,           /* "caps.tx_copy" bool: force the copy path; with
                                      MTL_SESSION_REQUIRE_DIRECT -MTL_EINVAL (0) C */
  /* 500-599 video; the video.* keys also apply to compressed video */
  MTL_OPT_VIDEO_DISABLE_BULK = 500, /* "video.disable_bulk" bool C P */
  MTL_OPT_VIDEO_STATIC_PAD_P = 501, /* "video.static_pad_p" bool S P */
  MTL_OPT_VIDEO_START_VRX = 502,    /* "video.start_vrx" packets S P */
  MTL_OPT_VIDEO_PAD_INTERVAL = 503, /* "video.pad_interval" packets S P */
  MTL_OPT_VIDEO_CONVERT_DEVICE = 504, /* "video.convert_device" MTL_CODEC_DEVICE_*: the
                                         converter, on cvideo the codec (any) C */
#if defined(MTL_LATER)
  MTL_OPT_VIDEO_VTOTAL = 505,      /* "video.vtotal" total lines (ST 2110-21 value for its
                                      rasters, else the height): IPMX schedule, Info Block C
                                      (Phase 7) */
  MTL_OPT_VIDEO_HTOTAL = 506,      /* "video.htotal" total pixels per line (the width) C
                                      (Phase 7) */
#endif
  MTL_OPT_CVIDEO_THREADS = 511,     /* "cvideo.threads" (plugin default) C P */
  MTL_OPT_CVIDEO_QUALITY = 512,     /* "cvideo.quality" MTL_CVIDEO_QUALITY_* (plugin default) S P */
  MTL_OPT_CVIDEO_PACK = 514,        /* "cvideo.pack" MTL_CVIDEO_PACK_* (CODESTREAM) C P */
  MTL_OPT_CVIDEO_MAX_BITRATE = 515, /* "cvideo.max_bitrate_bps" VBR_MAX ceiling, CMAX from it
                                       (from codestream_bytes) C */
  /* 600-699 audio */
  MTL_OPT_AUDIO_ABSORB_SAMPLES = 600, /* "audio.absorb_samples" (S in TAI mode, else 0) S P */
  MTL_OPT_AUDIO_LAUNCH_OFFSET_NS = 601, /* "audio.launch_offset_ns" (the pacing class's early
                                           error + margin, at most ptime / 2) S P */
  MTL_OPT_AUDIO_BUILD_PACING = 602, /* "audio.build_pacing" bool (0) C P */
  MTL_OPT_AUDIO_FIFO_MS = 603,      /* "audio.fifo_ms" (10) C P */
  MTL_OPT_AUDIO_RL_ACCURACY_NS = 604, /* "audio.rl_accuracy_ns" rate-limit pacing accuracy S P */
  MTL_OPT_AUDIO_RL_OFFSET_NS = 605,   /* "audio.rl_offset_ns" rate-limit warm-up offset S P */
  /* 700-799 ANC and fast metadata */
  MTL_OPT_ANC_TARGET_DELAY_NS = 701, /* "anc.target_delay_ns" (the model's) S */
#if defined(MTL_LATER)
  MTL_OPT_ANC_WINDOW = 702,         /* "anc.window" MTL_ANC_WINDOW_* (AUTO) S (later) */
#endif
  MTL_OPT_ANC_TOTAL_LINES = 703,    /* "anc.total_lines" (from the raster) S */
  /* 704 and 705 are retired: one ANC packet per RTP packet is MTL_ANCF_NEW_RTP on each
     entry, and the packets per unit are the field anc.max_packets (mtl.h) */
  MTL_OPT_FASTMETA_TARGET_DELAY_NS = 710, /* "fastmeta.target_delay_ns" (timing.md) S */
  /* 800-899 packet units */
  MTL_OPT_PKT_HEADER_SLOT_BYTES = 800, /* "packet.header_slot_bytes" SPLIT (64) C */
  MTL_OPT_PKT_RX_MIN_PACKETS = 801,    /* "packet.rx_min_packets" (1) S */
  MTL_OPT_PKT_RX_MAX_WAIT_NS = 802,    /* "packet.rx_max_wait_ns" (one ptime, or 1 ms) S */
  /* 2000-2099 instance: scheduling and threads */
  MTL_OPT_SCHED_MAX = 2000,        /* "instance.sched_max" (auto, <= 18) C P */
  MTL_OPT_SCHED_QUOTA_MBS = 2001,  /* "instance.sched_quota_mbs" (12 x 1080p59.94) C P */
  MTL_OPT_SCHED_SLEEP_US = 2002,   /* "instance.sched_sleep_us" per scheduler; 0 = never R P */
  MTL_OPT_SCHED_TX_AUDIO_MAX = 2003, /* "instance.sched_tx_audio_max" sessions C P */
  MTL_OPT_TASKLETS_PER_SCHED = 2004, /* "instance.tasklets_per_sched" C P */
  MTL_OPT_RX_SEPARATE_VIDEO_LCORE = 2005, /* "instance.rx_separate_video_lcore" bool C P */
  MTL_OPT_TASKLET_TIME_MEASURE = 2008, /* "instance.tasklet_time_measure" bool R P */
  MTL_OPT_SYS_LCORE = 2009,        /* "instance.sys_lcore" MTL_SYS_*: the system tasklets
                                      (CNI, ARP, PTP) on a shared scheduler or their own
                                      (shared); never main_lcore C P */
  MTL_OPT_MAIN_LCORE = 2010,       /* "instance.main_lcore" the CPU of EAL's main lcore and of
                                      every thread that is not a scheduler, never given a
                                      scheduler (the first CPU of the mask) C P */
  MTL_OPT_ACROSS_NUMA_CORES = 2011, /* "instance.across_numa_cores" bool C P */
  MTL_OPT_NO_BIND_NUMA = 2012,     /* "instance.no_bind_numa" bool: today's NOT_BIND_NUMA C P */
  MTL_OPT_NO_BIND_PROCESS_NUMA = 2013, /* "instance.no_bind_process_numa" bool C P */
  MTL_OPT_SIMD_512 = 2014,         /* "instance.simd_512" bool C P */
  MTL_OPT_SCHED_RX_AUDIO_MAX = 2016, /* "instance.sched_rx_audio_max" sessions C P */
  MTL_OPT_IOVA_MODE = 2017,        /* "instance.iova" MTL_IOVA_* (auto) C P */
  MTL_OPT_MEMZONE_MAX = 2018,      /* "instance.memzone_max" C P */
  MTL_OPT_CNI = 2019,              /* "instance.cni" MTL_CNI_* (thread) C P */
  /* 2020-2039 instance: pods and process hygiene (deployment.md) */
  MTL_OPT_STALL_NS = 2020,         /* "instance.stall_ns" liveness stall limit; events at 1/8,
                                      1/4, 1/2 of it (1 s) R */
  MTL_OPT_CPU_ARBITRATION = 2021,  /* "instance.cpu_arbitration" MTL_CPUARB_* (auto: none in an
                                      exclusive cpuset, where an MtlManager socket mounted for
                                      AF_XDP grants AF_XDP queues only; else MtlManager if
                                      present, else locks in runtime_dir if set, else none: the
                                      affinity mask is the lease) C */
  MTL_OPT_CPU_SHARED_POLICY = 2023, /* "instance.cpu_shared" MTL_CPU_SHARED_*, when the CFS
                                      quota is below the CPUs of the mask (WARN) C */
  MTL_OPT_ALLOW_NOIOMMU = 2024,    /* "instance.allow_noiommu" bool: accept DMA without an
                                      IOMMU, which survives SIGKILL (0) C */
  MTL_OPT_RUNTIME_DIR = 2025,      /* "instance.runtime_dir" str: the only place MTL writes
                                      files (none: no files) C */
  MTL_OPT_HOTPLUG = 2026,          /* "instance.hotplug" bool: DPDK device-removal handling,
                                      installs DPDK's SIGBUS handler (0) C P */
  MTL_OPT_TELEMETRY = 2027,        /* "instance.telemetry" bool: DPDK telemetry socket in
                                      runtime_dir (0) C P */
  MTL_OPT_MAX_UDP_PAYLOAD = 2028, /* "instance.max_udp_payload" the max_udp_payload of every
                                      TX session of the instance whose field is 0 (legacy
                                      pkt_udp_suggest_max_size); the field's range (1452) C */
  /* 2100-2199 instance: ports and NIC, all per port */
  MTL_OPT_MAX_QUEUES = 2100,       /* "port.max_queues" (HW maximum) C P */
  MTL_OPT_TX_DESC = 2101,          /* "port.tx_desc" C P */
  MTL_OPT_RX_DESC = 2102,          /* "port.rx_desc" C P */
  MTL_OPT_RX_POOL_DATA_SIZE = 2103, /* "port.rx_pool_data_size" C P */
  MTL_OPT_RL_BURST = 2104,         /* "port.rl_burst" ICE PF devarg C P */
  MTL_OPT_RSS_MODE = 2105,         /* "port.rss" MTL_RSS_* (auto) C P */
  MTL_OPT_SHARED_RX_QUEUE = 2106,  /* "port.shared_rx_queue" bool C P */
  MTL_OPT_RX_UDP_PORT_ONLY = 2108, /* "port.rx_udp_port_only" bool C P */
  MTL_OPT_NO_SYSTEM_RX_QUEUES = 2109, /* "port.no_system_rx_queues" bool C P */
  MTL_OPT_PROMISCUOUS = 2110,      /* "port.promiscuous" bool C */
  MTL_OPT_NO_MULTICAST = 2111,     /* "port.no_igmp" bool: SDN fabrics deliver directly C */
  MTL_OPT_VIRTIO_USER = 2112,      /* "port.virtio_user" bool C P */
  MTL_OPT_AF_XDP_COPY = 2113,      /* "port.af_xdp_copy" bool: no zero-copy C P */
  MTL_OPT_TX_MONO_POOL = 2114,     /* "port.tx_mono_pool" bool C P */
  MTL_OPT_TX_NO_BURST_CHECK = 2115, /* "port.tx_no_burst_check" bool C P */
  MTL_OPT_ARP_TIMEOUT_S = 2116,    /* "port.arp_timeout_s" C */
  MTL_OPT_RX_MONO_POOL = 2117,     /* "port.rx_mono_pool" bool C P */
  MTL_OPT_RX_USE_CNI = 2120,       /* "port.rx_use_cni" bool C P */
  MTL_OPT_DHCP = 2121,             /* "port.dhcp" bool: built-in DHCP on a DPDK port C */
  MTL_OPT_RSS_SCHEDS = 2122,       /* "port.rss_scheds" schedulers dispatching shared RSS C P */
  MTL_OPT_XSK_MAP = 2123,          /* "port.xsk_map" str: AF_XDP socket map from the node
                                      agent: a bpffs pin path, "fd:<n>" or "uds:<path>"; MTL
                                      then never loads or attaches an XDP program C */
#if defined(MTL_LATER)
  MTL_OPT_IGMP_VERSION = 2124,     /* "port.igmp_version" MTL_IGMP_* (v3, falling back to
                                      v2 after a v2 querier, RFC 3376) C (Phase 7) */
#endif
  MTL_OPT_IGMP_REPORT_INTERVAL_MS = 2125, /* "port.igmp_report_ms" unsolicited reports until a
                                      query is seen, then answers to queries (10000) R */
  /* 2200-2299 instance: time and logging */
  MTL_OPT_PTP_DOMAIN = 2200,       /* "time.ptp_domain" 0-127, built-in PTP (127, ST 2059-2
                                      §5.5.1) C */
  MTL_OPT_PTP_PI = 2201,           /* "time.ptp_pi" bool: PI servo C P */
  MTL_OPT_PTP_UNICAST = 2202,      /* "time.ptp_unicast" bool: delay requests to the master C */
  MTL_OPT_PHC2SYS = 2203,          /* "time.phc2sys" bool: built-in phc2sys steers the host's
                                      CLOCK_REALTIME; never in a pod; frequency restored at close C P */
  MTL_OPT_PTP_SOURCE_TSC = 2204,   /* "time.ptp_source_tsc" bool C P */
  MTL_OPT_PTP_PI_KP = 2205,        /* "time.ptp_pi_kp" x 1e-9 C P */
  MTL_OPT_PTP_PI_KI = 2206,        /* "time.ptp_pi_ki" x 1e-9 C P */
  MTL_OPT_PHC_TRUST = 2207,        /* "time.phc_trust" MTL_PHC_TRUST_* whether a node daemon
                                      disciplines the PHC (detect: agreement with CLOCK_TAI
                                      at start; cannot see a grandmaster loss) C */
#if defined(MTL_LATER)
  MTL_OPT_TIME_FALLBACK = 2208,    /* "time.fallback" MTL_FALLBACK_* after holdover (FREERUN) C
                                      (Phase 7) */
#endif
  MTL_OPT_PTP_ANNOUNCE_TIMEOUT = 2209, /* "time.ptp_announce_timeout" announce intervals 2-10,
                                      built-in PTP (3, ST 2059-2 §5.5.1) C */
  /* 2210 is not used: log levels belong to the log sinks (mtl_observe.h) */
  MTL_OPT_STAT_DUMP_S = 2211,      /* "log.stat_dump_s" periodic stats in the log; 0 off (10) R */
  MTL_OPT_DMA_DEVICES = 2220,      /* "instance.dma" str: "0000:80:04.0,0000:80:04.1" C P */
#if defined(MTL_LATER)
  MTL_OPT_FREERUN_SLEW_PPM = 2230, /* "time.freerun_slew_ppm" FREERUN follows CLOCK_TAI by
                                      frequency only, bounded; 0 = never (0) R (Phase 7) */
#endif
};

/* Values of enumerated keys (zero is never "derived": absence is). */
enum mtl_late_policy {
  MTL_LATE_DROP = 1,
#if defined(MTL_LATER)
  MTL_LATE_SEND_LATE = 2, /* bounded by tx.late_tolerance_ns, else DROPPED (WOULD_OVERLAP);
                             the default of MTL_MEDIA_SENDER (Phase 7) */
#endif
  MTL_LATE_DEFER = 3, /* AUTO only: the next feasible index (MTL_TXR_DEFERRED) */
};
enum mtl_underrun_policy {
  MTL_UNDERRUN_SKIP = 1,
  MTL_UNDERRUN_EMPTY_ANC = 2,
  MTL_UNDERRUN_KEEPALIVE = 3,
  MTL_UNDERRUN_SILENCE = 4,
};
enum mtl_snap_mode {
  MTL_SNAP_NEAREST = 1,
#if defined(MTL_LATER)
  MTL_SNAP_LOCKED_PHASE = 2, /* (later) */
#endif
};
#if defined(MTL_LATER)
enum mtl_off_grid { MTL_OFF_GRID_RELOCK = 1, MTL_OFF_GRID_REANCHOR = 2, MTL_OFF_GRID_DROP = 3 };
#endif
enum mtl_rows_late { MTL_ROWS_TRUNCATE = 1, MTL_ROWS_PAD = 2, MTL_ROWS_STALL = 3 };
enum mtl_rx_incomplete { MTL_RX_DELIVER = 1, MTL_RX_DISCARD = 2 };
#if defined(MTL_LATER)
enum mtl_mediaclk {
  MTL_MEDIACLK_DIRECT = 1,
  MTL_MEDIACLK_SENDER = 2,
  MTL_MEDIACLK_AUTO = 3, /* from the sender reports' Info Block and the grandmaster */
};
enum mtl_profile { MTL_PROFILE_ST2110 = 1, MTL_PROFILE_IPMX = 2 };
enum mtl_time_fallback { MTL_FALLBACK_HOLDOVER = 1, MTL_FALLBACK_FREERUN = 2 };
enum mtl_anc_window { MTL_ANC_WINDOW_AUTO = 1, MTL_ANC_WINDOW_MEDIA = 2 };
#endif
#define MTL_LINK_OFFSET_AUTO (-1)
#if defined(MTL_LATER)
enum mtl_igmp { MTL_IGMP_V2 = 2, MTL_IGMP_V3 = 3 };
#endif
enum mtl_txq { MTL_TXQ_DEDICATED = 1, MTL_TXQ_SHARED = 2 };
enum mtl_src_port_mode { MTL_SRC_PORT_FIXED = 1, MTL_SRC_PORT_RANDOM = 2, MTL_SRC_PORT_MULTI = 3 };
enum mtl_req { MTL_REQ_PREFER = 1, MTL_REQ_REQUIRE = 2, MTL_REQ_OFF = 3 };
enum mtl_codec_device { /* absent = any device */
  MTL_CODEC_DEVICE_CPU = 1,
  MTL_CODEC_DEVICE_GPU = 2,
  MTL_CODEC_DEVICE_FPGA = 3,
  MTL_CODEC_DEVICE_TEST = 4, /* an in-process test device (mtl_plugin_open with dev) */
  MTL_CODEC_DEVICE_TEST_INTERNAL = 5, /* the library's built-in test codec */
};
enum mtl_rss { MTL_RSS_NONE = 1, MTL_RSS_L3 = 2, MTL_RSS_L3_L4 = 3 };
enum mtl_cvideo_pack { MTL_CVIDEO_PACK_CODESTREAM = 1, MTL_CVIDEO_PACK_SLICE = 2 /* not yet */ };
enum mtl_cvideo_quality { /* enum st22_quality_mode + 1 */
  MTL_CVIDEO_QUALITY_SPEED = 1,
  MTL_CVIDEO_QUALITY_QUALITY = 2,
};
enum mtl_iova { MTL_IOVA_VA = 1, MTL_IOVA_PA = 2 };
enum mtl_cni { MTL_CNI_THREAD = 1, MTL_CNI_TASKLET = 2 };
enum mtl_sys_lcore { MTL_SYS_SHARED = 1, MTL_SYS_DEDICATED = 2 };
enum mtl_cpu_arbitration { MTL_CPUARB_NONE = 1, MTL_CPUARB_LOCKS = 2, MTL_CPUARB_MANAGER = 3 };
enum mtl_cpu_shared { MTL_CPU_SHARED_WARN = 1, MTL_CPU_SHARED_REFUSE = 2, MTL_CPU_SHARED_SLEEP = 3 };
enum mtl_phc_trust { MTL_PHC_TRUST_YES = 1, MTL_PHC_TRUST_NO = 2 };

/* mtl_set_options() flags */
#define MTL_OPTION_RESET 0x1u /* the listed keys go back to absent: the documented default
                                 (value and str are not read) */
/* Changes n options of a live object (instance, port, session) at once, all or none,
   each within its key's rule (C, S, R above): -MTL_EBUSY with reason OPTION_STATE
   otherwise, naming the first key that cannot change. CP. (MS1) */
MTL_API_CP(1) int mtl_set_options(struct mtl_object o, const struct mtl_option* opts,
                                  uint32_t n, uint64_t flags);
static inline int mtl_set_option(struct mtl_object o, const struct mtl_option* opt) {
  return mtl_set_options(o, opt, 1, 0);
}
/* The effective value, the derived default included, in *value. String keys: buf (may be
   NULL with cap 0) receives the string, NUL-terminated (-MTL_ENOSPC if cap is short),
   *value is 0, and the result is the string's length. CP. (MS1) */
MTL_API_CP(1) int mtl_get_option(struct mtl_object o, uint32_t key, uint32_t scope,
                                 int64_t* value, char* MTL_NULLABLE buf, size_t cap);

enum mtl_option_type {
  MTL_OPTION_INT = 1,
  MTL_OPTION_BOOL = 2,
  MTL_OPTION_ENUM = 3,
  MTL_OPTION_NS = 4, /* nanoseconds */
  MTL_OPTION_STR = 5,
};
#define MTL_OPTION_AT_CREATE 0x1u /* settable in the create/open array */
#define MTL_OPTION_STOPPED 0x2u   /* also when STOPPED */
#define MTL_OPTION_RUNNING 0x4u   /* also while running */
enum mtl_option_scope {
  MTL_SCOPE_NONE = 0,
  MTL_SCOPE_PORT = 1,
  MTL_SCOPE_LEG = 2,
  MTL_SCOPE_SCHED = 3,
};
/* mtl_option_desc.flags */
#define MTL_OPTION_PROVISIONAL 0x1u /* P: range, default and meaning may change at any
                                       release, and the key may be withdrawn */
#define MTL_OPTION_DEPRECATED 0x2u  /* a stable key that became an accepted no-op */
/* Output value type of the key list; no instance needed. */
struct mtl_option_desc {
  uint32_t key;
  uint32_t object_kind; /* enum mtl_object_kind it applies to */
  uint32_t type;        /* enum mtl_option_type */
  uint32_t when;        /* MTL_OPTION_* */
  uint32_t scope_kind;  /* enum mtl_option_scope */
  uint32_t flags;       /* MTL_OPTION_PROVISIONAL, MTL_OPTION_DEPRECATED */
  int64_t min;
  int64_t max;
  int64_t def;                 /* the default; derived defaults read as INT64_MIN */
  const char* enum_names;      /* static "name=value" pairs: every value of an ENUM key
                                  ("drop=1,defer=3"), the named values of an INT or NS
                                  key ("auto=-1"); NULL if none */
  char name[48];               /* "rx.skew_budget_ns" */
};
/* Every key the running library implements; *n = total, at most cap written. AS. (MS1) */
MTL_API_AS(1) int mtl_option_list(struct mtl_option_desc* d, size_t desc_size, uint32_t cap,
                                  uint32_t* n);
/* By name; -MTL_EINVAL (OPTION_UNKNOWN) if the running library does not know it,
   -MTL_ENOTSUP (NOT_IMPLEMENTED) for a key it knows but does not implement yet,
   -MTL_ENOTSUP (OPTION_WITHDRAWN) for a provisional key it withdrew. AS. (MS1) */
MTL_API_AS(1) int mtl_option_find(const char* name, struct mtl_option_desc* d, size_t desc_size);
/* One option from text (a GStreamer structure field, an AVDictionary entry, a "key=value"
   argument split at its first '='). name: a key name, optionally followed by "/N", the
   index N (from 0) of the port, leg or scheduler of a scoped key, stored as scope
   MTL_INDEX(N). value by the key's type: INT decimal or 0x hex, optional sign; BOOL 1, 0,
   true, false, yes, no, on, off, any case; ENUM a name of enum_names or its number; NS an
   integer with an optional suffix ns, us, ms or s; INT and NS also a name of enum_names;
   STR the text as is (',', '=' and ':' included), out->str pointing into value, which the
   caller keeps until open, create or mtl_set_options copies it. Returns the key's object
   kind (>= 1: instance, port or session), so the caller routes the option. -MTL_EINVAL
   with OPTION_UNKNOWN, OPTION_RANGE (not of the type, or outside min..max) or
   INVALID_ARGUMENT ("/N" on an unscoped key or not a number), field naming the key and
   detail the value; -MTL_ENOTSUP (NOT_IMPLEMENTED, OPTION_WITHDRAWN) as mtl_option_find.
   AS. (MS2) */
MTL_API_AS(2) int mtl_option_parse(const char* name, const char* value, struct mtl_option* out);

MTL_SIZE_CHECK(mtl_option_desc, 104);

#if defined(__cplusplus)
}
#endif

/* clang-format on */
#endif /* MTL_EXPERIMENTAL_MTL_OPTIONS_H */
