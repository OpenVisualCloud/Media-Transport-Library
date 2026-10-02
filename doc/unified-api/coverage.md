# Coverage: every legacy capability and its unified home

| | |
|---|---|
| Status | Maintained. Design for maintainer review; nothing is implemented. The headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) are normative: where this page and a header disagree, the header wins |
| Date | 2026-10-02 |
| Folded from | the archived simplification studies S1 (coverage inventory), S3 §P6, S7 (samples friction) and S9 (hiding the session headers), the R4 coverage check (§1–§8), research note 07 (modes matrix, §3, §7, §9) |
| Baseline | `main` @ `545a266a`; legacy headers in `include/`; the 17 unified headers (32 functions in `mtl.h`, 125 in all + 14 under `MTL_LATER`) |

This page is the safety net of port first (D-98): a checkable list of every use case today's
public API offers, the unified symbol that reaches it, and the phase that ports it. §2 is the
inventory, one row per use case (U-001 … U-418), grouped by area. §3 is the outcome of the
revision-4 coverage check (counts, regressions R-1…R-10, inconsistencies I-1…I-17). §4 is the
inventory behind hiding the session-level headers (rows V1…X4, gaps H-01…H-19, cuts CUT-1…CUT-8).
§5 is the sample-friction study (F-01…F-26) and what each finding became. §6 lists the feature
asymmetries of today's media families. The field-by-field maps are in [migration.md](migration.md)
§4 (stats map §4.14, mode map §4.16); the scope map an implementer works from is
[implementation-plan.md](implementation-plan.md) §2.1. Removal candidates wait for decision M12
([decisions.md](decisions.md) §2.13); wire-visible default changes for M13.

## 1. How to read the inventory

**Method of the inventory.** Every installed header was read end-to-end (`include/*.h`,
`include/experimental/st20_combined_api.h`; `st_convert_api.h` and `st_convert_internal.h`, 47 +
58 functions, were counted, not read in detail). Consumers were counted with `git grep -l -w
<symbol>` per consumer tree at HEAD, for all 212 flag names, all 377 public function names and
about 80 ops and init fields; the counts are **files**, not call sites. Each row was then checked
symbol by symbol against the headers: a feature described only in prose or a comment, with no
symbol to request it, counts as partial at best.

| Column | Meaning |
|---|---|
| U-ID | S1 row; IDs are grouped by area, so numbers have gaps. 291 rows, 290 classed (U-414 is a note) |
| Legacy | the capability and its legacy symbols; the header follows from the prefix (`mtl_` = `mtl_api.h`, `st_` = `st_api.h` or `st_pipeline_api.h`, `st20_`/`st22_` = `st20_api.h`, `st20p_`/`st22p_` = `st_pipeline_api.h`, `mtl_sch_` = `mtl_sch_api.h`, `st20rc_` = `experimental/st20_combined_api.h`) |
| Unified home | the header symbol, field (`sc.` = `struct mtl_session_config`, `ip.` = `struct mtl_instance_params`, `v.`/`c.`/`a.`/`n.`/`f.` = its video, cvideo, audio, anc, fastmeta member, `u.` = `struct mtl_unit`, `info.` = `struct mtl_session_info`), option key or stats key (contract.md §11.3) |
| Phase | 1–6 = the roadmap phase that ports it ([implementation-plan.md](implementation-plan.md) §6); `Mx` = the branch milestone that delivers it for ST 2110-20; 2P = packet units (decision M14); 7 = later; LATER = declared under `MTL_LATER`; M12 = removal proposal awaiting decision M12; "debug" = `mtl_debug.h`, debug builds |
| Notes | class (**C** core: most apps; **Co** common: frameworks, gateways, test tools; **R** rare: tuning, debugging; **L** legacy-only: a cut candidate), consumers, `S!` or `t`, and what differs |

**Consumer codes** (number = files): SMP `app/` (samples, perf, tools), RXTX `tests/tools/RxTxApp`,
FF FFmpeg plugin, GST GStreamer plugin, OBS OBS plugin, MXL `ecosystem/MTL_with_MXL`, PLG
`plugins/`, PY Python SWIG, RS Rust, KT KahawaiTest (`tests/integration_tests`), UT unit tests,
ext:bobi the public bobi.studio engine. "none" = no in-tree consumer.

**Session-header marks.** `S!` = reachable **only** through a session-level header (`st20_api.h`,
`st30_api.h`, `st40_api.h`, `st41_api.h`, `st_api.h`), no pipeline API offers it (31 rows). `t` =
a type, enum or helper declared in a session-level header that pipeline users also need (38
rows): hiding the header without a replacement breaks every st20p/st22p/st30p/st40p consumer at
compile time, because the pipeline ops and frame structs embed these types (§4).

**Status words** (R4 check): COVERED (a symbol does it), CHANGED (reachable, the model, unit or
default differs), PARTIAL, LATER, NOT COVERED, CUT (on the M12 list). Every row below is now
COVERED or CHANGED unless its notes say otherwise; §3.1 gives the counts.

## 2. Inventory

### 2.1 Instance lifecycle and init parameters

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-001 | open an instance on a list of ports: `mtl_init`, `mtl_init_params.port[]`, `num_ports` | `mtl_instance_open(&ip, &mt)`, `ip.ports[].name`, `ip.port_count`; `MTL_PORTS` environment fallback | 1 (M0) | C; every consumer (SMP 51, KT 8, MXL 8, PY 7, FF 4) |
| U-002 | close: `mtl_uninit` | `mtl_instance_close(mt, timeout_ns)` (refcounted; lives until its last session retires) | 1 (M0) | C; every consumer |
| U-003 | re-open in the same process (#1341, broken today) | open after close works by design (EAL keeps its arguments; same ports, a subset of the CPUs) | 1 | Co; FF, GST, dvled work around it. CHANGED |
| U-004 | share one instance between components (`mtl_common.c:25-28`, `gst_mtl_common.c:12-18`) | `MTL_INSTANCE_SHARED`; a differing later open is `-MTL_EEXIST` (`INSTANCE_MISMATCH`) | 1 | Co; FF, GST, ext dvled. The revision-3 merge table is gone (OI-1); OBS and FFmpeg settings that break sharing: OI-43 |
| U-005 | device start/stop, auto start/stop: `mtl_start`, `mtl_stop`, `MTL_FLAG_DEV_AUTO_START_STOP` | no instance start; `mtl_session_start/stop`, `mtl_session_open` (create + start) | 1 | C; SMP 35, KT 31, FF 6, MXL 7, PY 7. CHANGED; bridged instance before `mtl_start()`: OI-2 |
| U-006 | abort from a signal handler: `mtl_abort` | `mtl_instance_abort` (AS) and `mtl_instance_interrupt(mt, 1)` (AS) | 1 | Co; SMP 3, RXTX 1. Was PARTIAL in the check; closed in the headers since |
| U-007 | static source IP per port: `sip_addr[]`, `mtl_para_sip_set` | `mtl_port_spec.sip` | 1 | C; all |
| U-008 | netmask per port: `netmask[]` | `mtl_port_spec.prefix_len` (0 = /24) | 1 | Co; SMP 1, RXTX 2, RS 7 |
| U-009 | gateway per port: `gateway[]` | `mtl_port_spec.gateway` | 1 | Co; SMP 1, RXTX 2, RS 7 |
| U-010 | DHCP on a DPDK port: `net_proto[] = MTL_PROTO_DHCP` | `MTL_OPT_DHCP` (`port.dhcp`); kernel backends: `sip` all zero | 1 | R; SMP 1, RXTX 2 |
| U-011 | TX/RX queue counts: `tx_queues_cnt[]`, `rx_queues_cnt[]`, `st_tx/rx_sessions_queue_cnt` | `mtl_port_spec.tx_queues/rx_queues` (0 = auto), `MTL_OPT_MAX_QUEUES` | 1 | C; SMP 3, RXTX 3, FF 2, GST 1, OBS 2, KT 3 |
| U-012 | force a port's NUMA node: `MTL_PORT_FLAG_FORCE_NUMA` + `socket_id` | `mtl_port_spec.numa` (node + 1) | 1 | R; RXTX 1 |
| U-013 | bring up with ports link-down: `MTL_FLAG_ALLOW_DOWN_PORTS`, `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION` | the default: open never waits for links, legs are never pruned; `mtl_leg_status.oper`, `MTL_EVENT_PORT_LINK`, `mtl_instance_get_health` | 1 | Co; GST 2, RXTX 1. CHANGED |
| U-014 | ICE PF rate-limit burst devarg: `port_params[].rl_burst_size` | `MTL_OPT_RL_BURST` (`port.rl_burst`) | 1 | R; none |
| U-015 | restrict MTL to an lcore list: `lcores` | `ip.lcores` | 1 | Co; SMP 2, OBS 2, MXL 7, PY 7, RXTX 1 |
| U-016 | choose the DPDK main lcore: `main_lcore` | `MTL_OPT_MAIN_LCORE` | 1 | R; none (#1179) |
| U-017 | log level at init: `log_level` | `MTL_OPT_LOG_LEVEL` (`enum mtl_log`, +1) | 1 | C; SMP 3, RXTX 2, FF, GST, OBS, MXL 7, RS 6, KT 5. CHANGED: an option, not a field |
| U-018 | log level at runtime: `mtl_set_log_level`, `mtl_get_log_level` | `mtl_set_option` / `mtl_get_option` with `MTL_OPT_LOG_LEVEL` (R; this instance's lines) | 1 | R; KT 1 |
| U-019 | route logs into the app's logger (#657): `mtl_set_log_printer` | `mtl_log_set_sink(fn, user, prefix)` (process-wide, `mtl_observe.h`) | 1 | Co; RXTX 1, UT 3 |
| U-020 | custom line prefix: `mtl_set_log_prefix_formatter` | the static `prefix` of `mtl_log_set_sink`; a dynamic prefix is formatted in the sink | 1 | R; RXTX 1. CHANGED |
| U-021 | log to a FILE: `mtl_openlog_stream` | the sink writes to the app's FILE | 1 | R; RXTX 1. CHANGED |
| U-022 | periodic stats dump with a callback: `stat_dump_cb_fn`, `dump_period_s`, `priv` | `MTL_OPT_STAT_DUMP_S` (`log.stat_dump_s`, R) + an exporter loop over `mtl_stat_list/read` ([migration.md](migration.md) §11.8) | 1 | Co; RXTX 1, KT 1. CHANGED: no callback |
| U-023 | force/query IOVA mode: `iova_mode`, `mtl_iova_mode_get` | `MTL_OPT_IOVA_MODE` (`MTL_IOVA_VA/PA`); keys `caps.iova_va`, `instance.iova_mode` | 1 | R; RXTX 1, KT 1, SMP 1 |
| U-024 | RSS mode; query it: `rss_mode`, `mtl_rss_mode_get` | `MTL_OPT_RSS_MODE` (`MTL_RSS_NONE/L3/L3_L4`, legacy + 1) | 1 | R; SMP 1, RXTX 2, PY 7, KT 1 |
| U-025 | schedulers for shared-RSS dispatch: `rss_sch_nb[]` | `MTL_OPT_RSS_SCHEDS` (`port.rss_scheds`) | 1 | R; RXTX 1 |
| U-026 | descriptor ring sizes: `nb_tx_desc`, `nb_rx_desc` | `MTL_OPT_TX_DESC`, `MTL_OPT_RX_DESC` (per port) | 1 | R; SMP 1, RXTX 1, MXL 3, PY 7, KT 1 |
| U-027 | RX mempool data room: `rx_pool_data_size` | `MTL_OPT_RX_POOL_DATA_SIZE` | 1 | R; RXTX 1 |
| U-028 | memzone limit: `memzone_max` | `MTL_OPT_MEMZONE_MAX` | 1 | R; none |
| U-029 | maximum UDP payload: `pkt_udp_suggest_max_size` | `MTL_OPT_MAX_UDP_PAYLOAD` (per session; set on the instance = the default); packet units `sc.packet.slot_bytes` | 1 | R; none. CHANGED: per session |
| U-030 | ARP timeout: `arp_timeout_s` | `MTL_OPT_ARP_TIMEOUT_S` | 1 | R; RXTX 1 |
| U-031 | name the DMA devices: `dma_dev_port[]`, `num_dma_dev_port`, `mtl_para_dma_port_set` | `MTL_OPT_DMA_DEVICES` (`instance.dma`, comma-separated) | 1 | Co; SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 1 |
| U-032 | deprecated sizing: `tx_sessions_cnt_max`, `rx_sessions_cnt_max` | none | M12 | L; MXL 1; marked deprecated ("Use tx_queues_cnt") |
| U-033 | instance info and session counts: `mtl_get_fix_info`, `mtl_get_var_info`, `st_get_var_info` | keys `instance.*`, `instance.sessions{essence,dir}`; `mtl_instance_list_sessions`; `info.essence` | 1 | R; RXTX 1, KT 7; t |
| U-034 | version string: `mtl_version()`, `MTL_VERSION` | `mtl_version_string()`, `mtl_version_num()`, `MTL_API_VERSION` | 1 | Co; SMP 1, PY 1, RS 1, KT 1 |
| U-035 | is MtlManager alive: `mtl_is_manager_alive` | key `instance.manager`; `MTL_EVENT_MANAGER_LOST` | 1 | R; none outside lib |
| U-036 | survive MtlManager loss (implicit shm fallback) | `MTL_OPT_CPU_ARBITRATION` (auto: MtlManager if present), `MTL_EVENT_MANAGER_LOST`, `MTL_REASON_MANAGER_LOST`, `MTL_REASON_MANAGER_REQUIRED` | 1, 2 (reconnect) | Co: the check's instance flag (`MTL_INSTANCE_MANAGER_OPTIONAL`) was removed by D-92. Open: auto picks the manager in a pod that mounts its socket for AF_XDP only (OI-48) |

### 2.2 Time source and PTP

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-040 | built-in PTP client: `MTL_FLAG_PTP_ENABLE` | `ip.time_source = MTL_TIME_SOURCE_PTP_BUILTIN`; on a VF it disciplines MTL's own time base as today (`mt_ptp.c:1390-1393`) | 1 | Co; SMP 2, RXTX, FF, GST, PY 7, KT 2. Revision 3's duplicate flag is gone |
| U-041 | PI servo and gains: `MTL_FLAG_PTP_PI`, `kp`, `ki` | `MTL_OPT_PTP_PI`, `MTL_OPT_PTP_PI_KP`, `MTL_OPT_PTP_PI_KI` (× 1e-9) | 1 | R; RXTX 1, FF 1, RS 1 |
| U-042 | unicast delay requests: `MTL_FLAG_PTP_UNICAST_ADDR` | `MTL_OPT_PTP_UNICAST` (bool: to the learned master) | 1 | R; RXTX 1, FF 1 |
| U-043 | built-in phc2sys: `MTL_FLAG_PHC2SYS_ENABLE` | `MTL_OPT_PHC2SYS` (never in a pod) | 1 | R; SMP 1, RXTX 1 |
| U-044 | PTP time from TSC: `MTL_FLAG_PTP_SOURCE_TSC` | `MTL_OPT_PTP_SOURCE_TSC` | 1 | R; SMP 1, RXTX 1, KT 1 |
| U-045 | app time function: `ptp_get_time_fn` + `priv` | `MTL_TIME_SOURCE_USER` + `mtl_time_user_update` (pushed pairs, not a pull); `MTL_TIME_SOURCE_CLOCK_TAI`, `_SYSTEM_TAI` | 1 | Co; SMP 1, RXTX 2, FF 1 (CLOCK_TAI), KT 5. CHANGED |
| U-046 | notify on each sync: `ptp_sync_notify`, `mtl_ptp_sync_notify_meta` | `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_STEP`; keys `time.offset_ns`, `time.utc_offset_s`, `time.last_sync_age_ns` | 3 | R; RXTX 1 |
| U-047 | HW RX timestamps: `MTL_FLAG_ENABLE_HW_TIMESTAMP` | `MTL_INSTANCE_HW_TIMESTAMP`, `MTL_OPT_HW_TIMESTAMPS`, `MTL_RXF_HW_ARRIVAL`, `MTL_PKTE_HW_ARRIVAL` | 1 | R; SMP 1, RXTX 1, PY 1, RS 1, KT 4 |
| U-048 | read PTP/TAI time: `mtl_ptp_read_time`, `mtl_ptp_read_time_raw` | `mtl_time_now`; `mtl_time_cross`, `mtl_time_convert` | 1; 3 | Co; SMP 2, RXTX 4, KT 11, ext:bobi |

### 2.3 Scheduler, lcores, threading

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-050 | schedulers as pthreads: `MTL_FLAG_TASKLET_THREAD` | `MTL_INSTANCE_TASKLET_THREAD` | 1 | Co; RXTX 1, RS 1, KT 1 |
| U-051 | sleep when idle: `MTL_FLAG_TASKLET_SLEEP` | `MTL_INSTANCE_TASKLET_SLEEP` | 1 | Co; RXTX 1 |
| U-052 | per-scheduler sleep at runtime: `mtl_sch_enable_sleep` | `MTL_OPT_SCHED_SLEEP_US` per scheduler (R; 0 = never) | 1 | R; KT 1, UT 2 |
| U-053 | maximum sleep: `mtl_sch_set_sleep_us` | `MTL_OPT_SCHED_SLEEP_US` | 1 | R; RXTX 1 |
| U-054 | separate lcore for RX video: `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` | `MTL_OPT_RX_SEPARATE_VIDEO_LCORE` | 1 | Co; SMP 4, RXTX 2, FF 1, KT 3 |
| U-055 | migrate busy video sessions; opt out: `MTL_FLAG_TX/RX_VIDEO_MIGRATE`, `ST20(P)_RX_FLAG_DISABLE_MIGRATE` | `MTL_OPT_TX_VIDEO_MIGRATE`, `MTL_OPT_RX_VIDEO_MIGRATE`; per session `MTL_OPT_MIGRATE` | 1 | R; FF 1, RXTX 2, KT 1. The check said unified sessions never migrate (Q-THR-6); migration is now ported ([migration.md](migration.md) §4.15) |
| U-056 | data quota per scheduler: `data_quota_mbs_per_sch` | `MTL_OPT_SCHED_QUOTA_MBS` | 1 | Co; RXTX 1, MXL 5, KT 1 |
| U-057 | audio sessions per scheduler: `tx/rx_audio_sessions_max_per_sch` | `MTL_OPT_SCHED_TX_AUDIO_MAX`, `MTL_OPT_SCHED_RX_AUDIO_MAX` | 1 | R; RXTX 2 |
| U-058 | tasklets per scheduler: `tasklets_nb_per_sch` | `MTL_OPT_TASKLETS_PER_SCHED` | 1 | R; none |
| U-059 | dedicated system lcore: `MTL_FLAG_DEDICATED_SYS_LCORE` | `MTL_OPT_SYS_LCORE = MTL_SYS_DEDICATED` | 1 | R; RXTX 1 |
| U-060 | CNI on a thread or tasklet: `MTL_FLAG_CNI_THREAD`, `_CNI_TASKLET` | `MTL_OPT_CNI` (`MTL_CNI_THREAD/TASKLET`) | 1 | R; RXTX 1, RS 1, KT 3 |
| U-061 | tasklet time measurement: `MTL_FLAG_TASKLET_TIME_MEASURE` | `MTL_OPT_TASKLET_TIME_MEASURE`; key `sched.tasklet_p9999_ns` | 1 | R; RXTX 1, RS 1 |
| U-062 | bind threads to the NIC NUMA or not: `MTL_FLAG_BIND_NUMA` (never read), `MTL_FLAG_NOT_BIND_NUMA` | bound by default; opt out `MTL_OPT_NO_BIND_NUMA` | 1 | Co; BIND: SMP 2, FF, OBS 2, PY 7, RS 6; NOT_BIND: RXTX 1. Fixes revision 3's inverted flag |
| U-063 | do not bind the process: `MTL_FLAG_NOT_BIND_PROCESS_NUMA` | `MTL_OPT_NO_BIND_PROCESS_NUMA` | 1 | R; none |
| U-064 | cores across NUMA nodes: `MTL_FLAG_ALLOW_ACROSS_NUMA_CORE` | `MTL_OPT_ACROSS_NUMA_CORES` | 1 | R; RXTX 1, KT 1 |
| U-065 | 512-bit SIMD burst: `MTL_FLAG_RXTX_SIMD_512` | `MTL_OPT_SIMD_512` | 1 | R; RXTX 1, KT 1, UT 1 |
| U-066 | query CPU SIMD level: `mtl_get_simd_level`, `_name` | key `instance.simd_level` (`enum mtl_simd`) | 1 | R; SMP 14 (print), RS 1, KT 1 |
| U-067 | borrow an MTL lcore: `mtl_get_lcore`, `mtl_bind_to_lcore`, `mtl_put_lcore` | none (CUT-5b) | M12 | Co; SMP 14, RXTX 1, KT 1; perf tools move to the internal tier |
| U-068 | app-created scheduler: `mtl_sch_create/start/stop/free`, `mtl_sch_ops` | none; manual progress `mtl_sched_run_once` (`mtl_util.h`, LATER) | M12 | R; KT 1 (`sch_test.cpp`) |
| U-069 | app tasklet on an MTL scheduler: `mtl_sch_register_tasklet/unregister`, `mtl_tasklet_ops` (start, stop, handler, `advice_sleep_us`) | none (H-08) | M12 | R; KT 1; if kept: an advanced header with the inline-safe DP subset |
| U-070 | session's scheduler index: `st20/st22/st20p_*_get_sch_idx` | `info.sched_index` | 1 (M3a) | R; RXTX 1, KT 1; t |
| U-071 | name a thread: `mtl_thread_setname` | none (CUT-8) | M12 | R; RXTX 20 |
| U-072 | SysV shm lcore table print/clean: `mtl_lcore_shm_print`, `_clean` | none: the SysV table is removed (D-92, OI-4); an in-tree admin tool at most | M12 | L; SMP 1 (`app/tools/lcore_shmem_mgr.c`); `doc/design.md:51` advises against it |
| U-073 | sleep and busy-delay: `mtl_sleep_us`, `mtl_delay_us` | libc (CUT-8) | M12 | L; KT 1 / none |

### 2.4 Port data-path tuning (instance flags today)

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-075 | shared TX queue: `MTL_FLAG_SHARED_TX_QUEUE` | port default `MTL_OPT_SHARED_TX_QUEUE`; per session `MTL_OPT_TX_QUEUE = MTL_TXQ_SHARED`, which wins | 1 | Co; SMP 1, RXTX 1, RS 1. Today it forces TSC pacing for the whole port (`mt_dev.c:1446-1450`) |
| U-076 | shared RX queue: `MTL_FLAG_SHARED_RX_QUEUE` | `MTL_OPT_SHARED_RX_QUEUE` | 1 | R; SMP 1, RXTX 1, RS 1 |
| U-077 | RX through the CNI queue: `MTL_FLAG_RX_USE_CNI` | `MTL_OPT_RX_USE_CNI` | 1 | R; RXTX 1 |
| U-078 | flow rules on UDP port only: `MTL_FLAG_RX_UDP_PORT_ONLY` | `MTL_OPT_RX_UDP_PORT_ONLY` | 1 | R; RXTX 1 |
| U-079 | no system RX queues: `MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES` | `MTL_OPT_NO_SYSTEM_RX_QUEUES` | 1 | R; RXTX 1 |
| U-080 | promiscuous RX: `MTL_FLAG_NIC_RX_PROMISCUOUS` | `MTL_OPT_PROMISCUOUS` | 1 | R; RXTX 1 |
| U-081 | no IGMP join (SDN fabrics): `MTL_FLAG_NO_MULTICAST` | `MTL_OPT_NO_MULTICAST` (`port.no_igmp`) | 1 | R; RXTX 1 |
| U-082 | virtio_user exception path: `MTL_FLAG_VIRTIO_USER` | `MTL_OPT_VIRTIO_USER` | 1 | R; RXTX 1 |
| U-083 | AF_XDP copy mode only: `MTL_FLAG_AF_XDP_ZC_DISABLE` | `MTL_OPT_AF_XDP_COPY` | 1 | R; RXTX 1, KT 1 |
| U-084 | mono RX/TX mempool: `MTL_FLAG_RX_MONO_POOL`, `_TX_MONO_POOL` | `MTL_OPT_RX_MONO_POOL`, `MTL_OPT_TX_MONO_POOL` | 1 | R; RXTX 1, KT 1 |
| U-085 | force TX copy (no chained mbuf): `MTL_FLAG_TX_NO_CHAIN` | `MTL_OPT_TX_COPY` (per session; with `MTL_SESSION_REQUIRE_DIRECT` `-MTL_EINVAL`) | 1 | R; RXTX 1, KT 1. CHANGED: per session |
| U-086 | skip the TX burst check: `MTL_FLAG_TX_NO_BURST_CHK` | `MTL_OPT_TX_NO_BURST_CHECK` | 1 | R; RXTX 1 |
| U-087 | random or multiple UDP source ports: `MTL_FLAG_RANDOM_SRC_PORT`, `_MULTI_SRC_PORT` | `MTL_OPT_SRC_PORT_MODE` (`MTL_SRC_PORT_RANDOM/MULTI`), per session | 1 | R; RXTX 1, KT 2 |
| U-088 | pacing engine per port (AUTO/RL/TSC/TSN/PTP/BE/TSC_NARROW): `mtl_init_params.pacing`, `enum st21_tx_pacing_way` | `MTL_OPT_PACING` per session (`MTL_PACING_HW_RATE`, `_HW_LAUNCH`, `_SW`, `_SW_NARROW`, `_PTP`, `_BEST_EFFORT`), `MTL_OPT_PACING_REQUIRED` | 1 | Co; RXTX, KT, FF. CHANGED; on the instance = the default; `info.pacing_class` |
| U-089 | UDP transport remnants: `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | none | M12 | L; SMP 1 (`sample_util.c:276` sets the flag, no effect), RS 1; stack deleted in `2b182cd87` |
| U-090 | simulated loss on redundant TX: `MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS`, `port_packet_loss[]` | `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` per session and leg | debug | R; KT 1 |

### 2.5 Ports and backends

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-092 | DPDK PMD port (PCI BDF), PF or VF: `MTL_PMD_DPDK_USER` | `mtl_port_spec.name` = BDF; `MTL_BACKEND_DPDK_PMD` | 1 | C; all |
| U-093 | native AF_XDP: `native_af_xdp:<if>` | name prefix; `MTL_BACKEND_AF_XDP`; `MTL_OPT_XSK_MAP` in pods | 1 | Co; RXTX, KT |
| U-094 | kernel socket (experimental): `kernel:<if>` | name prefix; `MTL_BACKEND_KERNEL_SOCKET` | 1 | R; RXTX, KT |
| U-095 | DPDK AF_XDP and AF_PACKET PMDs: `dpdk_af_xdp:`, `dpdk_af_packet:` | none; the `mtl_port_spec.name` comment lists them for removal | M12 | L; RXTX; regression R-2 (revision 3 had enum values) |
| U-096 | backend by name; DPDK-based or AF_XDP test: `mtl_pmd_by_port_name`, `mtl_pmd_is_dpdk_based`, `mtl_pmd_is_af_xdp` | key `caps.backend` (`enum mtl_backend`) after open; before open the name prefix says it | 1 | Co; SMP 1, RXTX 3, FF 1, GST 1, PY 7, KT 2. CHANGED |
| U-097 | port NUMA node: `mtl_get_numa_id` | key `caps.numa` | 1 | R; KT 1 |
| U-098 | port address in use (incl. a DHCP lease): `mtl_port_ip_info` | `mtl_port_get_spec` (address, prefix, gateway, MAC in use) | 1 | R; RXTX 1, KT 1 |
| U-099 | port I/O counters: `mtl_get_port_stats`, `struct mtl_port_status` | keys `port.rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`, `rx_errors`, `tx_errors`, `rx_missed`, `rx_nombuf` ([migration.md](migration.md) §4.14) | 1 | Co; RXTX 1, ext:bobi |
| U-100 | reset port counters: `mtl_reset_port_stats` | none: cumulative counters, deltas by the reader | 1 | R; RXTX 1. CHANGED |
| U-101 | interface IP helper: `mtl_get_if_ip` | none | M12 | L; 0 consumers |
| U-102 | init-param setters for bindings: `mtl_para_*_set/get`, `mtl_p_port`, `mtl_r_port`, `mtl_p/r_sip_addr` | plain POD fields, `MTL_ADDR(T)`, `MTL_PORTS` | 1 | Co; PY 7, MXL 6, SMP 1, RXTX 1. CHANGED; spec strings are gone (D-97) |
| U-103 | Windows (DPDK only), `lib/windows/` | `mtl_session_get_wait_handle` = an auto-reset event HANDLE; Linux errno values on every OS; compile-only CI (G-74) | 1 | R; MSVC sample |
| U-104 | many processes on one NIC (SR-IOV + MtlManager) | manager model unchanged; `MTL_OPT_CPU_ARBITRATION` | 1 | Co; every multi-app deployment |

### 2.6 Memory, DMA and zero-copy plumbing

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-110 | hugepage alloc: `mtl_hp_malloc`, `mtl_hp_zmalloc`, `mtl_hp_free` | `mtl_mem_alloc` (region + va), `mtl_mem_close`; no malloc-style pointer API | 4 | Co; SMP 24, RXTX 5, KT 4. CHANGED |
| U-111 | IOVA of memory or a plane: `mtl_hp_virt2iova`, `st_frame.iova[]`, `st_frame_iova`, `st_ext_frame.iova[]`, `st20_ext_frame.buf_iova` | `mtl_mem_iova(region, offset, &iova)`; `mtl_mem_info.va` gives a library-pool plane's offset | 4 | Co; SMP 18, PY 4, KT 2; t. Was PARTIAL in the check; closed since; non-hugepage import IOVA: OI-41 |
| U-112 | DMA-map app memory: `mtl_dma_map`, `mtl_dma_unmap` | `mtl_mem_import`, `mtl_mem_close` | 4 | Co; SMP 3, MXL 2, KT 5. Today it maps port P only (SF-07) |
| U-113 | page-aligned DMA block: `mtl_dma_mem_alloc/free/addr/iova` | `mtl_mem_alloc` + `mtl_mem_iova` | 4 | R; SMP 5, KT 2 |
| U-114 | page size and alignment: `mtl_page_size`, `mtl_size_page_align` | `mtl_mem_info.page_size`, keys `caps.page_size`, `caps.hugepage_sizes`; no align helper (CUT-8) | 4 | R; SMP 3, MXL 1, KT 5 |
| U-115 | fast memcpy: `mtl_memcpy`, `mtl_memcpy_action` | `mtl_unit_copy_in/out` for leased units only; a public `mtl_memcpy` is CUT-8 | M12 | Co; SMP 21, RXTX 15, FF 6, OBS 1, PLG 2, RS 2, PY 2, KT 4. Open: OI-42 recommends removal |
| U-116 | user DMA engine: `mtl_udma_create/free/copy/fill/fill_u8/submit/completed` | none (CUT-5); sessions use DMA through `MTL_OPT_DMA` | M12 | R; SMP 15 (dma samples), KT 2 |
| U-117 | RX frames in GPU VRAM: `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`, `gpu_context`, `st20_rx_ops.gpu_direct_framebuffer_in_vram_device_address` | `mtl_mem_import_device` (LATER) | 4–6 | R; SMP 1, FF 1 (session field: none); Q-MEM-8 (NG4) |

### 2.7 Session plumbing common to every essence

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-120 | create / free: `st*_tx/rx_create`, `*_free` | `mtl_session_create`, `mtl_session_open`, `mtl_session_close(s, timeout)` | 1, 2 | C; every consumer |
| U-121 | live from create (no start) | explicit `mtl_session_start` (`struct mtl_when`: `MTL_NOW`, `MTL_AT_TAI`, `MTL_AT_INDEX`), `mtl_session_stop` (DRAIN, FLUSH); `mtl_session_open` keeps "live at once" | 1 | C; all. CHANGED |
| U-122 | TX destination IP + UDP per leg: `dip_addr[]`, `udp_port[]`, `st_tx_port` | `sc.flows[i].ip`, `.udp_port`; `mtl_flow_ipv4()` | 1 | C; all |
| U-123 | RX group or unicast source + UDP per leg: `ip_addr[]`, `udp_port[]`, `st_rx_port` | `sc.flows[i].ip`, `.udp_port` | 1 | C; all |
| U-124 | SSM source filter: `mcast_sip_addr[]` | `sc.flows[i].source_filter` | 1 | Co; RXTX 10, MXL 1 |
| U-125 | bind a leg to a named port: `port[][]` | `sc.flows[i].port` (index + 1), `mtl_port_find` | 1 | C; all |
| U-126 | ST 2022-7 dual-leg TX and RX merge: `num_port = 2` | `sc.flows[1]`, `sc.legs_disabled`, `mtl_leg_status`, `pkts_received[leg]` | 1 | C; many |
| U-127 | payload type (RX 0 = off): `payload_type` | `sc.flows[i].payload_type` | 1 | C; all |
| U-128 | SSRC (TX 0 = random, RX 0 = off): `ssrc` | `sc.flows[i].ssrc` | 1 | Co; RXTX 4, KT 5, RS 1 |
| U-129 | UDP source port: `udp_src_port[]` | `sc.flows[i].udp_src_port` | 1 | R; none |
| U-130 | static destination MAC: `tx_dst_mac[]` + `*_TX_FLAG_USER_P_MAC/USER_R_MAC` | `MTL_FLOWF_USER_MAC` + `sc.flows[i].dst_mac` | 1 | Co; SMP 2, RXTX 10 |
| U-131 | session name: `name` | `sc.name` (copied, unique, `""` = generated, OI-11) | 1 | Co; many |
| U-132 | callback private pointer: `priv` | per-unit `u.cookie`; the session is `mtl_tx_result.session` / the event origin | 1 | C; every callback user. CHANGED; regression R-7, OI-42: a result cookie only |
| U-133 | per-session NUMA: `*_FLAG_FORCE_NUMA` + `socket_id` (not ST40/41) | `MTL_OPT_NUMA` | 1 | R; RXTX 11 |
| U-134 | dedicated TX queue for audio, ANC, fastmeta: `ST30/40/41(P)_TX_FLAG_DEDICATE_QUEUE` | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | 2 | R; RXTX 1, KT 1 |
| U-135 | change TX destination at runtime: `st*_tx_update_destination`, `st_tx_dest_info` | `mtl_session_update(s, &sc, MTL_UPDATE_FLOWS, &when, &planned)` (every leg atomically) | 2 | Co; MXL 1, KT 1; t |
| U-136 | change RX source at runtime: `st*_rx_update_source`, `st_rx_source_info` | `mtl_session_update(…, MTL_UPDATE_FLOWS, …)` | 2 | Co; KT 1; t |
| U-137 | RX burst size: `rx_burst_size` | `MTL_OPT_RX_BURST` | 1 | R; SMP 2, RXTX 3, PY 1 |
| U-138 | number of frame buffers: `framebuff_cnt` | `sc.pool_count`; `info.max_count` | 1 | C; all |
| U-139 | app-managed flows and IGMP; queue IDs: `*_RX_FLAG_DATA_PATH_ONLY`, `*_rx_get_queue_meta`, `st_queue_meta` | none; route `mtl_open_ext` reserved (`mtl_util.h`, LATER) | M12 | L; flag: none; queue meta KT 3; t. Q-MODE-5: suspected NULL dereference (`mt_queue.c:56`, SP-02); regression R-10 |
| U-140 | pcapng capture of RX packets: `st20/st22/st20p/st22p/st20rc_rx_pcapng_dump`, `st_pcap_dump_meta` | `mtl_session_capture` / `_capture_stop` + `MTL_EVENT_CAPTURE_DONE` (asynchronous) | 2 | R; RXTX 1, KT 1; t. CHANGED: no synchronous mode |
| U-141 | NACK retransmission ST20/ST22: `*_FLAG_ENABLE_RTCP`, `st_tx_rtcp_ops.buffer_size`, `st_rx_rtcp_ops` | `MTL_OPT_RTX`, `_RTX_BUFFER_PKTS`, `_RTX_NACK_INTERVAL_US`, `_RTX_SEQ_BITMAP`, `_RTX_SEQ_SKIP`; keys `rtx.*` | 1, 2 | R; RXTX 1, KT 4; t |
| U-142 | RTCP flags on ST30/40/41/st40p | none (CUT-7: no effect today) | M12 | L; RXTX 1 passes them through; Q-MODE-8 |
| U-143 | TX hang-detect timeout: `tx_hang_detect_ms` | `MTL_OPT_TX_HANG_DETECT_NS` | 1 | R; none. ANC and fastmeta have no detection today: OI-45 |
| U-144 | TX queue recovery / fatal: `ST_EVENT_RECOVERY_ERROR`, `ST_EVENT_FATAL_ERROR` (video only) | `MTL_EVENT_RECOVERY`, `MTL_EVENT_SESSION_STATE` to ERROR, `MTL_REASON_TX_QUEUE_FATAL`, `_HANG` (every essence) | 1 (M3a); 5 (worker) | Co; RXTX 2, KT 2; t |
| U-145 | create fails with a reason (today NULL + a log line) | negative `MTL_E*`, `mtl_last_error`, `mtl_reasons.h` | 1 | C; all |

### 2.8 TX timing and pacing (every essence)

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-150 | library-paced TX on the epoch grid (default) | `sc.media_mode = MTL_MEDIA_AUTO` | 1 (M1) | C; all |
| U-151 | user pacing at a TAI time: `*_TX_FLAG_USER_PACING` + `timestamp`/`tfmt` | `MTL_MEDIA_TAI` (snapped), or `MTL_SUBMIT_NOT_BEFORE` + `u.launch_tai_ns` | 3 (M3b) | Co; RXTX, GST 1, KT 4. MEDIA_CLK input is converted, not ignored |
| U-152 | exact user pacing: `ST20/ST20P/ST40/ST40P_TX_FLAG_EXACT_USER_PACING` | `MTL_SUBMIT_EXACT` + `u.launch_tai_ns`, `MTL_INFO_NON_COMPLIANT` | 3 (M3b) | R; RXTX 1, KT 1, UT 3; audio and fastmeta `-MTL_ENOTSUP` until Phase 3 (OI-39) |
| U-153 | user-chosen RTP timestamp: `*_TX_FLAG_USER_TIMESTAMP` (absent on st30p) | `MTL_SUBMIT_RTP_TS` + `u.rtp`; needs INDEX or TAI (`MTL_REASON_RTP_TS_AUTO`); library slot + app RTP = INDEX with `mtl_tx_next_slot` | 3 (M3b) | Co; SMP 3, KT 2, UT 3. CHANGED |
| U-154 | RTP exactly on the epoch: `ST20(P)_TX_FLAG_RTP_TIMESTAMP_EPOCH` | the default (D-10) | 1 | R; RXTX 1, UT 2 |
| U-155 | today's default ST20 RTP from the TX cursor (`st_tx_video_session.c`) | not the default (M13); `sc.media_time_offset_ns` approximates it | 1 (M1; compared in M6, G-99) | Co; every legacy ST20 sender. CHANGED, wire-visible; mixed-API hazard ([migration.md](migration.md) §6.4) |
| U-156 | RTP delta / TROFF trim: `rtp_timestamp_delta_us` (st20, st20p, st30, st30p) | `sc.media_time_offset_ns` (× 1000), `MTL_OPT_INDEX_OFFSET`, `v.troffset_ns`, `MTL_OPT_TROFFSET_NS` | 1 | R; RXTX 2, KT 2, ext:bobi |
| U-157 | drop late frames: `*P_TX_FLAG_DROP_WHEN_LATE` (pipelines, needs USER_PACING) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` (every mode) | 3 (M3a, M3b) | Co; RXTX 1, KT 1 |
| U-158 | late notification: `notify_frame_late(priv, epoch_skipped)` | `mtl_tx_result` status `MTL_TX_LATE`/`MTL_TX_DROPPED`, `reason`, `margin_ns`; `mtl_tx_result_full.slots_skipped_before`; AUTO: `MTL_TXR_RESLOTTED` | 1 (M3a) | Co; KT 1 |
| U-159 | per-epoch vsync: `*_FLAG_ENABLE_VSYNC`, `ST_EVENT_VSYNC`, `st10_vsync_meta` (ST20/22 only) | `MTL_OPT_EPOCH_TICK` + `MTL_EVENT_EPOCH_TICK` | 1 (M3a) | R; RXTX 1, KT 1; t |
| U-160 | ST 2110-21 sender type: `enum st21_pacing`, `transport_pacing` | `v.sender_type`, `c.sender_type` (`MTL_SENDER_N/NL/W`) | 1 | Co; RXTX 2, MXL 2, KT 1; t |
| U-161 | TR offset, TRS, VRX: `st20_tx_get_pacing_params`, `st20p_tx_get_pacing_params` | keys `info.troffset_ns`, `info.trs_ps`, `info.vrx_full`; `info.min_submit_lead_ns` | 1 (M3a) | R; KT 3. CHANGED: keys, not typed fields |
| U-162 | RL tuning: `start_vrx`, `pad_interval`, `*_ENABLE_STATIC_PAD_P`, `*_DISABLE_BULK` | `MTL_OPT_VIDEO_START_VRX`, `_PAD_INTERVAL`, `_STATIC_PAD_P`, `_DISABLE_BULK`, `MTL_OPT_CVIDEO_DISABLE_BULK` | 1 | R; RXTX 2 |
| U-163 | next frame due, is a frame late: `st_frame_is_late`, `st30_frame_is_late`, `st40_frame_is_late`; RxTxApp `st_app_user_time()` | `mtl_tx_next_slot` (`mtl_slot_hint`), `mtl_index_at`, `mtl_epoch_index_at`, `mtl_tx_row_deadline` | 3 (M3b) | Co; KT 1, RXTX |

### 2.9 Completion, notification and blocking

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-170 | blocking get with a timeout: `*P_*_FLAG_BLOCK_GET`, `*_set_block_timeout` | the `timeout_ns` argument of `mtl_tx_acquire`, `mtl_rx_dequeue` | 1 | C; SMP 4, RXTX, FF, GST, PY 3, MXL 2, KT |
| U-171 | wake a blocked getter: `*_wake_block` | `mtl_session_interrupt(s, 1)` (sticky `-MTL_ECANCELED`, cleared with 0) | 1 | C; SMP 4, RXTX, MXL 2, KT, ext NUDA9A |
| U-172 | non-blocking poll (get without BLOCK_GET → NULL) | timeout 0 → `-MTL_EAGAIN` (and the target is armed) | 1 | Co; GST st40p (1 ms poll) |
| U-173 | "frame available": `notify_frame_available` | `mtl_session_get_wait_handle` (eventfd, HANDLE), `mtl_session_wait` | 1 | Co; SMP 10, OBS 1, MXL 1, PLG 2, RS 1, KT 4 |
| U-174 | pipeline TX done with status: `notify_frame_done(priv, st_frame*)` | `MTL_SESSION_RESULTS` + `mtl_tx_reap` (`mtl_tx_result.status`) | 1 (M1) | Co; SMP 14, GST 1, MXL 1, KT 10 |
| U-175 | session TX done by index: `notify_frame_done(priv, idx, meta)` | `mtl_tx_result.slot`, `mtl_tx_reap`, `mtl_queue_reap` | 1 (M1) | Co; SMP, RXTX 5, RS 1; S! |
| U-176 | zero-hop callback on the tasklet | `mtl_session_set_inline_notify` (`mtl_queue.h`, LATER, decision M5); now `MTL_OPT_WAKER = MTL_WAKER_DIRECT` | LATER | R; MXL (bridge), ext:bobi (slice); S! (H-15) |
| U-177 | session event callback: `notify_event(priv, st_event, args)` | `mtl_session_read_events`, `mtl_queue_bind(…, MTL_BIND_EVENTS)`, `mtl_queue_read_events` | 1 (M3a); 2 (queues) | Co; RXTX 2, KT 2; t |
| U-178 | abort a got frame: `*_put_frame_abort` | `mtl_tx_release`, `mtl_rx_release` | 1 | Co; KT 1, UT 2 |

### 2.10 ST 2110-20 video TX

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-180 | pipeline TX loop: `st20p_tx_get_frame`, `st20p_tx_put_frame` | `mtl_tx_acquire` + `mtl_tx_submit`; `mtl_session_open` | 1 (M1) | C; SMP 9, FF, GST, OBS, MXL 2, PY 2, RS, KT 4 |
| U-181 | session pull model: `st20_tx_ops.get_next_frame` + `st20_tx_frame_meta` | push: acquire and submit (D-02); the meta becomes `struct mtl_unit` + `mtl_tx_next_slot` | 1 (M1) | Co; SMP 8, RXTX 1, RS 1, KT 12; S!. CHANGED |
| U-182 | session framebuffers by index: `st20_tx_get_framebuffer/_size/_count` | `mtl_session_get_slot`, `mtl_tx_acquire_slot`, `info.unit_bytes`, `info.pool_count` | 1 (M1); 4 (by index) | Co; SMP 4, RXTX 1, RS 1, KT 6; S! |
| U-183 | pipeline framebuffer address and size: `st20p_tx_get_fb_addr`, `st20p_tx_frame_size` | as U-182 | 1 | Co; SMP 4, FF, GST, MXL 2, PY 2, KT 2 |
| U-184 | session TX app frames per index: `ST20_TX_FLAG_EXT_FRAME`, `st20_tx_set_ext_frame`, `st20_ext_frame` | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` + `mtl_tx_acquire_slot` | 4 | Co; SMP 5, KT 3; S! |
| U-185 | pipeline TX app memory per frame (any address): `ST20P_TX_FLAG_EXT_FRAME`, `st20p_tx_put_ext_frame`, `st_ext_frame` | attached slots (≤ `info.max_count`); a new layout per acquire `mtl_tx_acquire_layout` (LATER) | 4 | Co; SMP 2, GST 1, KT 1. Regression R-5 (revision 3 had a dynamic pool); D-56, OI-20 |
| U-186 | two-phase release: `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE`, `st20p_tx_notify_ext_frame_free` | app memory always produces results; `mtl_tx_pin` | 4 | Co; SMP 2, GST 1, KT 1, UT 3 |
| U-187 | interlaced TX (`interlaced`, `second_field`) | `raster.scan = MTL_INTERLACED`: the unit is a field, parity from the media index | 1 | Co; many |
| U-188 | line padding: `linesize`, `transport_linesize` | `v.linesize[]` (library pools), `mtl_plane.stride`, `mtl_attach` strides | 1 (M2) | Co; SMP 7, GST 2, OBS 1, MXL 1, KT 4 |
| U-189 | split-forward TX tiles from an RX buffer (`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:127-131`) | `mtl_session_get_pool_region` + `mtl_session_attach` + `u.hold`, `mtl_tx_send_slot` (ex09) | 4 | R; SMP |
| U-190 | per-frame user metadata: `st20_tx_frame_meta.user_meta`, `st_frame.user_meta` | meta record `MTL_META_USER` (≤ 1332 B, `tag`, `tag_version`) | 1 | Co; SMP 7, RXTX 4, KT 1 |
| U-191 | slice TX: `ST20_TYPE_SLICE_LEVEL`, `query_frame_lines_ready`, `st20_tx_slice_meta` | `MTL_UNIT_ROWS`: resubmit the lease with a larger `u.used`; `mtl_tx_row_deadline`, `MTL_OPT_ROWS_LATE` | 6 | R; SMP 1, RXTX 1, KT 2, ext:bobi; S!. CHANGED: push, not a pull callback |
| U-192 | packing BPM / GPM / GPM_SL: `packing`, `transport_packing` | `v.packing` (`enum mtl_packing`, same values) | 1 | Co; SMP 2, RXTX 2, MXL 2, PY 2; t |
| U-193 | RFC 4175 transport formats (16 values): `enum st20_fmt` | `enum mtl_video_format` (all 16, legacy + 1) | 1 | Co; many; t |
| U-194 | non-RFC 4175 transport (planar 10LE, V210): `ST20_FMT_YUV_422_PLANAR10LE`, `ST20_FMT_V210` | `MTL_YUV422P10LE_NONSTD`, `MTL_V210_NONSTD` (`mtl_video_format_ext`), `MTL_INFO_NON_COMPLIANT` | 1 | L→kept; RXTX 2, KT 1; Q-MODE-7 |
| U-195 | resolution and frame rate: `width`, `height`, `enum st_fps` | `struct mtl_raster` (`rate` = `MTL_FPS_*`, or a rational `fps`), `mtl_fps_rational` (adds 47.95, 48) | 1 | C; all; t |
| U-196 | size, pixel group, bandwidth helpers: `st20_frame_size`, `st20_get_pgroup`, `st20_pgroup`, `st20_get_bandwidth_bps`, `st20_1080p59_yuv422_10bit_bandwidth_mps` | `mtl_format_frame_bytes`, `mtl_format_pgroup`, `mtl_video_bandwidth`, `mtl_session_query(…, req)` | 1 | R; SMP 6, RXTX 4, KT 8; t |
| U-197 | transport format names: `st20_fmt_name`, `st20_name_to_fmt` | `mtl_format_names`, `mtl_format_parse` | 1 | R; SMP 1, PY 1; t |

### 2.11 ST 2110-20 video RX

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-200 | pipeline RX loop: `st20p_rx_get_frame`, `st20p_rx_put_frame` | `mtl_rx_dequeue` + `mtl_rx_release` | 1 (M2) | C; SMP 12, FF 2, GST, OBS, MXL, PY 3, RS, KT 4 |
| U-201 | session push callback, return later or reject: `notify_frame_ready` + `st20_rx_put_framebuff` | `mtl_rx_dequeue` / `mtl_rx_release` (any thread, any order) | 1 (M2) | Co; SMP 5, RXTX 4, MXL 4, RS 1, KT 11; S!. CHANGED |
| U-202 | deliver incomplete frames: `*_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | `MTL_OPT_RX_INCOMPLETE`: DELIVER is the default (M13), `MTL_RX_DISCARD` opt-in | 1 (M2) | Co; SMP 1, RXTX 1, MXL 2, KT 6, ext:bobi. CHANGED, wire-visible default |
| U-203 | frame status: `enum st_frame_status`, `st_is_frame_complete` | `u.status` (`MTL_RX_COMPLETE/INCOMPLETE`), `MTL_UNITF_USED_REDUNDANCY` (= RECONSTRUCTED) | 1 (M2) | C; SMP 5, RXTX 2, MXL 2, KT 4; t |
| U-204 | packets per leg: `pkts_total`, `pkts_recv[]` | `mtl_rx_get_detail` (`pkts_expected`, `pkts_received[]`, `pkts_recovered`), `mtl_rx_get_missing` | 1 (M2) | Co; KT, RXTX |
| U-205 | first/last packet arrival: `timestamp_first_pkt`, `timestamp_last_pkt`, `receive_timestamp` | `mtl_rx_detail.arrival_first_tai_ns[]`, `arrival_last_tai_ns[]` | 1 (M2) | Co; RXTX 4, GST, OBS |
| U-206 | RX RTP and media time: `rtp_timestamp`, `timestamp` + `tfmt` | `u.rtp`, `u.media_tai_ns`, `u.media_index` (`MTL_UNITF_*_VALID`) | 1 (M2; index M3b) | C; GST, OBS, RXTX |
| U-207 | bytes per frame: `frame_total_size`, `frame_recv_size`, `uframe_total_size` | `mtl_rx_detail.bytes_received` | 1 (M2) | R; KT |
| U-208 | RX into fixed app buffers: `st20_rx_ops.ext_frames[]`, `st20p_rx_ops.ext_frames` | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` | 4 | Co; SMP 4, KT 6 |
| U-209 | RX into an app buffer chosen per frame: `query_ext_frame` (st20, st20p, st22p) + `*_RX_FLAG_EXT_FRAME` | `MTL_SESSION_RX_BY_INDEX` now; `mtl_rx_provide` (LATER) | 4 | Co; SMP 1, GST 1, MXL 3, KT 3 (H-09) |
| U-210 | per-buffer identity: `st20_ext_frame.opaque`, `st_frame.opaque` | `u.slot` (stable index, D-76) | 1 | Co; GST, MXL. Regression R-8, accepted |
| U-211 | RX DMA offload: `ST20(P)_RX_FLAG_DMA_OFFLOAD`, `st20_rx_dma_enabled` | `MTL_OPT_DMA`, `MTL_OPT_DMA_DEVICES`, `MTL_PATH_DIRECT_DMA`, `rx.pkts_dma` | 1 (M2) | Co; RXTX 1, FF 1 (flag misused, `mtl_st20p_rx.c:176`), GST 2, KT 5 |
| U-212 | auto-detect raster, fps, packing, interlace: `*_RX_FLAG_AUTO_DETECT`, `notify_detected`, `st20_detect_meta/reply` | `v.detect = MTL_DETECT_ON`, `MTL_EVENT_RX_FORMAT`, keys `rx.detected.*` | 1 (M3a) | R; SMP 1, RXTX 1, KT 1; t (H-06) |
| U-213 | header split: `*_RX_FLAG_HDR_SPLIT`, `nb_rx_hdr_split_queues` | none (CUT-1) | M12 | L; SMP 1, RXTX 1, KT 1; needs a DPDK patch absent for 26.07; single port, BPM only |
| U-214 | ST 2110-21 timing parser in stats: `*_TIMING_PARSER_STAT` | `MTL_OPT_RX_TIMING_PARSER`, keys `tp.*` | 1 (M2) | R; RXTX 1 |
| U-215 | timing result per frame and port: `ST20(P)_RX_FLAG_TIMING_PARSER_META`, `st20_rx_tp_meta`, `st_frame_tp_meta` | `mtl_rx_get_timing(s, lease, leg, …)`: compliance, `failed_cause`, cinst, vrx, `fpt_ns`, latency, RTP offset and delta, ipt | 1 (M3a or later) | R; SMP 1, PY 1, KT 2, UT 1, ext:bobi; t (H-07) |
| U-216 | pass thresholds: `st20(p)_rx_timing_parser_critical`, `st20_rx_tp_pass` | keys `tp.pass.*` | 1 | R; SMP 1; t |
| U-217 | two RX threads above 40 Gb/s: `*_RX_FLAG_USE_MULTI_THREADS` | `MTL_OPT_RX_THREADS` (2 with two legs or rows: `-MTL_ENOTSUP`, OI-27) | 1 (M2) | R; RXTX 1 |
| U-218 | slice RX: `ST20_TYPE_SLICE_LEVEL`, `slice_lines`, `notify_slice_ready`, `st20_rx_slice_meta` | dequeue at the first rows with `MTL_UNITF_PARTIAL`, then `mtl_rx_wait_rows`; step `MTL_OPT_RX_ROWS_STEP` | 6 | R; SMP 1, RXTX 1, KT 3, ext:bobi; S!. CHANGED; regression R-1, fixed |
| U-219 | app converts pixel groups on the tasklet: `uframe_size`, `uframe_pg_callback`, `st20_rx_uframe_pg_meta` | none (CUT-2: app code on a tasklet) | M12 | L; RXTX 1, KT 3; S! |
| U-220 | per-packet conversion in st20p: `ST20P_RX_FLAG_PKT_CONVERT` (3 formats) | `MTL_OPT_RX_CONVERT_PER_PACKET` (library code on the RX tasklet, OI-28) | 1 (M2) | L→kept; KT 1 (H-16) |
| U-221 | RX user metadata: `st20_rx_frame_meta.user_meta` | meta record `MTL_META_USER` | 1 (M2) | Co; SMP 7, RXTX 4 |
| U-222 | first-packet time per frame: `st20_rx_frame_meta.fpt` | `mtl_rx_timing_result.fpt_ns` (parser on); else `arrival_first_tai_ns − media_tai_ns` | 1 | R; none |
| U-223 | simulated RX loss: `*_RX_FLAG_SIMULATE_PKT_LOSS`, `burst_loss_max`, `sim_loss_rate` | `mtl_debug_inject` with `MTL_FAULT_DROP_PKTS` (pattern) or `MTL_FAULT_DROP_RANDOM` (rate) | debug (M2) | R; KT 4 |
| U-224 | RX framebuffer size and count: `st20_rx_get_framebuffer_size/_count`, `st20p_rx_get_fb_addr`, `st20p_rx_frame_size` | `info.unit_bytes`, `info.pool_count`, `mtl_session_get_slot` | 1 | Co; SMP 4, RXTX 1, FF, GST, RS 1 |

### 2.12 ST 2110-22 compressed video

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-230 | pipeline TX through a codec plugin: `st22p_tx_ops` (`input_fmt`, `codec`, `quality`, `codestream_size`, `codec_thread_cnt`, `device`) | `sc.essence = MTL_CVIDEO`: `c.codec`, `c.app_format`, `c.codestream_bytes`; `MTL_OPT_CVIDEO_QUALITY`, `_THREADS`, `_DEVICE` | 2 | Co; SMP 2, RXTX 1, FF 1, PY 1, RS 1, KT 1 |
| U-231 | pipeline RX decoded: `st22p_rx_ops` (`output_fmt`, `max_codestream_size`) | `struct mtl_cvideo_config` (RX) | 2 | Co; SMP 1, RXTX 1, FF 1, PY 1, RS 1, KT 1 |
| U-232 | codestream passthrough: st22p with a codestream format; `framebuff_max_size`, `st22_tx_frame_meta.codestream_size` | `c.app_format = 0`, `u.used` (bytes) | 2 | Co; SMP 3, RXTX 2, MXL 2, KT 2 |
| U-233 | constant vs variable bytes per frame (today VBR-like) | `c.rate_mode`: `MTL_CVIDEO_CBR` default (padding), `MTL_CVIDEO_VBR_MAX` = today (non-compliant) | 2 | Co; every ST22 user. CHANGED, wire-visible (M13, D-32) |
| U-234 | JPEG-XS box headers off: `ST22(P)_TX/RX_FLAG_DISABLE_BOXES` | none (CUT-6); `info.box_hdr_bytes` stays reported | M12 | R; no consumer (st22p forces it for non-JPEG-XS codecs) |
| U-235 | packetization mode: `pack_type` (SLICE unsupported) | `MTL_OPT_CVIDEO_PACK` (`enum mtl_cvideo_pack`) | 2 | Co; FF 1; t. Regression R-6, fixed |
| U-236 | session-level ST22: `st22_tx/rx_ops`, `st22_tx_get_fb_addr`, `st22_rx_get_fb_addr`, `st22_rx_put_framebuff` | as U-181, U-201 | 2 | R; SMP 1, RXTX 1, MXL 1, KT 1; S!. CHANGED |
| U-237 | interlaced ST22 (a codestream per field) | `c.raster.scan` (CBR per field) | 2 | R; RXTX |
| U-238 | ST22 pipeline app memory TX / RX: `ST22P_TX_FLAG_EXT_FRAME` + `st22p_tx_put_ext_frame`; `ST22P_RX_FLAG_EXT_FRAME` + `query_ext_frame` | as U-185, U-209 (LATER parts) | 4 | R; KT 1 |
| U-239 | deliver incomplete ST22 frames | `MTL_OPT_RX_INCOMPLETE` | 2 | R; none |

### 2.13 ST 2110-30/31 audio

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-240 | PCM8/16/24 and AM824: `enum st30_fmt` | `enum mtl_audio_format` | 2 | C; FF, GST, RXTX, KT; t |
| U-241 | 48 / 96 / 44.1 kHz: `enum st30_sampling` | `a.sample_rate` (Hz) | 2 | C; all audio; t |
| U-242 | channel count: `channel` | `a.channels` | 2 | C; all audio |
| U-243 | packet time incl. ST 2110-31 values (1 ms, 125/250/333 µs, 4 ms, 80 µs, 1.09/0.14/0.09 ms): `enum st30_ptime` | `a.ptime` (`enum mtl_ptime`, legacy + 1; 0 = 1 ms) | 2 | C; all audio; t. The check had a samples-per-packet field; the header has `ptime`. Non-integer samples (80 µs at 48 kHz): OI-32 |
| U-244 | pipeline get/put: `st30p_tx/rx_get_frame`, `put_frame`, `struct st30_frame` | `mtl_tx_acquire`/`mtl_tx_submit` (`u.used` bytes), `mtl_rx_dequeue`, `mtl_tx_write` | 2 | C; SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 2 |
| U-245 | buffer size for a frame duration: `st30_calculate_framebuff_size`, `framebuff_size` | `a.unit_samples` (0 = 10 ms); `info.buffer_capacity_bytes`, `info.unit_samples` from `mtl_session_query` | 2 | Co; SMP 2, RXTX 2, FF 2, GST 2, RS 1, KT 2; t |
| U-246 | size/time math: `st30_get_packet_size`, `_sample_size`, `_sample_num`, `_sample_rate`, `_packet_time` | `mtl_audio_bytes`, `mtl_session_query` | 2 | Co; RXTX 2, KT 5, UT 1; t |
| U-247 | per-session pacing engine AUTO/RL/TSC: `pacing_way` | `MTL_OPT_PACING` | 2 | Co; RXTX 1, FF 2, PY 7, KT 1; t |
| U-248 | RL warm-up: `rl_accuracy_ns`, `rl_offset_ns` | `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` | 2 | R; RXTX 1 |
| U-249 | pace in the builder too: `ST30_TX_FLAG_BUILD_PACING` | `MTL_OPT_AUDIO_BUILD_PACING` | 2 | R; RXTX 1; S! |
| U-250 | builder-to-pacer FIFO: `fifo_size` (packets) | `MTL_OPT_AUDIO_FIFO_MS` (ms, default 10) | 2 | R; RXTX 1. CHANGED: unit |
| U-251 | audio user timestamp: `ST30_TX_FLAG_USER_TIMESTAMP` (no st30p flag) | media mode TAI or INDEX + `MTL_SUBMIT_RTP_TS`; `MTL_OPT_AUDIO_ABSORB_SAMPLES` | 3 | Co; KT 1, UT 3; S! |
| U-252 | audio user pacing: `ST30(P)_TX_FLAG_USER_PACING` | `MTL_MEDIA_TAI` (+ `MTL_SUBMIT_NOT_BEFORE` from Phase 3) | 3 | Co; RXTX 1, GST 1, KT 3 |
| U-253 | lost packets read as silence: `ST30(P)_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | `MTL_OPT_RX_INCOMPLETE`; zero fill of library pools by default (`MTL_SESSION_RX_NO_FILL` opts out) | 2 | R; KT 1, UT 1 |
| U-254 | audio timing parser (dpvr, ipt, tsdf per 200 ms): `ST30_RX_FLAG_TIMING_PARSER_STAT/META`, `notify_timing_parser_result`, `st30_rx_tp_meta` | `MTL_OPT_RX_TIMING_PARSER`; per unit `mtl_rx_get_timing` (`dpvr_max_ns`, `ipt_max_ns`, `tsdf_ns`); keys `tp.*` | 2 | R; RXTX 1; S!. CHANGED: per unit, not a 200 ms callback |
| U-255 | AM824 / AES3 subframe layouts: `struct st31_am824`, `st31_aes3` (+ convert `st31_*`) | `struct mtl_am824_subframe`, `mtl_convert_am824_to_aes3`, `_aes3_to_am824` | 2 | R; KT; t |
| U-256 | deprecated `sample_size`, `sample_num` | none ("Not use anymore, plan to remove") | M12 | L; none |
| U-257 | audio RX arrival time: `timestamp_first_pkt`, `receive_timestamp` | `mtl_rx_detail.arrival_first_tai_ns[]` | 2 | Co; RXTX |
| U-258 | session-level audio: `st30_tx_ops.get_next_frame`, `st30_tx_get_framebuffer`, `st30_rx_ops.notify_frame_ready`, `st30_rx_put_framebuff` | as U-181, U-201 | 2 | R; RXTX 4, KT 2; S!. CHANGED |

### 2.14 ST 2110-40 ancillary

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-260 | ANC TX/RX with a packet table + UDW: `st40p_*`, `st40_frame_info`, `struct st40_meta` | `MTL_ANC`: meta record `MTL_META_ANC` (`struct mtl_anc_packet`) + UDW in plane 0 | 2 | Co; SMP 1, RXTX 1, GST 1, KT 3; t. `stream` byte: bit 7 = S, bits 0–6 = StreamNum |
| U-261 | packets per frame: `ST40_MAX_META = 20` (excess truncated) | `MTL_OPT_ANC_MAX_PACKETS` (≤ 255, default 255) | 2 | Co; all ANC; t. CHANGED |
| U-262 | UDW capacity: `max_udw_buff_size`, `framebuff_size`, `st40p_*_max_udw_buff_size`, `get_udw_buff_addr` | `n.max_udw_bytes` (0 = 64 KiB), `info.buffer_capacity_bytes` | 2 | Co; SMP 2, RXTX 2, GST 2, KT 2 |
| U-263 | one ANC packet per RTP packet: `ST40(P)_TX_FLAG_SPLIT_ANC_BY_PKT` | `MTL_OPT_ANC_SPLIT_BY_PACKET` | 2 | Co; SMP 1, GST 2, KT 1 |
| U-264 | RX interlace auto-detect and its off switch: `ST40(P)_RX_FLAG_DISABLE_AUTO_DETECT` | `MTL_OPT_ANC_RX_DETECT` (default ON) | 2 | Co; GST 1. Video detect is a field, ANC detect an option (I-9 residual) |
| U-265 | interlaced ANC TX (`second_field`) | `n.video.scan` + index parity | 2 | Co; RXTX, KT |
| U-266 | per-port sequence loss and discontinuity, marker: `port_seq_lost[]`, `port_seq_discont[]`, `seq_lost`, `seq_discont`, `rtp_marker` | `mtl_rx_detail.marker_seen`, `seq_discont[leg]`, `pkts_received[leg]`, `units_missing_before`; key `leg.pkts_lost` | 2 | R; KT, GST |
| U-267 | TX test mutations: `st40_tx_test_config` in `st40_tx_ops` / `st40p_tx_ops` | `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` + `enum mtl_tx_mutation` | debug | R; GST (test element), KT; t |
| U-268 | RFC 8331 helpers: `st40_get_udw`, `st40_set_udw`, `st40_calc_checksum`, `st40_add/check_parity_bits`, `st40_rfc8331_*` | `mtl_anc_udw_get/set`, `mtl_anc_parity`, `mtl_anc_parity_ok`, `mtl_anc_checksum`, `mtl_anc_rfc8331_encode/decode` (`mtl_util.h`) | 2 | Co; RXTX 2, GST 1, KT 2, UT 3; S! (H-13) |
| U-269 | session-level ANC: `st40_tx_ops.get_next_frame` + `st40_frame`, `st40_rx_ops.notify_frame_ready`, `st40_rx_put_framebuff` | as U-181, U-201 | 2 | R; RXTX 1, KT 2, UT 5; S!. CHANGED |
| U-270 | ANC user pacing, timestamp, exact: `ST40(P)_TX_FLAG_USER_PACING/USER_TIMESTAMP/EXACT_USER_PACING` | as U-151 … U-153 | 3 | R; RXTX 1, GST 1, KT 2 |
| U-271 | `ST40P_*_FLAG_FORCE_NUMA` ("NOT SUPPORTED YET", `st40_pipeline_api.h:117-121`) | none | M12 | L; none |
| U-272 | `st40p_rx_ops.rtp_ring_size` (documented mandatory, unused) | none | M12 | L; none |

### 2.15 ST 2110-41 fast metadata (no pipeline API today)

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-280 | TX one data item per frame: `st41_tx_ops.get_next_frame`, `st41_frame`, `st41_tx_get_framebuffer` | `sc.essence = MTL_FASTMETA`, `u.used` | 2 | R; RXTX 1; S! |
| U-281 | data item type and K bit: `fmd_dit`, `fmd_k_bit` | `f.data_item_type`, `f.k_bit` | 2 | R; RXTX 3; S! |
| U-282 | RX filter on DIT / K (`0xffffffff` / `0xff` = off) | `MTL_FASTMETA_RX_MATCH_DIT`, `_MATCH_K` | 2 | R; RXTX 3; S!. Today the filter cannot change at runtime: `st41_rx_update_source` takes `st_rx_source_info` (`include/st_api.h:159`), which has no filter field; unified, it changes with `MTL_UPDATE_MEDIA` in CREATED or STOPPED |
| U-283 | RX (RTP only today): `st41_rx_ops.notify_rtp_ready` + `st41_rx_get_mbuf` | frame-level RX is new; the raw path is U-345 | 2 | R; RXTX 1; S!. CHANGED |
| U-284 | user pacing, timestamp: `ST41_TX_FLAG_USER_PACING/USER_TIMESTAMP` | `sc.media_mode`, `MTL_SUBMIT_*` | 3 | R; none; S! |
| U-285 | rate and interlace: `fps`, `interlaced` | `f.video` (raster), `MTL_FASTMETA_FREE_RUNNING` | 2 | R; RXTX; S! |
| U-286 | stats, update, queue meta: `st41_*_get/reset_session_stats`, `st41_*_update_*`, `st41_rx_get_queue_meta` | stats keys, `mtl_session_update`; queue meta: U-139 | 2 | R; none; S! |

### 2.16 RTP passthrough (app-built packets), common by maintainer requirement

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-340 | TX app-built RTP video (MTL adds L2–L4, paces): `ST20_TYPE_RTP_LEVEL`, `st20_tx_get_mbuf/put_mbuf`, `rtp_ring_size`, `rtp_frame_total_pkts`, `rtp_pkt_size`, `notify_rtp_done` | `unit = MTL_UNIT_PACKETS`, `mtl_packet_config` (`packets_per_chunk`, `slot_bytes`, `set_fields`, `pacing`), `mtl_pkt_tx`, `MTL_SUBMIT_UNIT_END` | 2P | Co; SMP 1, RXTX 1, KT 2, UT 1; S! (H-01) |
| U-341 | RX raw RTP video (2022-7 de-duplicated): `st20_rx_get_mbuf/put_mbuf`, `notify_rtp_ready` | `mtl_rx_dequeue` of a chunk, `struct mtl_pkt_rx`, `MTL_PKT_RX_NO_DEDUP`, `sc.packet.rx_ring_packets` | 2P | Co; SMP 1, RXTX 1, KT 1; S! |
| U-342 | ST22 RTP (exact packet count per frame): `ST22_TYPE_RTP_LEVEL`, `st22_tx/rx_get_mbuf/put_mbuf` | `MTL_CVIDEO` + `MTL_UNIT_PACKETS`, `packets_per_unit` | 2P | Co; KT 1; S! |
| U-343 | ST30 RTP: `ST30_TYPE_RTP_LEVEL`, `st30_tx/rx_get_mbuf/put_mbuf` | `MTL_AUDIO` + `MTL_UNIT_PACKETS` | 2P | Co; RXTX 1, KT 1; S! |
| U-344 | ST40 RTP: `ST40_TYPE_RTP_LEVEL`, `st40_tx/rx_get_mbuf/put_mbuf` | `MTL_ANC` + `MTL_UNIT_PACKETS` | 2P | Co; RXTX 1, KT 1; S! |
| U-345 | ST41 RTP: `ST41_TYPE_RTP_LEVEL`, `st41_tx/rx_get_mbuf/put_mbuf` | `MTL_FASTMETA` + `MTL_UNIT_PACKETS` | 2P | Co; RXTX 1; S! |
| U-346 | ST 2022-6 and custom payloads through RTP level (`doc/design.md:326-330`) | `sc.essence = MTL_RTP`, `struct mtl_rtp_config` (`clock_rate`, `profile`, `encoding`) | 2P | Co; external, not in tree; S! |
| U-347 | RTP and payload headers: `st_rfc3550_rtp_hdr`, `st20_rfc4175_rtp_hdr`, `st20_rfc4175_extra_rtp_hdr`, `ST20_SRD_OFFSET_CONTINUATION`, `ST20_SECOND_FIELD`, `ST20_RETRANSMIT`, `st22_rfc9134_rtp_hdr`, `st40_rfc8331_*_hdr`, `st41_rtp_hdr` | `mtl_rtp_hdr`, `mtl_rfc4175_hdr`, `mtl_rfc4175_srd`, `mtl_rfc9134_hdr`, `mtl_rfc8331_hdr`, `mtl_st41_hdr` | 2P | Co; SMP, RXTX, KT, GST; S! |
| U-348 | pixel-group bit layouts: `st20_rfc4175_422_10_pg2_be` … `st20_rfc4175_444_12_pg2_le` | `mtl_format_pgroup` gives bytes and pixels per group only | 1 | R; SMP (perf), KT; t. Open: OI-42 recommends a home in `mtl_format.h` |
| U-349 | packet size limits: `MTL_PKT_MAX_RTP_BYTES`, `MTL_UDP_MAX_BYTES`, `MTL_MTU_MAX_BYTES` | `MTL_UDP_HDR_BYTES`, `MTL_IPV4_HDR_BYTES` + the rule "MTU minus IP and UDP"; `MTL_OPT_MAX_UDP_PAYLOAD`, `MTL_REASON_PKT_CONFIG` | 2P | R; RXTX, KT. CHANGED: no fixed constant |

### 2.17 Pipeline conversion, formats and frame helpers

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-360 | convert app layout and transport format in the library: st20p `input_fmt` / `output_fmt` vs `transport_fmt` | `v.app_format` + `v.format`, `mtl_format_convertible`, `info.direct`, `MTL_TXR_COPIED`, key `info.convert_context` | 1 (M1, M2) | C; FF, GST, OBS, RXTX, PY, RS. Today conversion runs in the app thread inside `put_frame`/`get_frame` |
| U-361 | app frame formats: `enum st_frame_fmt` | `enum mtl_app_format` (every legacy raw format + NV12, RGBA); codestreams through `enum mtl_codec` | 1 | Co; many |
| U-362 | non-compliant 8-bit passthrough: `ST_FRAME_FMT_YUV420CUSTOM8`, `_YUV422CUSTOM8` | `MTL_APP_YUV420_CUSTOM8`, `MTL_APP_YUV422_CUSTOM8` (`MTL_INFO_NON_COMPLIANT`) | 1 | Co; FF 2, GST 1, OBS 1, RXTX 1, RS 1, SMP 1 |
| U-363 | zero-copy (derive) when formats match | `MTL_SESSION_REQUIRE_DIRECT`, `info.direct`, `MTL_PATH_DIRECT` | 1 | Co; GST, KT |
| U-364 | converter device for st20p: `st20p_*_ops.device` (`enum st_plugin_device`) | `MTL_OPT_VIDEO_CONVERT_DEVICE` (`MTL_CODEC_DEVICE_*`) | 1 | R; RXTX, KT |
| U-365 | `struct st_frame`: planes, strides, size, format, timestamps, status | `struct mtl_unit` + `mtl_tx_result` / `mtl_rx_detail` | 1 | C; every pipeline user |
| U-366 | buffer requirements before create: `st_frame_size`, `*_frame_size` | `mtl_session_query(…, info, req)`, `mtl_format_frame_bytes` | 1 | Co; SMP 8, KT 4 |
| U-367 | standalone frame allocation: `st_frame_create/free`, `st_frame_create_by_malloc` | `mtl_mem_alloc` + `mtl_session_attach`, or app memory + `mtl_format_frame_bytes` for `mtl_convert` | 4 | R; KT 1, PY 2. CHANGED |
| U-368 | format helpers: `st_frame_fmt_*`, `st_frame_name_to_fmt`, `st_frame_least_linesize`, `st_frame_plane_size`, `st_frame_data_height`, `st_frame_sanity_check` | `mtl_format_plane`, `mtl_format_names`, `mtl_format_frame_bytes`, `mtl_format_parse` | 1 | Co; SMP, FF, GST, OBS, PLG 3, PY, KT 4 |
| U-369 | software frame operations: `st_frame_convert`, `st_frame_downsample`, `st_draw_logo`, `st_field_merge`, `st_field_split` | `mtl_convert` with `MTL_CONVERT_FIELD_SPLIT`, `_FIELD_MERGE`, `_HALF_SCALE`; `st_draw_logo` is CUT-8 | 2 | R; SMP 4, PY 1, KT 1 |
| U-370 | codec name and enum: `st_name_to_codec`, `enum st22_codec` | `enum mtl_codec`, `mtl_codec_parse`, `mtl_codec_name` | 1 | R; SMP 1, FF 2, PY 1 |
| U-371 | SWIG helpers: `st_frame_addr_cpuva`, `st_frame_addr`, `st_rxp/st_txp_para_*` | POD structs, `MTL_ADDR(T)`, `mtl_flow_ipv4` | 1 | Co; PY 8. CHANGED; no spec strings or parsers (D-97) |

### 2.18 Codec and converter plugins

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-380 | register an ST22 encoder or decoder in-process: `st22_encoder_register/unregister`, `st22_decoder_register/unregister`, `st22_encoder_dev`, `st22_decoder_dev`, `*_create_req` | `mtl_plugin_register` + `struct mtl_plugin_device` (`MTL_PLUGIN_ENCODER`, `_DECODER`) | 2 | Co; PLG 2, KT 1 (H-19) |
| U-381 | encoder/decoder worker loop: `st22_encoder/decoder_get_frame`, `put_frame`, `wake_block`, `set_block_timeout`, `*_RESP_FLAG_BLOCK_GET`, `st22_encode/decode_frame_meta` | `mtl_plugin_host.get_work(…, timeout)` / `put_work` (ABI v2) | 2 | Co; PLG 2, KT 1. CHANGED |
| U-382 | st20p format converter: `st20_converter_register/unregister/get_frame/put_frame`, `st20_converter_dev` | `MTL_PLUGIN_CONVERTER` | 2 | R; PLG 1, KT 1 |
| U-383 | load plugins from `.so` (JSON list at init): `st_plugin_register/unregister`, `st_get_plugins_nb`, `ST_PLUGIN_*_API`, `st_plugin_meta`, `ST_PLUGIN_VERSION_V1_MAGIC` | `mtl_plugin_load`, `MTL_PLUGIN_ENTRY_SYMBOL`, `mtl_plugin_unload`; no list at open | 2 | Co; KT 2; `plugins/st22_avcodec`, `plugins/sample`. CHANGED |
| U-384 | format capability masks: `ST_FMT_CAP_*` | `struct mtl_plugin_format_pair` lists | 2 | Co; PLG. CHANGED: no 64-value limit |
| U-385 | test-only plugin devices: `ST_PLUGIN_DEVICE_TEST`, `_TEST_INTERNAL` | `MTL_CODEC_DEVICE_TEST`, `MTL_CODEC_DEVICE_TEST_INTERNAL` | 2 | R; KT |

### 2.19 Time, rate and conversion helpers

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-390 | frame rate enum, number and name: `st_frame_rate`, `st_frame_rate_to_st_fps`, `st_name_to_fps` | `enum mtl_fps`, `mtl_fps_rational`; no name parser (D-97) | 1 | C; RXTX 11, KT 18, FF 1, GST 3, PY 1; t |
| U-391 | TAI ↔ media clock and unwrap: `st10_tai_to_media_clk`, `st10_media_clk_to_ns`, `st10_media_clk_to_tai`, `st10_get_tai`, `st10_get_media_clk`, `enum st10_timestamp_fmt` | `mtl_media_ticks`, `mtl_media_tai` (`mtl_sync.h`) | 1 | Co; RXTX 4, KT 6, UT 8; t (H-12) |
| U-392 | sampling-rate constants: `ST10_VIDEO_SAMPLING_RATE_90K`, `ST10_AUDIO_SAMPLING_RATE_*` | implicit (Hz numbers) | 1 | R; RXTX, KT; t |
| U-393 | standalone colour conversion library (RFC 4175 ↔ planar / V210 / Y210 / LE, AM824 ↔ AES3; SIMD and DMA variants): `st_convert_api.h` (47), `st_convert_internal.h` (58) | `mtl_convert(&desc)` (one call, optional DMA), `mtl_convert_am824_to_aes3`; per-pair functions internal (CUT-4) | 2 | Co; SMP (perf, tools), PLG 1, PY, KT (70 functions in `cvt_test.cpp`). CHANGED |

### 2.20 Stats and observability

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-400 | session stats per port and family: `st*_get_session_stats`, `st_tx/rx_user_stats`, `st_tx/rx_port_stats`, `st20/30/40/41_*_user_stats` | `mtl_stat_list/find/read/get`, keys `tx.*`, `rx.*`, `leg.*` (one schema; ST22 gains stats) | 1 (M3a); 2 | Co; RXTX 1, KT 3, UT 3, ext NUDA9A; t |
| U-401 | reset session stats: `st*_reset_session_stats` | none: cumulative (D-80) | 1 | Co; RXTX 1, UT 2. CHANGED |
| U-402 | family-specific counters (no slot, wrong interlace, burst sizes, …): `st20_rx_user_stats` (~35 fields) | `rx.pkts_rejected{cause}` and the map of [migration.md](migration.md) §4.14 | 1, 2 | R; RXTX, KT; t. Open: about 20 fields without a key, OI-40 |

### 2.21 Experimental and legacy remnants

| U-ID | Legacy | Unified home | Phase | Notes |
|---|---|---|---|---|
| U-410 | redundant-combined ST20 RX: `st20rc_rx_*`, `ST20RC_RX_FLAG_*` | a two-leg RX session; the API itself is CUT-3 | M12 | L; SMP 1, RXTX 1; Q-MODE-7 |
| U-411 | deprecated `sip_addr` alias of RX `ip_addr` (`st*_rx_ops`, `st_rx_port`, `st_rx_source_info`) | none | M12 | L; none |
| U-412 | reserved RX `pacing` / `packing` fields of `st20_rx_ops` ("not in use") | none | M12 | L; none |
| U-413 | ST22 slice packetization `ST22_PACK_SLICE` ("not support now") | `MTL_OPT_CVIDEO_PACK = MTL_CVIDEO_PACK_SLICE` ("not yet", `-MTL_ENOTSUP`) | 2 | L; none. Part of R-6, fixed |
| U-414 | legacy callback guard pattern (`if (!ctx->handle)`: callbacks may fire before create returns) | no callbacks; the handle exists before start | — | note, not counted; KT 37 sites, OBS |
| U-415 | `MTL_FLAG_BIND_NUMA` set by apps (`mtl_api.h:340`, no effect) | none (see U-062) | M12 | L; SMP 2, FF, OBS 2, PY 7, RS 6 |
| U-416 | plain `mtl_udma_fill` (u64 pattern) | none (U-116) | M12 | L; none |
| U-417 | raw `st20_rx_ops.gpu_direct_framebuffer_in_vram_device_address` | none (U-117 for the function) | M12 | L; none |
| U-418 | fb address getters with no users: `st20p_rx_get_fb_addr`, `st22p_rx_get_fb_addr`, `st22_rx_get_fb_addr`, `st40p_tx/rx_get_udw_buff_addr`, `st40p_tx_get_fb_addr` | `mtl_session_get_slot` | 1 | R; none |

## 3. The coverage check of revision 4

The check read the revision-4 headers row by row against §2, against the session-only rows of §4
and against the pre-hide gaps H-01…H-19; it is decision D-87's gate (every legacy capability has
a home, a later phase, or an approved cut; awaiting M12). The rule: a feature described only in
a comment or in prose, with no symbol to request it, is partial at best; option keys and stats
keys count as symbols once the key catalogue (contract.md §11.3) names them.

### 3.1 Counts

| Status | Revision 3 (first inventory) | Revision 4, first check | After the fixes | Now |
|---|---|---|---|---|
| COVERED (incl. debug) | 138 (3 debug) | 186 | 212 | 214 |
| CHANGED | 24 | 43 | 44 | 44 |
| PARTIAL | 34 | 24 | 5 | 3 (U-115, U-348, U-402) |
| LATER (`MTL_LATER`) | in NOT COVERED | 5 | 5 | 5 (U-117, U-176, U-185, U-209, U-238) |
| NOT COVERED | 93 | 8 | 0 | 0 |
| CUT (M12) | not used | 24 | 24 | 24 |
| Session-only rows of §4 (V1…X4, 74) | 34 / 11 partial / 6 later / 5 not / 12 cut / 5 packet / 1 internal | 43 / 2 / 11 / 3 / 1 / 14 | 54 / 2 / 1 / 3 / 0 / 14 | as after the fixes; M8's partial part (device abort) closed |
| Pre-hide gaps H-01…H-19 | — | 10 / 0 / 4 / 3 / 0 / 2 | 14 / 0 / 0 / 3 / 0 / 2 | as after the fixes |

Revision-4 columns read COVERED / CHANGED / PARTIAL / LATER / NOT COVERED / CUT. The first
inventory classed the 290 rows as 33 core, 113 common, 122 rare and 22 legacy-only; 31 rows
were `S!` and 38 `t`. Its largest uncovered clusters were RTP passthrough (U-340…U-349), the
instance and port tuning of `mtl_init_params` and `MTL_FLAG_*` (U-014…U-031, U-041…U-044,
U-057…U-086), the plugin ABI (U-380…U-385) and the conversion library (U-393), lcore borrowing
and user tasklets (U-067…U-069), IOVA and user DMA (U-111, U-116) and log routing
(U-019…U-021); revision 4 closed all of them or put them on the M12 list. "Now" counts U-006
(`mtl_instance_abort`) and U-111 (`mtl_mem_info.va` + `mtl_mem_iova`), closed in the headers
after the check.

Not regressions, although the status word changed: U-176, U-209, U-238 are under `MTL_LATER`
again, as in revision 3's later block; U-117 moved from a reserved memory kind to
`mtl_mem_import_device`; U-022 and U-115 were rated more strictly; U-174 lost revision 3's
EXCEPTIONS completion mode (M11), which had no legacy counterpart; U-153 legacy `USER_TIMESTAMP`
without `USER_PACING` needs INDEX mode with `mtl_tx_next_slot` (`MTL_SUBMIT_RTP_TS` in AUTO is
`MTL_REASON_RTP_TS_AUTO`), one sentence the migration table carries.

### 3.2 Regressions R-1…R-10 (rows revision 3 covered better)

| # | Rows | What revision 4 lost | Outcome |
|---|---|---|---|
| R-1 | U-218; V5; H-02 (RX) | no wait for more rows of a held unit, no row step | fixed: `mtl_rx_wait_rows` (`mtl_sync.h`), `MTL_OPT_RX_ROWS_STEP`, `MTL_UNITF_PARTIAL` |
| R-2 | U-095 | the DPDK AF_XDP and AF_PACKET backends had enum values | open, M12; the `mtl_port_spec.name` comment lists them for removal |
| R-3 | U-096; M8 | `enum mtl_backend` defined nowhere | fixed: `enum mtl_backend` in `mtl_observe.h`, key `caps.backend` |
| R-4 | 19 rows that depend on stats key names: U-004, U-023, U-033, U-035, U-046, U-061, U-096, U-097, U-099, U-114, U-161, U-212, U-214, U-216, U-254, U-266, U-286, U-400, U-402 | the key names existed only in a study, not in the normative set | fixed: the catalogue is contract.md §11.3 |
| R-5 | U-185, U-238; V6 | per-acquire layouts moved to `MTL_LATER` | accepted as Phase 4 (D-56, OI-20; [migration.md](migration.md) §4.15) |
| R-6 | U-235, U-413; C4 | no packetization field, slice not expressible | fixed: `MTL_OPT_CVIDEO_PACK`, `enum mtl_cvideo_pack` |
| R-7 | U-132 | the config and buffer cookie | open (minor): only `u.cookie`; OI-42 recommends a result cookie only |
| R-8 | U-210 | the per-buffer cookie | accepted: the slot index is the identity (D-76) |
| R-9 | U-068, U-069; X1; H-08 | the reserved manual-progress call | partly fixed: `mtl_sched_run_once` reserved under `MTL_LATER`; user tasklets stay an M12 cut |
| R-10 | U-139; V23; H-17 | the reserved extension-table route | fixed as a reserve: `mtl_open_ext(mt, name, version, ops, ops_size)` under `MTL_LATER`; queue meta stays an M12 decision |

### 3.3 Inconsistencies I-1…I-17 found inside the revision-4 headers

| # | Inconsistency | Outcome |
|---|---|---|
| I-1 | key names were "the contract", but no normative catalogue held them (`rx.detected.*`, `caps.*`, `port.*`, `info.trs_ps`) | fixed; residual I-1r (the `info.*` names were listed only by count) fixed: contract.md §11.3 lists them |
| I-2 | enums for key values and events defined nowhere (`mtl_backend`, `mtl_time_state`; `pacing_class` named no enum) | fixed: `enum mtl_pacing`, `enum mtl_time_state`, `enum mtl_flow_state` in `mtl.h`, `enum mtl_backend` in `mtl_observe.h` |
| I-3 | the "port" option group held instance keys and a scope that meant TX/RX | fixed: `MTL_OPT_IOVA_MODE`, `_MEMZONE_MAX`, `_CNI` at 2017–2019; `MTL_OPT_TX_MONO_POOL` / `_RX_MONO_POOL`; `MTL_OPT_SCHED_TX/RX_AUDIO_MAX`; `enum mtl_option_scope` |
| I-4 | the audio RL warm-up key in packets, the legacy knobs in ns | fixed: `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `_RL_OFFSET_NS` |
| I-5 | `enum mtl_rss` lacked legacy L3 and added a value with no legacy meaning | fixed: `MTL_RSS_NONE`, `_L3`, `_L3_L4` |
| I-6 | `MTL_OPT_PTP_UNICAST` a string, the legacy flag a bool | fixed: a bool |
| I-7 | "the library never calls the application" vs dispatch threads, the log sink, plugins, inline notify | fixed: R6 of `mtl.h` names the dispatch and log threads and the inline-notify exception (I-7r) |
| I-8 | "nothing by the timeout" returned 0 in some calls and `-MTL_EAGAIN` in others; `mtl_index_at` reused EAGAIN | fixed: R2, every data call returns a count ≥ 1 or `-MTL_EAGAIN`; `mtl_index_at` returns `-MTL_EBUSY` (WRONG_STATE) while T0 is unresolved (I-8r) |
| I-9 | two knobs for one thing: an RX fill flag and option; session vs port shared TX queue; video detect a field, ANC detect an option | fill: only `MTL_SESSION_RX_NO_FILL`; queue: `session.tx_queue` wins (`mtl_options.h`); **open (I-9r, OI-55)**: `v.detect` field vs `MTL_OPT_ANC_RX_DETECT` |
| I-10 | a process-wide sink with a per-instance level | fixed: `MTL_OPT_LOG_LEVEL` filters this instance's lines |
| I-11 | option names and stats names shared scopes | fixed: separate registries (`mtl_options.h` banner) |
| I-12 | close vs destroy naming after the rename | fixed: `mtl_mem_close`, `mtl_queue_close`, the wait handle; key `instance.sessions_closing` (I-12r) |
| I-13 | `MTL_OPT_CVIDEO_DEVICE` defaulted to AUTO, an enum with no AUTO or TEST | fixed: `enum mtl_codec_device` "absent = any device", `MTL_CODEC_DEVICE_TEST` |
| I-14 | the bit layout of `mtl_anc_packet.stream` unstated | fixed: bit 7 = S, bits 0–6 = StreamNum |
| I-15 | a progressive symbol with a rows name and enum | fixed: `MTL_OPT_ROWS_LATE`, `enum mtl_rows_late` |
| I-16 | the revision-4 summary behind the headers (function counts; the H-gap tally) | fixed: [migration.md](migration.md) §8.4 says 14 have a home, 3 are `MTL_LATER`, 2 are M12 removals (I-16r) |
| I-17 | `MTL_OPT_CVIDEO_PACK` took bare values | fixed: `enum mtl_cvideo_pack` |

Renames since the check (no row lost coverage; old names in history.md): the region close is `mtl_mem_close`,
the slot pin `mtl_tx_pin`, the requirements call → `mtl_session_query(…, req, req_size)`,
the user-MAC flow flag → `MTL_FLOWF_USER_MAC`, RX unit flags → `MTL_UNITF_*`, the packet arrival
flag → `MTL_PKTE_HW_ARRIVAL`, the progressive late option → `MTL_OPT_ROWS_LATE`, the mono-pool
and audio-max options split by direction, the RL warm-up key split in two, the RX fill option
removed, and (D-97) the spec-string parsers removed.

### 3.4 Still open

| Item | Proposal | Where decided |
|---|---|---|
| U-115 general fast copy | remove (`mtl_memcpy` is CUT-8), or `int mtl_memcpy(void* dst, const void* src, size_t n)` (DPC) in `mtl_util.h` | OI-42: remove |
| U-348 pixel-group bit layouts | byte-layout structs of the RFC 4175 pixel groups in `mtl_format.h`, or internal (perf tools, tests) | OI-42: a home in `mtl_format.h` |
| U-402 unmapped stats fields | a key each, or "no key" listed ([migration.md](migration.md) §4.14) | OI-40 |
| R-2 / U-095 | remove the DPDK AF_XDP and AF_PACKET backends | M12 |
| R-7 / U-132 | a `user_cookie` on the config, echoed in the info and events, or a result cookie only | OI-42 |
| I-9r | make ANC detect a typed field, or video detect an option | OI-55 (recommends a typed field for both) |

### 3.5 Guard rails for any later simplification

A lean core must reach directly: instance open and close, the shared instance, the port spec
(U-001…U-011, U-015, U-017); every essence at frame or field granularity with library pools
(U-180, U-200, U-230…U-232, U-244, U-260, U-280); flows with IP, UDP port, PT, SSRC, source
filter, user MAC, two legs and atomic update (U-122…U-130, U-135, U-136); blocking with
timeouts, interrupt, polling, a wait handle (U-170…U-173); RX status, packets per leg, arrival
and media times, TX result status (U-174, U-203…U-206); transport and app formats, stride,
interlace, rate (U-187, U-188, U-193, U-195, U-360, U-365); epoch pacing, TAI user pacing, user
RTP, drop when late, late reports (U-150, U-151, U-153, U-157, U-158); app memory with two-phase
release (U-112, U-184…U-186, U-208); stats and errors (U-145, U-400); RTP passthrough for every
essence (U-340…U-347); a replacement for every `t` type before the session headers are hidden.

Rules: a row may move from a typed field to a helper or an option freely, never to NOT COVERED
without an M12 entry; a default that changes the wire (U-155, U-202, U-233) is listed as CHANGED
with the legacy-compatible setting named (M13); merging two knobs keeps every legacy value
reachable (`time_source` and the former PTP flag, the two RL warm-up values of U-248); before any
session header is hidden, every `S!` row has a home and every `t` type a replacement.

The first inventory also found five inconsistencies in the revision-3 sketch, all gone in the
headers: an epoch-tick subscription with no switch (now `MTL_OPT_EPOCH_TICK`), an inverted
NUMA-binding default (now `MTL_OPT_NO_BIND_NUMA`), a per-session UDP payload with no field (now
`MTL_OPT_MAX_UDP_PAYLOAD`), no cvideo bulk switch (now `MTL_OPT_CVIDEO_DISABLE_BULK`), and the
time source set twice (now `ip.time_source` only).

Unified features with no legacy counterpart are no migration obligation: explicit states and
start instants, `mtl_session_update`, media modes, source kinds, timelines and start arrays,
late and underrun policies, results and shared queues (`mtl_queue.h`), wait handles, `MTL_E*`
with reasons and call classes, `struct_size` versioning, regions, holds, pins and export pools,
capacity queries, DSCP and TTL, the time state, histograms, the null backend, the test clock and
fault injection, and the legacy bridge ([concepts.md](concepts.md)).

## 4. Hiding the session-level headers

The requirement (D-83, M15): RTP passthrough stays, and `include/st20_api.h` and its siblings stop
being public with no loss of function. The mechanism (three tiers, version nodes, stages F, F+1,
≥ F+2, the gate header) is in [migration.md](migration.md) §8; the plugin ABI v2 in §9 there.
This section keeps the inventory behind it.

### 4.1 Today's public header set

| Fact | Evidence |
|---|---|
| all 13 top-level headers install into `<prefix>/include/mtl/`, `st20_combined_api.h` into `mtl/experimental/`; the generated `mtl_build_config.h` too, because `mtl_api.h:27` includes it | `include/meson.build:4-8`, `:11-16`; `meson.build:39-53` |
| pkg-config emits `-I${includedir} -I${includedir}/mtl`, so `<mtl/st20_api.h>` and `<st20_api.h>` both resolve; apps use the first, the MXL POC the bare form | `meson.build:77-84`; `app/sample/sample_util.h:11`, `tests/tools/RxTxApp/src/app_base.h:12`, `ecosystem/MTL_with_MXL/poc/src/include/poc_mtl_rx.h:8-9` |
| no tiering: `st_convert_internal.h` says "for internal test usage only" yet is installed and SWIG-wrapped; the experimental header has no opt-in | `include/st_convert_internal.h:5-9`, `python/swig/pymtl.i:21`, `:30`; `st20_combined_api.h:6-8` |
| include graph: `mtl_api.h` ← `st_api.h` ← `st20/30/40/41_api.h` (`:12`); `st_pipeline_api.h` ← `st20_api.h` (`:14`); `st30_pipeline_api.h` ← `st30_api.h` + `st_pipeline_api.h` (`:12-13`); `st40_pipeline_api.h` ← `st40_api.h` + `st_pipeline_api.h` (`:8-9`) | the headers |
| `st_convert_api.h` ← `st_convert_internal.h` (`:13`) ← `st20_api.h`, `st30_api.h` (`:13-14`); `st20_combined_api.h` ← `st20_api.h` (`:12`); `mtl_sch_api.h`, `mtl_lcore_shm_api.h` ← `mtl_api.h` (`:12`) | the headers |
| function counts: `mtl_api.h` 70, `st_api.h` 12, `st20_api.h` 51 (ST20 and ST22), `st30_api.h` 23, `st40_api.h` 27, `st41_api.h` 16, `st_pipeline_api.h` 106, `st30_pipeline_api.h` 24, `st40_pipeline_api.h` 27, `mtl_sch_api.h` 6, `mtl_lcore_shm_api.h` 2, convert 47 + 58 (≈ 40 public `static inline` wrappers call `*_simd`, `st_convert_api.h:41-45`), `st20_combined_api.h` 6 | `-aux-info` |
| line anchors at `545a266a`: `mtl_api.h` | `mtl_set_log_printer` `:1086`, `mtl_openlog_stream` `:1100`, `mtl_get_lcore` `:1142`, `mtl_bind_to_lcore` `:1157`, `mtl_memcpy` `:1185`, `mtl_dma_map` `:1310`, `mtl_udma_*` `:1400-1505`, `enum st21_tx_pacing_way` `:318`; `mtl_sch_api.h`: `mtl_sch_register_tasklet` `:128`; `mtl_lcore_shm_api.h`: `:44`, `:59` |
| line anchors: `st_api.h` | `st_fps` `:60`, `st_frame_status` `:78`, RFC 3550 header `:99`, `st_tx_dest_info` `:148`, `st_rx_source_info` `:159`, `st_queue_meta` `:186`, `st10_vsync_meta` `:196`, `st_event` `:208`, port and user stats `:251-397`, media-clock helpers `:545-621` |
| line anchors: `st20_api.h` | flags `:35-289`, `st20_fmt` `:321`, `st20_type` `:352`, pgroup structs `:817-1096`, `st20_ext_frame` `:1105`, RTCP ops `:1119`, `:1441`, ops `:1133`, `:1286`, `:1473`, `:1632`, timing-parser meta `:475`, `:515`, `st20_get_pgroup` `:2020`, `st20_frame_size` `:2034` |
| line anchors: `st30_api.h`, `st40_api.h`, `st41_api.h` | `st30_fmt`/`sampling`/`ptime` `:132-153`, AM824/AES3 `:193`, `:236`, size helpers `:756-832`; st40 test hooks `:90-103`, RFC 8331 structs `:142-214`, `st40_meta` `:284`, UDW/parity/RFC 8331 helpers `:816-925`; `st41_rx_ops` `:232` |
| line anchors: `st_pipeline_api.h` | `st_plugin_meta` `:77`, `st_frame_fmt` `:98`, `st_ext_frame` `:248`, `struct st_frame` `:262`, `st22_encoder_dev` `:700`, `st22_decoder_dev` `:756`, `st20_converter_dev` `:806`, register calls `:1225`, `:1385`, `:1440`, `st_tx_port`/`st_rx_port` `:838`, `:857`, `st_frame_*` helpers `:2260-2456`, `st_draw_logo` `:2365` |
| type leakage: `st_pipeline_api.h` uses `st_fps`, `st10_timestamp_fmt`, `st_frame_status`, `st_event`, `st10_vsync_meta`, `st_queue_meta`, `st_pcap_dump_meta`, `st_tx_dest_info`, `st_rx_source_info`, `st21_pacing`, `st20_packing`, `st20_fmt`, `st22_pack_type`, RTCP ops, detect meta, `st20_rx_tp_meta` (in `st_frame`, `:321`) | `st_pipeline_api.h:288-2176` |
| the st30/st40 pipelines take `st_tx_port`/`st_rx_port` from `st_pipeline_api.h` (`st30_pipeline_api.h:111`, `:274`; `st40_pipeline_api.h:140`, `:224`), which drags in all of `st20_api.h`; st40p uses `st40_meta` (`:24`), `st40_tx_test_config` (`:155`) | the headers |
| the plugin ABI uses `mtl_handle` (`st_plugin_create_fn`, `:89`), `st_frame_fmt`, `struct st_frame`, `st_plugin_device`, `st22_quality_mode`, 64-bit caps masks; the convert API uses pgroup structs, `st31_am824`/`st31_aes3`, `mtl_simd_level`, `mtl_udma_handle` | `st_pipeline_api.h:85-95`, `:670-830`; `st_convert_api.h:13`, `:44`, `:1064`; internal `:48` |
| ecosystem pipeline users need session types: GStreamer `st20_rx_frame_meta` (`gst_mtl_st20p_rx.c:140`), `st30_calculate_framebuff_size` (`gst_mtl_st30p_rx.c:229`), st40 UDW/parity (`gst_mtl_st40p_tx.c:706-707`); FFmpeg `st30_calculate_framebuff_size` (`mtl_st30p_rx.c:126`) and the session flag `ST20_RX_FLAG_DMA_OFFLOAD` on st20p ops (`mtl_st20p_rx.c:176`, a misuse) | the plugins |
| ABI hygiene: no `-fvisibility`, no version script, no export macro; 757 dynamic symbols, about 228 internal `mt_*`; soname plain `libmtl.so`; the Windows `.def` generated from every export; `ALLOW_EXPERIMENTAL_API` set for lib only | `lib/meson.build:85-87`, `:150-157`, `:122-123`, `:8` |
| deprecation: `__mtl_deprecated_msg()` on fields only, never functions; a no-op under `__MTL_PYTHON_BUILD__`, silenced in the library by `__MTL_LIB_BUILD__` | `include/mtl_api.h:141-147`, `:745-759`; `meson.build:30` |
| `lib/src` uses the installed headers through `st_header.h:9-15`; `mt_main.h:24-25` includes `mtl_sch_api.h`, `mt_sch.c:15` `mtl_lcore_shm_api.h`; include path `include_directories('.', 'include')`, so moving the files needs one include-path change and no source edit | `lib/src/st2110/st_header.h`, `meson.build:58` |
| the pipelines create sessions through the session API, which therefore stays the internal engine | `st20_pipeline_tx.c:482`, `:963`; `st20_pipeline_rx.c:599`, `st22_pipeline_tx.c:527`, `st30_pipeline_tx.c:300`, `st40_pipeline_rx.c:186` |

No consumer outside the library uses `manager/`; `ld_preload/` has no sources (`ld_preload/meson.build`
only, removed in `2b182cd87`).

### 4.2 Features reachable only through the session headers (74 rows)

Status is the revision-4 status after the fixes; the U-row of §2 gives the symbol.

| # | Feature (legacy, `st20_api.h` lines unless named) | In-tree consumers | Now |
|---|---|---|---|
| V1 | frame TX by index callback, `get_next_frame`/`notify_frame_done` `:1186-1195` | `legacy/tx_video_sample.c:220`; `app/v4l2_to_ip/v4l2_to_ip.c:1963`; RXTX `legacy/tx_video_app.c:858`; KT `st20/*` (13 creates); Rust `rust/src/imtl/video.rs:543` | COVERED (U-181) |
| V2 | frame RX callback + `st20_rx_put_framebuff` `:1544`, `:2262` | samples, RXTX legacy, MXL `poc/src/sender/mxl_bridge.c:499`, Rust `video.rs:858` | COVERED (U-201); MXL by `MTL_SESSION_RX_BY_INDEX` |
| V3 | tasklet-context latency of V1/V2 (bobi, MXL, fwd chose callbacks for latency) | MXL POC, fwd samples, ext bobi | LATER (U-176, H-15) |
| V4 | RTP level `:356`, `:1932-1948`, `:2279-2290`, `:1258`, `:1619` | `low_level/{tx,rx}_rtp_video_sample.c:161/107`; RXTX `legacy/tx_video_app.c:287`; KT (61 `RTP_LEVEL` sites) | COVERED (U-340, U-341, H-01) |
| V5 | slice TX/RX `:362`, `:1254`, `:1610`, `:1603` | `low_level/*slice*`; RXTX `legacy/tx_video_app.c:784`, `rx_video_app.c:548`; KT `st20/{digest,uframe,detect}` | COVERED (U-191, U-218, H-02) |
| V6 | TX ext frame `st20_tx_set_ext_frame` `:1882`, `ST20_TX_FLAG_EXT_FRAME` `:45` | samples ×4, `v4l2_to_ip.c:1457`, KT `st20_common.cpp:148` | COVERED (U-184); per-acquire layout LATER |
| V7 | RX static `ext_frames[]` `:1550` | KT | COVERED (U-208) |
| V8 | RX `query_ext_frame` `:1599` (needs `RECEIVE_INCOMPLETE_FRAME`) | KT `st20/st20_ext_frame.cpp:203`; MXL `poc/src/sender/mtl_rx.c:199`; GST via st20p (`gst_mtl_st20p_rx.c:140`) | LATER (U-209, H-09) |
| V9 | `uframe_pg_callback` + `uframe_size` `:1586`, `:1574` | RXTX `legacy/rx_video_app.c:593`; KT `st20_uframe.cpp:151`, `st20_detect.cpp:167` | CUT-2; per-packet conversion kept (`st_pipeline_api.h:590`, H-16) |
| V10 | auto-detect `:219`, `notify_detected` `:1592` | RXTX `legacy/rx_video_app.c:534,550`; KT `st20_detect.cpp:171`; `rx_st20p_auto_detect_sample.c:195` | COVERED (U-212, H-06) |
| V11 | timing parser STAT `:238` | RXTX `legacy/rx_video_app.c:565`; acceptance `tests/single/rx_timing` | COVERED (U-214) |
| V12 | timing parser META `:243`, `:475`, `:2328` | `rx_st20p_timing_parser_sample.c:85`; Python `rx_timing_parser.py:135`; UT `st20_harness.h:147` | COVERED (U-215, H-07) |
| V13 | RX DMA offload `:212`, `:2314` | RXTX, KT ×5, FF (misused flag) | COVERED (U-211) |
| V14 | header split `:226` | RXTX `legacy/rx_video_app.c:560`; KT `st20_digest.cpp:155`; `ext_frame/rx_st20p_hdr_split_gpu_direct.c:311` | CUT-1 (NG6, Q-MEM-9) |
| V15 | GPU VRAM frames `:1624`; st20p `USE_GPU_DIRECT_FRAMEBUFFERS` (`st_pipeline_api.h:642`) | FF `mtl_st20p_rx.c:204-220`; `app/sample/gpu_direct/` | LATER (U-117, H-10) |
| V16 | NACK retransmission `:87`, `:186`, `:1119`, `:1441` | RXTX `legacy/{tx,rx}_video_app.c:801/562`; KT `st20_digest.cpp:86,157`; `doc/rtcp.md` | COVERED (U-141, H-05) |
| V17 | two RX threads `:249` | RXTX | COVERED (U-217) |
| V18 | user pacing, exact, user timestamp, epoch RTP, RTP delta `:56-100` | KT `st20_user_pacing.cpp:75`, `tx_timestamp_sync_test.cpp:227`; `fwd/rx_st20_tx_st20_split_fwd.c:271` | COVERED (U-151…U-156) |
| V19 | VSYNC `:75`, `:181`; `st_api.h:208` | RXTX `legacy/tx_video_app.c:9,792`; KT `tests.cpp:955` | COVERED (U-159) |
| V20 | `notify_frame_late` `:1204` | KT | COVERED (U-158) |
| V21 | `update_destination` / `update_source` `:1866`, `:2187` | KT `st20/st20_update_source.cpp:146` | COVERED (U-135, U-136) |
| V22 | `get_sch_idx`, `get_pacing_params` `:1959`, `:1976`, `:2198` | RXTX `legacy/tx_video_app.c:139` | COVERED (U-070, U-161) |
| V23 | queue meta + `DATA_PATH_ONLY` `:2303`, `:176` | KT `st20_digest.cpp:212` and two more; `st22_test.cpp:1412` | CUT (Q-MODE-5, H-17) |
| V24 | pcapng dump `:2216` (+ st20p/st22p/st20rc) | RXTX `legacy/rx_video_app.c:700`; KT `st20/st20_dump.cpp:113` | COVERED (U-140, H-11) |
| V25 | stats get/reset `:1993`, `:2344` | RXTX, KT | CHANGED (no reset) |
| V26 | static pad, bulk, NUMA, user MACs, migrate off, burst, simulated loss `:35-249` | RXTX, KT | COVERED |
| V27 | linesize, `user_meta` | KT `st20_linesize_digest.cpp` | COVERED (U-188, U-190) |
| V28 | `st20_get_pgroup`, `st20_frame_size`, `st20_get_bandwidth_bps`, names `:2020-2517` | samples ×6, RXTX ×3, KT ×15, fuzz | COVERED (U-196, U-197, H-04) |
| V29 | transport-format set (18 `st20_fmt` values) `:321-351` | every video user | COVERED (U-193, U-194, H-03) |
| C1 | ST22 codestream frames `:373`, `st22_tx_get_fb_addr` `:2161` | `legacy/{tx,rx}_st22_video_sample.c:196/179`; RXTX `legacy/{tx,rx}_st22_app.c`; MXL `poc_8k/…/main.c:355,813`; KT `st22_test.cpp` | COVERED (U-232) |
| C2 | ST22 RTP level `:2120-2136`, `:2443-2454` | KT `st22_test.cpp` (×12) | COVERED (U-342) |
| C3 | `DISABLE_BOXES` `:127`, `:266` | none (`git grep DISABLE_BOXES`) | CUT-6 |
| C4 | `ST22_PACK_SLICE` `:386-387` | none | COVERED (U-413) |
| C5 | ST22 RTCP, pcapng, queue meta, VSYNC, update | KT `st22_test.cpp:1327,1152,1412,740` | as V16, V24, V23, V19, V21 |
| A1 | audio frame sessions (`st30_api.h:676`, `:847`) | RXTX `legacy/{tx,rx}_audio_app.c:484/416`; KT `st30_test.cpp:372/563` | COVERED (U-244) |
| A2 | audio RTP level (`:729-745`, `:902-913`) | RXTX `legacy/tx_audio_app.c:141`; KT `st30_test.cpp:72` | COVERED (U-343) |
| A3 | pacing way, RL warm-up, FIFO (`:178`) | RXTX | COVERED (U-247, U-248, U-250) |
| A4 | audio timing parser STAT/META (`:118` ff.) | RXTX `legacy/rx_audio_app.c:380,383` | CHANGED (U-254) |
| A5 | size helpers (`:756-832`) | FF `mtl_st30p_{rx,tx}.c:126/129`; GST `gst_mtl_st30p_rx.c:229`; Rust `audio.rs:215` | COVERED (U-245, U-246) |
| A6 | audio RTCP flags | RXTX `legacy/rx_audio_app.c:379` | CUT-7 |
| A7 | AM824/AES3 conversion (`st_convert_api.h:1064`) | — | COVERED (U-255) |
| N1 | ANC frame sessions, ≤ 20 meta (`st40_api.h:612`, `:725`) | RXTX `tx_ancillary_app.c:480`, `rx_ancillary_app.c:159` (main app, not legacy/); KT `st40_test.cpp:409/586` | COVERED (U-260, U-261) |
| N2 | ANC RTP level (`:695-711`, `:766-777`) | RXTX `tx_ancillary_app.c:147`, `rx_ancillary_app.c:67`; KT | COVERED (U-344) |
| N3 | RFC 8331 and UDW helpers (`:816-925`) | GST `gst_mtl_st40p_tx.c:655,706-707`; RXTX `rx_ancillary_app.c:17,30`; UT, fuzz | COVERED (U-268, H-13) |
| N4 | test mutation config (`:90-108`) | GST test element | COVERED (U-267) |
| N5 | split by packet, EXACT, DEDICATE_QUEUE | RXTX `tx_ancillary_app.c:475-478` | COVERED |
| N6 | ANC RTCP flags | RXTX `rx_ancillary_app.c:155` | CUT-7 |
| F1 | fastmeta TX (`st41_api.h:377`) | RXTX `tx_fastmetadata_app.c:475`; acceptance `tests/single/st41` | COVERED (U-280) |
| F2 | fastmeta RX, RTP only (`st41_rx_ops` `:232`, `:501-512`) | RXTX `rx_fastmetadata_app.c:166,277` | COVERED (U-283, U-345) |
| F3 | DIT/K filters | RXTX | COVERED (U-282) |
| F4 | fastmeta RTCP flags | RXTX `rx_fastmetadata_app.c:258` | CUT-7 |
| S1 | `st_fps`, `st_frame_rate*`, `st_name_to_fps` | FF `mtl_common.c:36`; plugins | COVERED (U-390) |
| S2 | media-clock helpers | RXTX latency code | COVERED (U-391, H-12) |
| S3 | `struct st_frame`, `st_ext_frame` | every pipeline consumer | COVERED (U-365) |
| S4 | `st_frame_fmt` (≈ 28 app formats) | GST `gst_mtl_st20p_tx.c:370,666`; OBS `mtl-output.c:217`; samples | COVERED (U-361) |
| S5 | `st_frame_*` helpers | samples, GST, plugins (`st_frame_fmt_name` ×10, `st_frame_plane_size` ×6) | COVERED (U-368) |
| S6 | `st_draw_logo` (`st_pipeline_api.h:2365`) | samples ×5, KT `st22p_test.cpp:435` | CUT-8 (sample-local code) |
| S7 | `st_name_to_codec` | FF `mtl_st22p_rx.c:92` | COVERED (U-370) |
| S8 | SWIG setters `st_txp_*`, `st_rxp_*`, `mtl_para_*` | Python | COVERED (U-102, U-371) |
| S9 | port and user stats structs, `st_var_info` | RXTX | COVERED (U-033, U-400) |
| S10 | `st_rfc3550_rtp_hdr` | RTP-level users | COVERED (U-347) |
| M1 | log hooks (`mtl_api.h:1086`, `:1100`) | RXTX `args.c`, `rxtx_app.c` | COVERED (U-019…U-021, H-14) |
| M2 | lcore borrow (`mtl_api.h:1142`, `:1157`) | `app/perf/*` ×14, RXTX, KT `st_test.cpp` | CUT-5b |
| M3 | user DMA `mtl_udma_*` (`mtl_api.h:1400-1505`) | `app/sample/dma/dma_sample.c:24`, `app/perf/*`, KT `dma_test.cpp`, `cvt_test.cpp` | CUT-5 |
| M4 | hugepage and DMA memory (`mtl_dma_map` `:1310`) | samples, fwd | COVERED (U-110…U-113) |
| M5 | PTP read, time function, sync notify | RXTX, FF | COVERED (U-045, U-046, U-048) |
| M6 | `mtl_sch_enable_sleep`, `_set_sleep_us` | RXTX `rxtx_app.c:176`; KT `st20p_test.cpp:929` | COVERED (U-052, U-053, H-18) |
| M7 | `mtl_memcpy` (`:1185`, ≈ 60 files), `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align` | widely | CUT-8 |
| M8 | port and backend queries, manager alive, port stats, `mtl_abort` | samples, RXTX | COVERED (U-006, U-035, U-096, U-099) |
| M9 | SIMD level | perf tools | COVERED (U-066) |
| X1 | user schedulers and tasklets (`mtl_sch_register_tasklet`, `mtl_sch_api.h:128`) | KT `sch_test.cpp:16,147` | CUT (H-08) |
| X2 | `mtl_lcore_shm_print`/`clean` (`mtl_lcore_shm_api.h:44`, `:59`) | `app/tools/lcore_shmem_mgr.c:60,63` | internal tool; SysV table removed (D-92) |
| X3 | ≈ 105 per-pair converters (`_simd`, `_simd_dma`, `_2way_cpuva`) | `app/tools/convert_app.c` (≈ 20), `app/perf/*`, RXTX `fmt.c:27`, `cvt_test.cpp` (≈ 70), `plugins/sample/convert_plugin_sample.c:24`, Python (`st_convert_internal.h:1423`) | COVERED by `mtl_convert` (CUT-4 for the per-pair functions) |
| X4 | `st20rc` redundant-combined RX | `experimental/rx_st20_redundant_combined_sample.c:191`; RXTX `experimental/rx_st20r_app.c:341` | CUT-3 (two legs, D-59) |

Tally at the study (revision 3): 34 covered, 11 partial (V28, V29, C5, A4, A7, S2, S4, S5, S7, M6,
X3), 6 later (V3, V5, V10, V15, V16, X1), 5 not covered (V8, V12, V24, N3, M1), 12 behind 10 cuts
(V9, V14, V23, C3, A6, N6, F4, S6, M2, M3, M7, X4), 5 packet-unit rows (V4, C2, A2, N2, S10: one
work item, H-01), 1 internal (X2). C5 counted as partial, F2 as covered. Today: §3.1.

### 4.3 Who blocks hiding

| Consumer | Session-header dependency | Path to unified |
|---|---|---|
| FFmpeg plugin | pipelines, plus `st30_calculate_framebuff_size`, `st_frame_rate_to_st_fps`, `st_name_to_codec`, the misused `ST20_RX_FLAG_DMA_OFFLOAD` (`mtl_st20p_rx.c:176`) | Phase 6 rewrite; the flag fixed in Phase 0 ([migration.md](migration.md) §11.4) |
| GStreamer plugin | pipelines, plus `st20_rx_frame_meta`, st40 UDW/parity helpers, `st_frame_fmt_*` | Phase 6 rewrite; needs H-09 and H-13 |
| OBS | `st_pipeline_api.h` only (`linux-mtl/linux-mtl.h:10`) | a small port |
| `plugins/` | implements `st22_encoder_dev`, `st22_decoder_dev`, `st20_converter_dev` (`plugins/sample/st22_plugin_sample.c:231,251`; `plugins/st22_avcodec/st22_avcodec_plugin.c:519,535`; `plugins/sample/convert_plugin_sample.c:144`) | plugin ABI v2, Phase 2; the v1 loader stays until F+2 |
| MXL POC | `st20_rx` + `query_ext_frame`, `st22` sessions | `MTL_SESSION_RX_BY_INDEX`; cvideo |
| Rust `imtl-rs` | bindgen over `wrapper.h:1-6`; session `st20_tx/rx_create` frame mode (`video.rs:543,815`) | rebind on `mtl.h` |
| Python `pymtl` | SWIG over six headers incl. the internal convert header (`pymtl.i:16-32`) | the Phase 1 wrapper; `_2way_cpuva` replaced by `mtl_convert` |
| `app/sample` | `sample_util.h:11-15` pulls every legacy header; session samples in `legacy/`, `low_level/`, `ext_frame/`, `fwd/`, `experimental/`, `dma/` | canonical samples on unified (Q-MIG-2); old ones on the legacy dependency until removal |
| `app/v4l2_to_ip` | `st20_api.h` (`v4l2_to_ip.c:31`), session TX with ext frames | video TX with attached slots |
| `app/tools`, `app/perf` | lcore shm, convert, `*_simd_dma`, lcore borrow, udma | the internal dependency (in-tree tools) |
| RxTxApp | session ANC and fastmeta apps in the main tree; legacy video, audio, st22; experimental st20rc; JSON kinds `video`, `audio`, `ancillary`, `fastmetadata`, `st22` (`tests/acceptance/mtl_engine/rxtxapp_config.py:20-41`) | ANC and fastmeta in Phase 2; RTP and slice JSON in 2P and Phase 6; the JSON schema kept ([migration.md](migration.md) §11.1) |
| KahawaiTest, unit, fuzz | 752 session call sites; unit and fuzz compile lib `.c` (`tests/unit/session/st20_harness.c:22`, `tests/fuzz/st20/st20_rx_frame_fuzz.c:26`; `tests/unit/meson.build:20-21`, `tests/fuzz/meson.build:43-44`) | not blockers: the internal dependency; they keep testing the engine |

### 4.4 Pre-hide gaps H-01…H-19

The homes are in [migration.md](migration.md) §8.4. The study's phase and size (S < 0.5 EM,
M 0.5–2 EM, L > 2 EM, guesses for spikes to cost) and what changed:

| Gap | Study phase, size | Now |
|---|---|---|
| H-01 RTP level, ST 2022-6 | 6 pulled before F, L | 2P (M14) |
| H-02 slice / rows | 6, M–L | 6 |
| H-03 format parity (+7 transport, ≈ +16 app formats) | 1, S | done (§2.10, §2.17) |
| H-04 format helpers | 1, S | done; no fps name parser (D-97) |
| H-05 NACK retransmission (proposed as a `next` block) | 2, M | `MTL_OPT_RTX*` options instead |
| H-06 video auto-detect (deliver nothing until accepted) | 2, M | `v.detect`; Phase 1 (M3a) |
| H-07 per-unit ST 2110-21 results (proposed as a CQ record) | 2, M | `mtl_rx_get_timing` per lease and leg |
| H-08 user tasklets | 2, M, or cut | M12 (Q-HIDE-4) |
| H-09 RX destination per unit | 4, M | LATER, Phase 4 |
| H-10 GPU VRAM | 4–6, M–L | LATER, Phases 4–6 (Q-MEM-8) |
| H-11 pcapng capture (a SYNC flag proposed) | 2, S | asynchronous only |
| H-12 media-clock helpers | 1, S | done |
| H-13 conversion, AM824, ANC helpers (`libmtl_convert` optional, Q-MODE-4) | 2, M | done |
| H-14 log sinks (+ an instance log prefix) | 1, S | `mtl_log_set_sink` with a prefix argument |
| H-15 inline notify | 3, S–M | LATER (M5) |
| H-16 per-packet RX conversion | 2, S | Phase 1 (M2), OI-28 |
| H-17 queue meta | 5, S, only if the cut is rejected | M12; `mtl_open_ext` reserved |
| H-18 scheduler sleep interval (an instance field proposed) | 1, S | `MTL_OPT_SCHED_SLEEP_US` |
| H-19 plugin ABI v2 | 2, M | `mtl_plugin.h` |

Risk the study named: H-01, H-02, H-05…H-11 and H-15 pull "later" work before F; if v1 does not
grow, F slips or the hide moves later as a block (spike S3 costs H-01 and H-02 with the packet
design).

### 4.5 Cuts CUT-1…CUT-8 (decision M12)

| Cut | What | Evidence | Replacement |
|---|---|---|---|
| CUT-1 | header split (`ST20_RX_FLAG_HDR_SPLIT`, `ST20P_RX_FLAG_HDR_SPLIT`) | NG6, Q-MEM-9; no `patches/dpdk/26.07/hdr_split/`; a test flag, one KT case, one sample | none; zero-copy RX through regions, H-10 |
| CUT-2 | `uframe_pg_callback`, `uframe_size` | app code on the tasklet (R-THR-4); one RXTX option, two KT files | H-16; user formats through a converter plugin |
| CUT-3 | `st20rc` and `experimental/st20_combined_api.h` | Q-MODE-7; one sample, one RXTX app | two-leg RX (D-59) |
| CUT-4 | public per-pair converters (≈ 105) | `st_convert_internal.h:5-9` says internal; the public set is mostly `static inline` wrappers | `mtl_convert`; per-pair internal for `cvt_test.cpp`, `app/perf` |
| CUT-5 | public user DMA (`mtl_udma_*`), `*_simd_dma` converters | one sample, perf tools, two KT files | DMA inside sessions (`MTL_OPT_DMA`) |
| CUT-5b | public lcore borrow | perf tools, RXTX, one KT case | internal; revisit if the private-user survey (Q-MIG-1) finds users |
| CUT-6 | `ST22_TX/RX_FLAG_DISABLE_BOXES` | no consumer | none |
| CUT-7 | RTCP flags on ST30/ST40/ST41 | no effect in `lib/`; Q-MODE-8 | none (video and cvideo keep NACK) |
| CUT-8 | `st_draw_logo`, `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align` | thin wrappers or sample utilities | sample code, libc, `mtl_unit_copy_in/out`, `caps.page_size` |
| CUT-? | queue meta + `DATA_PATH_ONLY` | KT only; suspected NULL-flow dereference on non-socket backends (Q-MODE-5) | H-17 if rejected |

M12 ([decisions.md](decisions.md) §2.13) also lists `mtl_get_if_ip`, `mtl_udma_fill`, the dead
fields, the DPDK AF_XDP and AF_PACKET backends and `st22_*_ops.fmt`. Cuts that are not
legacy-only, because consumers exist, were flagged for approval in the first inventory: the
CUSTOM8 formats (since kept, U-362), the plugin ABI and the conversion library (since replaced,
§2.18, U-393), lcore borrowing, the DMA device list (since kept, U-031) and the log printer
(since kept, U-019).

### 4.6 Prior art and the study's questions

| Project | Mechanism | What MTL takes |
|---|---|---|
| DPDK | `__rte_experimental` = deprecated unless `ALLOW_EXPERIMENTAL_API`; `__rte_internal` = `error("Symbol is not public ABI")` unless `ALLOW_INTERNAL_API`; driver headers only with `-Denable_driver_sdk=true` (`rte_compat.h:9-53`; DPDK 26.07 `meson_options.txt:25-26`, `lib/meson.build:128,210`) | the error guard of `MTL_INTERNAL`; legacy headers behind a build option |
| GStreamer | unstable -bad libraries `#warning` unless `GST_USE_UNSTABLE_API` (`gst/basecamerabinsrc/gstbasecamerasrc.h:26-29`) | the warn-first opt-in of stage F |
| FFmpeg | only listed headers installed (`libavcodec/Makefile:4`); `FFCodec` in an uninstalled header (`codec_internal.h:127`); `FF_API_*` ties removal to the next major (`version_major.h:31-43`); `attribute_deprecated` (`libavutil/attributes.h:100`) | removal tied to a soname major; counter-example: no external codec ABI |
| OpenSSL 3.0 | the low-level calls are deprecated in favour of EVP (deprecation macros and the compatibility level in `openssl/macros.h:158-187`, `aes.h:49`); engines replaced by providers that get a dispatch table (docs.openssl.org/3.0/man7/migration_guide) | high-level API and a redesigned plugin interface at once; the plugin gets a host table and never links libmtl |
| libfabric | app headers `rdma/fabric.h` + `fi_*.h`, provider `ofi_*.h` not installed; `fi_open_ops(fid, name, …)` | `mtl_open_ext` (LATER) |
| Vulkan | core plus extensions, beta ones behind `VK_ENABLE_BETA_EXTENSIONS`; `pNext` chains; loader-ICD version negotiation | the shape of `MTL_LEGACY_API`; the plugin entry point's version negotiation |
| Linux | `include/uapi/` is the only exported set | physical separation |

Lessons: hide physically (an uninstalled header cannot be defeated with `-D`); stage with a
visible opt-in, then a compile error, then removal tied to a major; tie symbols to version nodes
so a binary records its tier; redesign the plugin ABI with the high-level API.

| Question | Answer now |
|---|---|
| Q-HIDE-1 approve CUT-1…CUT-8 and CUT-? one by one | decision M12 |
| Q-HIDE-2 keep `libmtl_unified.so.1` separate at the freeze, so the legacy removal bumps only libmtl's soname | decision M8 (D-23); [migration.md](migration.md) §7.2 |
| Q-HIDE-3 the same removal date for the pipeline node as for the session node, or decided at the Phase 2 go/no-go | the Phase 2 go/no-go ([migration.md](migration.md) §8.3) |
| Q-HIDE-4 user tasklets: an advanced opt-in surface, or cut | M12 (H-08); `mtl_sched_run_once` reserved |

The study's proposed log entries became D-83 (three tiers, stages; plugin ABI v2;
`mtl_convert.h`), D-87 (the gate), D-82 (packet units, replacing D-25) and Q-MODE-4's answer;
it also changed Q-ABI-4 from "frozen" to "deprecated, then internal". Further risks it named:
about 10 private repositories may use session-only features no in-tree consumer shows (slice at
bobi, `DATA_PATH_ONLY`, user DMA), to re-check with the `readelf -V` audit (Q-MIG-1, M10);
`mtl_instance_from_legacy` must stay exported for the whole legacy window; the acceptance suite
hard-codes `.local_install` and RxTxApp's legacy JSON kinds, the critical path of the Phase 6
gate; a stage-2 `#error` breaks builds that include a legacy header only for a type (GStreamer
`st20_rx_frame_meta`), so the stage-2 CI job runs on the ecosystem trees before F+1.

## 5. Sample friction (revision-3 examples) and what became of it

The study read the 13 revision-3 examples, the 31 sample diagrams, today's `app/sample/` and
`include/st_pipeline_api.h` as a developer who knows `st20p_tx_get_frame` / `put_frame` and
nothing else. A line was **ceremony** (out-structs, init calls, `direction`, flow plumbing, error
branches, labels, casts, null checks, stride arguments, teardown order) or **app** (intent: width,
fps, addresses, `render()`); a **hidden rule** is a contract invisible at the call site; the score
runs from 1 (reads like the intent) to 5 (cannot be written correctly without the design
documents). Findings: about 82 % of example code was ceremony (±2 lines per example); revision 3
was longer than st20p for three of four core tasks (minimal TX 54 lines against ≈ 36, zero-copy TX
120 against ≈ 35); the examples carried 115 hidden rules (about 130 with the C++ twin); two
examples had bugs the API shape invited. Revision 4 took most proposals (D-72, D-74…D-78, D-88);
today's examples are in [examples.md](examples.md).

| Ex | Shows | Lines / code | Ceremony / app | Rules | Score |
|---|---|---|---|---|---|
| ex01 | minimal TX, library pool, no results | 63 / 54 | 46 / 8 (85 %) | 12 | 4 |
| ex02 | minimal RX, zero-filled gaps | 67 / 59 | 49 / 10 (83 %) | 9 | 4 |
| ex03 | TX results in an epoll loop | 76 / 67 | 57 / 10 (85 %; 12 lines are epoll) | 9 + 1 bug | 5 |
| ex04 | zero-copy TX from a framework arena, retire-aware teardown | 139 / 120 | 105 / 15 (88 %) | 20 + 1 bug | 5 |
| ex05 | RX lent to a framework, GStreamer unlock | 51 / 42 | 36 / 6 (86 %) | 8 | 3 |
| ex06 | MXL grains by index | 58 / 47 | 39 / 8 (83 %) | 9 | 4 |
| ex07 | A/V/ANC from one file on one timeline | 120 / 104 | 79 / 25 (76 %) | 13 | 5 |
| ex08 | progressive (line) TX | 40 / 33 | 23 / 10 (70 %) | 8 + 1 inconsistency | 4 |
| ex09 | 2160p → 4 × 1080p zero-copy split with RX holds | 88 / 76 | 56 / 20 (74 %) | 10 + incomplete teardown | 4 |
| ex10 | time-preserving processor | 77 / 67 | 57 / 10 (85 %) | 7 | 3 |
| ex11 | SIGINT shutdown | 53 / 44 | 25 MTL + 10 POSIX / 9 | 5 | 3 |
| ex12 | the revision-3 simple layer, TX and RX | 46 / 40 | 32 / 8 (80 %) | 5 | 2 |
| cpp | C++ twin (42 functions in one 111-line function) | 130 / 111 | ≈ 95 / 16 | ≈ 15 | 5 |

Hidden rules by example, in today's terms where they survive:

- **ex01:** `direction` required (0 invalid); init before any field; create does not start;
  back-pressure only explained by `status.blocked_on`; a failed submit left the lease with the app;
  a DRAIN stop may time out; close may be deferred; the instance outlives closing sessions; a cookie
  without results is rejected (`COOKIE_WITHOUT_RESULTS`); the payload type in the flow; fps through
  `mtl_fps_rational`.
- **ex02:** without start a dequeue waits for ever with no hint; a stride argument; zero fill for
  library pools only (D-37); RX stopped with FLUSH and timeout 0; release is mandatory;
  `-MTL_ESHUTDOWN` means "you asked", never "not started".
- **ex03:** results must be on, invisible in the loop; wait masks must match; the try-wait call returned
  1/0/<0 unlike every other call; the library drains the fd; at timeout 0 acquire said EAGAIN and
  reap 0; results in submission order; one waiter per target. Bug F-25.
- **ex04:** query before allocating; check `min_count_direct`, `min_span`; attached pools forced
  results, so reap or acquire blocks on RESULTS; page alignment; READ access for TX; six buffer
  fields copied from the requirements; attach only in CREATED; the submission cookie overrides the
  buffer's; FLUSH gives every accepted unit a result; reap until empty; retire needs an event queue
  subscribed before close; the memory stays mapped until the region closes; the queue closes last.
  Bug F-25.
- **ex05:** release from any thread, legal while closing; `-MTL_ECANCELED` is sticky until cleared;
  `-MTL_ESHUTDOWN` only from your own stop or close; latest-only receive needs `pool_count` 2 +
  `MTL_SESSION_RX_LATEST`; a failed wrap needs a release.
- **ex06:** by index the slot is `index mod count`; a leased or held slot drops the unit; a sizing
  formula; the epoch timeline set although null already means it; attach before start; WRITE access;
  the media-time validity flag; two calls returned the same index.
- **ex07:** every member needs the timeline, INDEX mode and a source kind (`PLAYBACK` is 0); AUTO
  members are rejected in a set; `media_index` counts frames for video and ANC but samples for
  audio; ANC k goes before or with video k; the empty ANC packet is automatic; ANC rate and raster
  equal the video's; start at index 0; start is all or nothing; reused submissions must reset ANC
  fields; two submit styles; teardown order group → sessions → timeline.
- **ex08:** rows + GATEWAY + INDEX + `troffset_ns`, none visible in the code; the hint needs an init
  and its index copied; ready rows set at submit; the lease stays current after submit; all rows =
  final; late rows truncated. Inconsistency F-23.
- **ex09:** TX buffers over the RX pool region; `stride ≥ row_bytes` keeps DIRECT; READ access;
  results forced, so reap and discard; a hold keeps the RX slot until the last TX result; release
  after every submit; EBUSY drops the quad; TX starts before RX; the validity flag; a slot cap.
  Teardown incomplete (F-26).
- **ex10:** TAI + CAPTURE + `min_tx_delay_ns`; RTP passthrough with AUTO rejected; a sender-clock
  input has no media time; audio needed both a sample count and bytes; ANC needs a cast of the meta
  area and a count; two return codes combined by hand.
- **ex11:** interrupt is AS-safe and sticky; it had to be cleared before a DRAIN stop (F-13); join the workers first; the handle must be written before the handler is installed.
- **ex12:** one frame outstanding per handle; the port spec per handle; format names as strings; `null:1`; close is a 1 s DRAIN.
- **cpp:** capacity-checked query; handle checks as noise; a view cache sized from the pool count; a
  lazy view; a 5-argument time difference with two validity masks; a private queue read; a
  dispatcher start and stop; the debug API.

### 5.1 Friction register F-01…F-26

| ID | Rank, hits, saves | Friction | Proposed fix | Outcome in the headers |
|---|---|---|---|---|
| F-02 | 1; 11 of 13; ≈ 80 lines | one session = two configs, `direction`, a flow parse, create, an error branch (10–14 lines) | an open call per essence taking a flow spec | one `struct mtl_session_config` and `mtl_session_open(mt, &sc, &s)` (D-72); typed flows, `mtl_flow_ipv4()` (D-97) |
| F-03 | 1; 9; ≈ 25 | create and start separate; a forgotten RX start times out with no hint | the open call starts NOW; a NOT_STARTED reason | `mtl_session_open` starts now; no NOT_STARTED reason |
| F-05 | 2; 7; ≈ 20 | a failed submit leaves the lease with the app; a missed release leaks the pool | submit always consumes the lease (a flag keeps the old rule) | adopted except for `-MTL_EAGAIN` (D-88); no flag; OI-15 |
| F-07 | 3; 8; ≈ 45 | teardown is a protocol (ex04: 20 lines) | `mtl_session_close(s, timeout)`: drain, destroy, wait for retire, null-safe | adopted: 0 retired, 1 still retiring (D-88) |
| F-04 | 4; 10; ≈ 30 | out-structs initialised every iteration, a stride on a single dequeue, a view cache | const views of the per-slot records; 3-argument acquire and dequeue | one `struct mtl_unit` lent by acquire and dequeue (D-75), `MTL_INIT` once |
| F-01 | 5; 12; ≈ 40 | 44 init calls; value macros unused | designated initialisers and compound literals | `MTL_INIT(&s)` only (D-74); zero-filled = default |
| F-08 | 6; 2 + plugins; ≈ 50 | zero-copy import ≈ 45 lines | the session imports and carves an arena; acquire by index | `mtl_session_attach` lays out N slots (D-76); `mtl_tx_acquire_slot` |
| F-06 | 7; 12; ≈ 30 | EAGAIN and ETIMEDOUT handled alike; every app writes `ex_fail`; null checks before every destroy | one "nothing now" code; a `perror`-style helper; null-safe destroy | `-MTL_EAGAIN` for every miss, `-MTL_ETIMEDOUT` for stop deadlines (R2, D-88); every close accepts null (R4); no helper: `mtl_error_name`, `mtl_reason_name`, `mtl_last_error` |
| F-11 | 8; 1 + playout apps; ≈ 30 | A/V/ANC: timeline, timing ×3, a group, adds, start params, ordered teardown | a group with a private timeline, `sc.group`, start at index 0 | no group object: start arrays on one timeline (D-78); null timeline = the epoch; ANC raster from the start |
| F-12 | 9; 2; ≈ 15 | epoll needs a wait object, a kind check, a cast and a try-wait before each sleep | a plain fd; a timeout-0 miss arms it | one plain wait handle `mtl_session_get_wait_handle`; a miss arms it (R2, D-88) |
| F-09 | 10; 3; ≈ 10 | attached pools force results; no-results rejects cookies | results never throttle unless requested; cookies always legal | not adopted: results always for app memory, opt-in for library pools, a cookie without results is `COOKIE_WITHOUT_RESULTS` (D-77) |
| F-13 | 11; 1 + signal apps; ≈ 3 | interrupt must be cleared before a DRAIN stop | interrupts cancel data waits only | adopted (D-88) |
| F-16 | 12; 3; ≈ 3 | two names for one buffer index | one index call | `u.slot` |
| F-17 | 13; 1; ≈ 4 | progressive and INDEX producers copy a hint's index | a submit flag "next index" | not adopted: `mtl_tx_next_slot` |
| F-15 | 14; 4; ≈ 4 | media time valid only with a bit; a 5-argument time difference | inline helpers | `MTL_UNITF_TAI_VALID`, `MTL_UNITF_INDEX_VALID`; no helpers |
| F-18 | 15; 3; ≈ 6 | examples set defaults, teaching that they are required | set only non-defaults; list the defaults relied on | documentation ([examples.md](examples.md)) |
| F-10 | 16; 1; ≈ 3 | one 20-field submission reused across essences | per-call literal; "read at the call only" | the unit's per-use fields are zeroed by acquire |
| F-19 | 17; all; 0 | the port string hard-coded | `MTL_PORTS` from the environment | adopted (`mtl_instance_open` with no ports) |
| F-20 | 18; 0 (a gap) | 2022-7 shown nowhere; legs filled separately | legs in one flow spec | spec strings removed (D-97); `sc.flows[0..1]`; ex02 receives two legs |
| F-21 | 19; 1; ≈ 2 | ANC passthrough casts the meta area | a typed table getter; `mtl_tx_write` takes the RX lease | `MTL_META_ANC` records; a received unit is a valid `mtl_tx_write` template |
| F-22 | 20; 2; ≈ 1 | audio passes bytes and a sample count | bytes 0 = derived | one field: `u.used` (bytes) |
| F-23 | 21; 1; 2 | publish failure contract inconsistent | settled by F-05 | no publish call: rows resubmit the lease (D-88 applies) |
| F-24 | 22; 1 | the C++ twin shows 42 functions in one function | split it | `examples_cpp.cpp` rewritten (88 lines) |
| F-25 | 23; 2 | ex03 and ex04 took a source frame before acquire and lost it on EAGAIN or EBUSY | acquire first | fixed: "slot first, then the source frame" (ex03) |
| F-26 | 24; 1 | ex09 never closed its 16 TX buffers or sessions | `mtl_session_close`; attached buffers owned by their session | `mtl_session_close`; sessions attached over a pool close first (1 until they do) |

Proposals that changed then-current decisions: F-05 the "fix and resubmit" rule, F-06 C2 and C7
(null handles), F-09 D-35, F-12 D-43 (implicit arming), F-13 D-30; F-02 and F-07 added 7
functions, 15 in all, and moved about 20 to an advanced tier. Rewrites counted 54 → 26 (minimal
TX), ≈ 50 → 25 (epoll RX; +4 for TX results), 104 → 48 (A/V/ANC; about 25 of it MTL) and
120 → 36 (zero-copy TX) code lines, each shorter than today's equivalent with every safety
property kept. Today's equivalents: ≈ 36 lines minimal TX or RX; epoll not expressible (a
`notify_frame_available` tasklet callback writing the app's own eventfd, ≈ +15 lines); A/V/ANC
≈ 90 with ST 2110-10 epoch maths by hand and no common start; zero-copy TX ≈ 35; RX lent to a
framework needs app refcounts and an unsafe deferred free; a 1 → 4 split ≈ 60 per quad.

| Question to the maintainer | Answer now |
|---|---|
| 1. Does anyone fix and resubmit after a failed submit? | no: submit consumes the lease (D-88) |
| 2. Should open start the session? | yes: `mtl_session_open`; `mtl_session_create` + start for start arrays and attach |
| 3. Drop `-MTL_ETIMEDOUT` from data calls? | yes (R2) |
| 4. Results never throttle by default? | no: results always for app memory (D-77) |
| 5. A timeout-0 miss arms the fd? | yes (R2); cost one atomic add and fence on an empty poll |
| 6. `media_index` in frames for video, samples for audio, or two fields? | one field; audio indices count samples (`mtl_audio_config` comment) |
| 7. Eight teaching diagrams? | [diagrams.md](diagrams.md) keeps one picture per example under four rules |

**Samples and diagrams.** The study mapped every sample to a reduced set E0 (simple), E1 TX,
E2 RX, E3 A/V/ANC, E4 zero copy, E5 lend to a framework, E6 forwarder, E7 processor, E8
progressive, E9 event loop, with variants for ST 2110-22, audio, ANC, 2022-7, auto-detect,
the timing parser, conversion, user meta, dynamic ext frames, split, merge and 1 → 1 forwards,
downsampling, GPU memory, slices, MXL by index and shutdown; RTP-level and session samples stayed
on the legacy API then (D-25, since replaced by D-82) and `dma/dma_sample` is not a session
feature. Its diagram rules (one idea; at most 4 participants or 9 nodes; App, MTL, Network,
Framework only; names without arguments; happy path; no internals; no formulas in nodes; short
labels; return arrows only when they carry something; numbered steps; one vocabulary; next to
≤ 15 lines of code) (style rules S1–S12) and the 31 → 18 diagram plan (teach T1–T8, advanced A1–A6, internals I1–I4;
the MXL-by-index C6 and signal C11 pictures dropped) are superseded by [diagrams.md](diagrams.md) and [examples.md](examples.md).

## 6. Today's modes: asymmetries the unified API removes

From the operating-mode catalogue (all cells verified by header and `lib/src` grep at
`545a266a`): five families × two directions × up to three units × two layers, plus `st20rc`
(20 session and 8 pipeline create functions, each with its own ops struct and flag space). The
pipelines are what external integrators use (FFmpeg: st20p/st22p/st30p; GStreamer: st20p/st30p/
st40p; OBS: st20p; Python: st20p/st22p; Rust: st20/st20p/st22p/st30p); session level and st41
only `app/sample/` and RxTxApp. Pipeline flag bits are not aligned with session bits
(`ENABLE_RTCP` is bit 2 in `ST20_RX_FLAG_*`, bit 4 in `ST20P_RX_FLAG_*`), so each pipeline
translates flags one by one (`st20_pipeline_rx.c:508-546`). The mode-by-mode map is
[migration.md](migration.md) §4.16 and [implementation-plan.md](implementation-plan.md) §2.1.

| Feature | Where it exists today | Unified |
|---|---|---|
| exact user pacing | ST20, ST40, st20p, st40p | `MTL_SUBMIT_EXACT` on every essence by Phase 3 (OI-39) |
| user timestamp | all but st30p | `MTL_SUBMIT_RTP_TS` everywhere |
| epoch RTP, RTP delta | ST20 (+ delta on ST30, st20p, st30p) | default; `sc.media_time_offset_ns` everywhere |
| `notify_event` (VSYNC, recovery, fatal) | ST20, ST22 and their pipelines only | events for every essence |
| drop when late | pipelines only | `MTL_OPT_LATE_POLICY` everywhere |
| session stats | absent on ST22, st22p | one schema, ST22 included |
| queue meta | absent on st30p | M12 |
| RTCP | ST20, ST22 (st20p, st22p); no effect on ST30/40/41 | NACK for video and cvideo; CUT-7 |
| per-session pacing engine | ST30 and st30p only (video inherits the port's; ST22 always TSC, `st_tx_video_session.c:3396-3398`, `:3412-3414`) | `MTL_OPT_PACING` per session |
| zero-copy TX | partial; ST22 always copies; st20p derive only | reported per unit (`MTL_TXR_COPIED`, `info.direct`) |
| app memory TX | ST20 session, st20p, st22p | attached pools for every essence (Phase 4) |
| incomplete delivery | flag on ST20, ST22, ST30 and pipelines; ST40 always | `MTL_OPT_RX_INCOMPLETE` everywhere |
| auto-detect | ST20 (raster, fps, packing, interlace; not format), ST40 (interlace, on by default) | `v.detect`, `MTL_OPT_ANC_RX_DETECT` |
| timing parser | ST20 STAT/META, ST30 STAT/META (200 ms), st20p; not st30p | `MTL_OPT_RX_TIMING_PARSER` everywhere |
| DMA, header split, two RX threads, `rx_burst_size`, DISABLE_MIGRATE | ST20 and st20p only | options (header split CUT-1) |
| FORCE_NUMA | not ST40, ST41; st40p "NOT SUPPORTED" | `MTL_OPT_NUMA` everywhere |
| frame RX | not ST41 (RTP only) | frame RX for fastmeta |

Silent downgrades today, reported in the unified API: RL → TSC pacing fallback (AUTO, ST22,
shared TX queue), chain → copy TX, DMA → CPU, 2 → 1 port pruning; network headers fixed (TTL
64, DSCP 0, no VLAN, IPv4 only, MTU ≤ 1500), now `mtl_flow.dscp`, `.ttl`, `ip_family`, `vlan`
reserved. The catalogue's classification (v1, later, deprecate) is superseded by the phases
above; its 16 maintainer questions became the Q-MODE family ([decisions.md](decisions.md)).
