# Coverage: every legacy capability and its unified home

| | |
|---|---|
| Status | Maintained. The headers in [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/) are normative: where this page and a header disagree, the header wins |
| Date | 2026-10-02 |
| Baseline | `main` @ `545a266a`; legacy headers in `include/`; the 15 unified headers (their function counts, per header and per milestone tag: `sketch/check.sh`) |

This page is the safety net of port first (D-98): every use case today's public API offers, the
unified symbol that reaches it, and the milestone that brings it ([implementation-plan.md](implementation-plan.md)
§1.2). The U-row is the one home of a feature's milestone (D-137): other documents link it. A
function's milestone is its header tag and a value's its row in the data tables (README §4); a
U-row carries the latest of those it names.
Nothing may be lost: a capability has a home and a milestone, or it is on the cut list
(§4.5, D-87); nothing else is cut (D-112). §2 is the inventory, one row per use case (U-001 …
U-420), grouped by area; §4 is the inventory behind hiding the session-level headers. The
field-by-field maps are in [migration.md](migration.md) §4 (stats map §4.14, mode map §4.16).

## 1. How to read the inventory

Every installed header was read end-to-end (`include/*.h`, `include/experimental/st20_combined_api.h`;
the 47 + 58 functions of `st_convert_api.h` and `st_convert_internal.h` were counted, not read in
detail). Consumers were counted with `git grep -l -w <symbol>` per consumer tree, in files, not
call sites. A feature described only in prose, with no symbol to request it, has no home.

| Column | Meaning |
|---|---|
| U-ID | one use case; IDs are grouped by area, so numbers have gaps (U-414 is a note, not a use case) |
| Legacy | the capability and its legacy symbols; the header follows from the prefix (`mtl_` = `mtl_api.h`, `st_` = `st_api.h` or `st_pipeline_api.h`, `st20_`/`st22_` = `st20_api.h`, `st20p_`/`st22p_` = `st_pipeline_api.h`, `mtl_sch_` = `mtl_sch_api.h`, `st20rc_` = `experimental/st20_combined_api.h`) |
| Unified home | the header symbol, field (`sc.` = `struct mtl_session_config`, `ip.` = `struct mtl_instance_params`, `v.`/`c.`/`a.`/`n.`/`f.` = its video, cvideo, audio, anc, fastmeta member, `u.` = `struct mtl_unit`, `info.` = `struct mtl_session_info`), option key or stats key (contract.md §11.3) |
| When | the milestone that brings it to the unified API (MS1…MS7, Phase 7); "debug" = `mtl_debug.h`, debug builds; LATER = declared under `MTL_LATER` (not in v1), no milestone yet; "removed (D-87, CUT-n)" = on the cut list (§4.5) |
| Notes | class (**C** core: most apps; **Co** common: frameworks, gateways, test tools; **R** rare: tuning, debugging; **L** legacy-only), consumers, `S!` or `t`, and what differs; CHANGED = reachable with another model, unit or default |

A row served by a function carries the milestone tag that ends the function's comment in the
headers (two functions: both tags, the later one named). A row served by a value of an exported
call (a flag, an option key, an enum value) carries the milestone of the value's row in
`availability.def` or `mtl_options.def` (README §4), which wins if they differ. Until H1b the rule
for those rows is: MS1 when the scope of MS1 names it or when RxTxApp or KahawaiTest sets it; any
other ST 2110-20 value MS2.

**Consumer codes** (number = files): SMP `app/` (samples, perf, tools), RXTX `tests/tools/RxTxApp`,
FF FFmpeg plugin, GST GStreamer plugin, OBS OBS plugin, MXL `ecosystem/MTL_with_MXL`, PLG
`plugins/`, PY Python SWIG, RS Rust, KT KahawaiTest (`tests/integration_tests`), UT unit tests,
ext:bobi the public bobi.studio engine. "none" = no in-tree consumer.

**Session-header marks.** `S!` = reachable **only** through a session-level header (`st20_api.h`,
`st30_api.h`, `st40_api.h`, `st41_api.h`, `st_api.h`), no pipeline API offers it. `t` = a type,
enum or helper declared in a session-level header that pipeline users also need: hiding the
header without a replacement breaks every st20p/st22p/st30p/st40p consumer at compile time,
because the pipeline ops and frame structs embed these types (§4).

## 2. Inventory

### 2.1 Instance lifecycle and init parameters

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-001 | open an instance on a list of ports: `mtl_init`, `mtl_init_params.port[]`, `num_ports` | `mtl_instance_open(&ip, &mt)`, `ip.ports[].name`, `ip.port_count`; `MTL_PORTS` environment fallback | MS1 | C; every consumer (SMP 51, KT 8, MXL 8, PY 7, FF 4) |
| U-002 | close: `mtl_uninit` | `mtl_instance_close(mt, timeout_ns)` (closes the sessions and regions still open; 1 while memory is still referenced: call it again to poll) | MS1 | C; every consumer |
| U-003 | re-open in the same process (#1341, broken today) | open after close works by design (EAL keeps its arguments; same ports, a subset of the CPUs) | MS1 | Co; FF, GST, dvled work around it. CHANGED |
| U-004 | share one instance between components (`mtl_common.c:25-28`, `gst_mtl_common.c:12-18`) | `MTL_INSTANCE_SHARED`; a later open asking for a port the instance lacks, or other settings, is `-MTL_EEXIST` (`INSTANCE_MISMATCH`); a wrapper is the shared instance (D-192) | MS2 | Co; FF, GST, ext dvled. OBS and FFmpeg settings that break sharing: OI-43 |
| U-005 | device start/stop, auto start/stop: `mtl_start`, `mtl_stop`, `MTL_FLAG_DEV_AUTO_START_STOP` | no instance start; `mtl_session_start/stop`, `mtl_session_open` (create + start) | MS1 | C; SMP 35, KT 31, FF 6, MXL 7, PY 7. CHANGED; bridged instance before `mtl_start()`: OI-2 |
| U-006 | abort from a signal handler: `mtl_abort` | `mtl_instance_abort` (AS) and `mtl_instance_interrupt(mt, 1)` (AS) | MS1 | Co; SMP 3, RXTX 1 |
| U-007 | static source IP per port: `sip_addr[]`, `mtl_para_sip_set` | `mtl_port_spec.sip` | MS1 | C; all |
| U-008 | netmask per port: `netmask[]` | `mtl_port_spec.prefix_len` (0 = /24) | MS1 | Co; SMP 1, RXTX 2, RS 7 |
| U-009 | gateway per port: `gateway[]` | `mtl_port_spec.gateway` | MS1 | Co; SMP 1, RXTX 2, RS 7 |
| U-010 | DHCP on a DPDK port: `net_proto[] = MTL_PROTO_DHCP` | `MTL_OPT_DHCP` (`port.dhcp`); kernel backends: `sip` all zero | MS1 | R; SMP 1, RXTX 2 |
| U-011 | TX/RX queue counts: `tx_queues_cnt[]`, `rx_queues_cnt[]`, `st_tx/rx_sessions_queue_cnt` | `mtl_port_spec.tx_queues/rx_queues` (0 = auto), `MTL_OPT_MAX_QUEUES` | MS1 | C; SMP 3, RXTX 3, FF 2, GST 1, OBS 2, KT 3 |
| U-012 | force a port's NUMA node: `MTL_PORT_FLAG_FORCE_NUMA` + `socket_id` | `mtl_port_spec.numa` (node + 1) | MS1 | R; RXTX 1 |
| U-013 | bring up with ports link-down: `MTL_FLAG_ALLOW_DOWN_PORTS`, `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION` | the default: open never waits for links, legs are never pruned; `mtl_leg_status.oper`, `MTL_EVENT_PORT_LINK`, `mtl_instance_get_health` | MS1; MS3 (events, health) | Co; GST 2, RXTX 1. CHANGED |
| U-014 | ICE PF rate-limit burst devarg: `port_params[].rl_burst_size` | `MTL_OPT_RL_BURST` (`port.rl_burst`) | MS2 | R; none |
| U-015 | restrict MTL to an lcore list: `lcores` | `ip.lcores` | MS1 | Co; SMP 2, OBS 2, MXL 7, PY 7, RXTX 1 |
| U-016 | choose the DPDK main lcore: `main_lcore` | `MTL_OPT_MAIN_LCORE` | MS2 | R; none (#1179) |
| U-017 | log level at init: `log_level` | `mtl_instance_params.log_level` (legacy + 1; process-wide, mismatch rule); on a wrapper the legacy level; from MS2a also a sink's `min_severity` | MS1 | C; SMP 3, RXTX 2, FF, GST, OBS, MXL 7, RS 6, KT 5 |
| U-018 | log level at runtime: `mtl_set_log_level`, `mtl_get_log_level` | MS1: on the bridge, the legacy call; MS2a: add a sink with the new level, then remove the old one | MS1, MS2 | R; KT 1 |
| U-019 | route logs into the app's logger (#657): `mtl_set_log_printer` | `mtl_log_add_sink(&p, &h)`: several sinks, each line with its origin and name | MS2 | Co; RXTX 1, UT 3 |
| U-020 | custom line prefix: `mtl_set_log_prefix_formatter` | the `prefix` of a `fn` NULL sink | MS2 | R; RXTX 1. CHANGED |
| U-021 | log to a FILE: `mtl_openlog_stream` | a sink that writes to the application's FILE | MS2 | R; RXTX 1. CHANGED |
| U-022 | periodic stats dump with a callback: `stat_dump_cb_fn`, `dump_period_s`, `priv` | `MTL_OPT_STAT_DUMP_S` (`log.stat_dump_s`, R) + an exporter loop over `mtl_stat_list/read` ([migration.md](migration.md) §11.8) | MS2 | Co; RXTX 1, KT 1. CHANGED: no callback |
| U-023 | force/query IOVA mode: `iova_mode`, `mtl_iova_mode_get` | `MTL_OPT_IOVA_MODE` (`MTL_IOVA_VA/PA`); keys `caps.iova_va`, `instance.iova_mode` | MS1 (option); MS2 (keys) | R; RXTX 1, KT 1, SMP 1 |
| U-024 | RSS mode; query it: `rss_mode`, `mtl_rss_mode_get` | `MTL_OPT_RSS_MODE` (`MTL_RSS_NONE/L3/L3_L4`, legacy + 1) | MS1 | R; SMP 1, RXTX 2, PY 7, KT 1 |
| U-025 | schedulers for shared-RSS dispatch: `rss_sch_nb[]` | `MTL_OPT_RSS_SCHEDS` (`port.rss_scheds`) | MS1 | R; RXTX 1 |
| U-026 | descriptor ring sizes: `nb_tx_desc`, `nb_rx_desc` | `MTL_OPT_TX_DESC`, `MTL_OPT_RX_DESC` (per port) | MS1 | R; SMP 1, RXTX 1, MXL 3, PY 7, KT 1 |
| U-027 | RX mempool data room: `rx_pool_data_size` | `MTL_OPT_RX_POOL_DATA_SIZE` | MS1 | R; RXTX 1 |
| U-028 | memzone limit: `memzone_max` | `MTL_OPT_MEMZONE_MAX` | MS2 | R; none |
| U-029 | maximum UDP payload: `pkt_udp_suggest_max_size` | `sc.max_udp_payload` (per session, a typed field: the SDP's MAXUDP), its TX instance default `MTL_OPT_MAX_UDP_PAYLOAD` (`instance.max_udp_payload`); packet units `sc.packet.slot_bytes` | MS2 | R; none. CHANGED: per session, with an instance default |
| U-030 | ARP timeout: `arp_timeout_s` | `MTL_OPT_ARP_TIMEOUT_S` | MS1 | R; RXTX 1 |
| U-031 | name the DMA devices: `dma_dev_port[]`, `num_dma_dev_port`, `mtl_para_dma_port_set` | `MTL_OPT_DMA_DEVICES` (`instance.dma`, comma-separated) | MS1 | Co; SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 1 |
| U-032 | deprecated sizing: `tx_sessions_cnt_max`, `rx_sessions_cnt_max` | none | removed (D-87, CUT-11) | L; MXL 1; marked deprecated ("Use tx_queues_cnt") |
| U-033 | instance info and session counts: `mtl_get_fix_info`, `mtl_get_var_info`, `st_get_var_info` | keys `instance.*`, `instance.sessions{essence,dir}`; `mtl_instance_list_sessions`; `info.essence` | MS2 | R; RXTX 1, KT 7; t |
| U-034 | version string: `mtl_version()`, `MTL_VERSION` | `mtl_library_version()`, `mtl_library_version_num()`, `MTL_API_VERSION` | MS1 | Co; SMP 1, PY 1, RS 1, KT 1 |
| U-035 | is MtlManager alive: `mtl_is_manager_alive` | key `instance.manager`; `MTL_EVENT_MANAGER_LOST` | MS2 (key); MS3 (event) | R; none outside lib |
| U-036 | survive MtlManager loss (implicit shm fallback) | `MTL_OPT_CPU_ARBITRATION` (auto: MtlManager if present), `MTL_EVENT_MANAGER_LOST`, `MTL_REASON_MANAGER_LOST`, `MTL_REASON_MANAGER_REQUIRED` | MS1; MS5 (reconnect) | Co; the manager-optional instance flag is gone (D-92); inside an exclusive cpuset auto is none and a mounted MtlManager socket grants AF_XDP queues only |

### 2.2 Time source and PTP

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-040 | built-in PTP client: `MTL_FLAG_PTP_ENABLE` | `ip.time_source = MTL_TIME_SOURCE_PTP_BUILTIN`; on a VF it disciplines MTL's own time base as today (`mt_ptp.c:1390-1393`) | MS1 | Co; SMP 2, RXTX, FF, GST, PY 7, KT 2 |
| U-041 | PI servo and gains: `MTL_FLAG_PTP_PI`, `kp`, `ki` | `MTL_OPT_PTP_PI`, `MTL_OPT_PTP_PI_KP`, `MTL_OPT_PTP_PI_KI` (× 1e-9) | MS1 | R; RXTX 1, FF 1, RS 1 |
| U-042 | unicast delay requests: `MTL_FLAG_PTP_UNICAST_ADDR` | `MTL_OPT_PTP_UNICAST` (bool: to the learned master) | MS1 | R; RXTX 1, FF 1 |
| U-043 | built-in phc2sys: `MTL_FLAG_PHC2SYS_ENABLE` | `MTL_OPT_PHC2SYS` (never in a pod) | MS1 | R; SMP 1, RXTX 1 |
| U-044 | PTP time from TSC: `MTL_FLAG_PTP_SOURCE_TSC` | `MTL_OPT_PTP_SOURCE_TSC` | MS1 | R; SMP 1, RXTX 1, KT 1 |
| U-045 | app time function: `ptp_get_time_fn` + `priv` | `MTL_TIME_SOURCE_USER` + `mtl_time_set_reference` with `MTL_TIMEREF_USER_PAIR` (pushed (TAI, monotonic) pairs, not a pull); `MTL_TIME_SOURCE_CLOCK_TAI`, `_SYSTEM_TAI` | MS2 | Co; SMP 1, RXTX 2, FF 1 (CLOCK_TAI), KT 5. CHANGED |
| U-046 | notify on each sync: `ptp_sync_notify`, `mtl_ptp_sync_notify_meta` | `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_STEP`; keys `time.offset_ns`, `time.utc_offset_s`, `time.last_sync_age_ns` | MS3 | R; RXTX 1 |
| U-047 | HW RX timestamps: `MTL_FLAG_ENABLE_HW_TIMESTAMP` | `MTL_INSTANCE_HW_TIMESTAMP`, `MTL_OPT_HW_TIMESTAMPS`, `MTL_RXF_HW_ARRIVAL`, `MTL_PKTE_HW_ARRIVAL` | MS1 | R; SMP 1, RXTX 1, PY 1, RS 1, KT 4 |
| U-048 | read PTP/TAI time: `mtl_ptp_read_time`, `mtl_ptp_read_time_raw` | `mtl_time_now` (with the monotonic and realtime sample of the same instant), `mtl_time_convert` (`static inline` over one `mtl_time_now` sample) | MS1 | Co; SMP 2, RXTX 4, KT 11, ext:bobi |

### 2.3 Scheduler, lcores, threading

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-050 | schedulers as pthreads: `MTL_FLAG_TASKLET_THREAD` | `MTL_INSTANCE_TASKLET_THREAD` | MS1 | Co; RXTX 1, RS 1, KT 1 |
| U-051 | sleep when idle: `MTL_FLAG_TASKLET_SLEEP` | `MTL_INSTANCE_TASKLET_SLEEP` | MS1 | Co; RXTX 1 |
| U-052 | per-scheduler sleep at runtime: `mtl_sch_enable_sleep` | `MTL_OPT_SCHED_SLEEP_US` per scheduler (R; 0 = never) | MS1 | R; KT 1, UT 2 |
| U-053 | maximum sleep: `mtl_sch_set_sleep_us` | `MTL_OPT_SCHED_SLEEP_US` | MS1 | R; RXTX 1 |
| U-054 | separate lcore for RX video: `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` | `MTL_OPT_RX_SEPARATE_VIDEO_LCORE` | MS1 | Co; SMP 4, RXTX 2, FF 1, KT 3 |
| U-055 | migrate busy video sessions; opt out: `MTL_FLAG_TX/RX_VIDEO_MIGRATE`, `ST20(P)_RX_FLAG_DISABLE_MIGRATE` | `MTL_OPT_MIGRATE` (`session.migrate`): set on the instance, the default of its sessions (TX and RX); per session, the opt-out | MS2 | R; FF 1, RXTX 2, KT 1 ([migration.md](migration.md) §4.15) |
| U-056 | data quota per scheduler: `data_quota_mbs_per_sch` | `MTL_OPT_SCHED_QUOTA_MBS` | MS1 | Co; RXTX 1, MXL 5, KT 1 |
| U-057 | audio sessions per scheduler: `tx/rx_audio_sessions_max_per_sch` | `MTL_OPT_SCHED_TX_AUDIO_MAX`, `MTL_OPT_SCHED_RX_AUDIO_MAX` | MS1 | R; RXTX 2 |
| U-058 | tasklets per scheduler: `tasklets_nb_per_sch` | `MTL_OPT_TASKLETS_PER_SCHED` | MS2 | R; none |
| U-059 | dedicated system lcore: `MTL_FLAG_DEDICATED_SYS_LCORE` | `MTL_OPT_SYS_LCORE = MTL_SYS_DEDICATED` | MS1 | R; RXTX 1 |
| U-060 | CNI on a thread or tasklet: `MTL_FLAG_CNI_THREAD`, `_CNI_TASKLET` | `MTL_OPT_CNI` (`MTL_CNI_THREAD/TASKLET`) | MS1 | R; RXTX 1, RS 1, KT 3 |
| U-061 | tasklet time measurement: `MTL_FLAG_TASKLET_TIME_MEASURE` | `MTL_OPT_TASKLET_TIME_MEASURE`; key `sched.tasklet_p9999_ns` | MS1 (option); MS2 (key) | R; RXTX 1, RS 1 |
| U-062 | bind threads to the NIC NUMA or not: `MTL_FLAG_BIND_NUMA` (never read), `MTL_FLAG_NOT_BIND_NUMA` | bound by default; opt out `MTL_OPT_NO_BIND_NUMA` | MS1 | Co; BIND: SMP 2, FF, OBS 2, PY 7, RS 6; NOT_BIND: RXTX 1 |
| U-063 | do not bind the process: `MTL_FLAG_NOT_BIND_PROCESS_NUMA` | `MTL_OPT_NO_BIND_PROCESS_NUMA` | MS2 | R; none |
| U-064 | cores across NUMA nodes: `MTL_FLAG_ALLOW_ACROSS_NUMA_CORE` | `MTL_OPT_ACROSS_NUMA_CORES` | MS1 | R; RXTX 1, KT 1 |
| U-065 | 512-bit SIMD burst: `MTL_FLAG_RXTX_SIMD_512` | `MTL_OPT_SIMD_512` | MS1 | R; RXTX 1, KT 1, UT 1 |
| U-066 | query CPU SIMD level: `mtl_get_simd_level`, `_name` | key `instance.simd_level` (`enum mtl_simd`) | MS2 (key) | R; SMP 14 (print), RS 1, KT 1 |
| U-067 | borrow an MTL lcore: `mtl_get_lcore`, `mtl_bind_to_lcore`, `mtl_put_lcore` | none (CUT-5b) | removed (D-87, CUT-5b) | Co; SMP 14, RXTX 1, KT 1; perf tools move to the internal tier |
| U-068 | app-created scheduler: `mtl_sch_create/start/stop/free`, `mtl_sch_ops` | none | removed (D-87, CUT-10) | R; KT 1 (`sch_test.cpp`) |
| U-069 | app tasklet on an MTL scheduler: `mtl_sch_register_tasklet/unregister`, `mtl_tasklet_ops` (start, stop, handler, `advice_sleep_us`) | none (H-08) | removed (D-87, CUT-10) | R; KT 1 |
| U-070 | session's scheduler index: `st20/st22/st20p_*_get_sch_idx` | `info.sched_index` | MS1 | R; RXTX 1, KT 1; t |
| U-071 | name a thread: `mtl_thread_setname` | none (CUT-8) | removed (D-87, CUT-8) | R; RXTX 20 |
| U-072 | SysV shm lcore table print/clean: `mtl_lcore_shm_print`, `_clean` | none: the SysV table is removed (D-92); the calls report nothing and leave at the freeze | MS7 | L; SMP 1 (`app/tools/lcore_shmem_mgr.c`); `doc/design.md:51` advises against it |
| U-073 | sleep and busy-delay: `mtl_sleep_us`, `mtl_delay_us` | libc (CUT-8) | removed (D-87, CUT-8) | L; KT 1 / none |

### 2.4 Port data-path tuning (instance flags today)

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-075 | shared TX queue: `MTL_FLAG_SHARED_TX_QUEUE` | `MTL_OPT_TX_QUEUE = MTL_TXQ_SHARED` (`session.tx_queue`) on the instance: the default of its sessions (contract.md §12.4); a session's own wins | MS1 | Co; SMP 1, RXTX 1, RS 1. Today it forces TSC pacing for the whole port (`mt_dev.c:1446-1450`) |
| U-076 | shared RX queue: `MTL_FLAG_SHARED_RX_QUEUE` | `MTL_OPT_SHARED_RX_QUEUE` | MS1 | R; SMP 1, RXTX 1, RS 1 |
| U-077 | RX through the CNI queue: `MTL_FLAG_RX_USE_CNI` | `MTL_OPT_RX_USE_CNI` | MS1 | R; RXTX 1 |
| U-078 | flow rules on UDP port only: `MTL_FLAG_RX_UDP_PORT_ONLY` | `MTL_OPT_RX_UDP_PORT_ONLY` | MS1 | R; RXTX 1 |
| U-079 | no system RX queues: `MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES` | `MTL_OPT_NO_SYSTEM_RX_QUEUES` | MS1 | R; RXTX 1 |
| U-080 | promiscuous RX: `MTL_FLAG_NIC_RX_PROMISCUOUS` | `MTL_OPT_PROMISCUOUS` | MS1 | R; RXTX 1 |
| U-081 | no IGMP join (SDN fabrics): `MTL_FLAG_NO_MULTICAST` | `MTL_OPT_NO_MULTICAST` (`port.no_igmp`) | MS1 | R; RXTX 1 |
| U-082 | virtio_user exception path: `MTL_FLAG_VIRTIO_USER` | `MTL_OPT_VIRTIO_USER` | MS1 | R; RXTX 1 |
| U-083 | AF_XDP copy mode only: `MTL_FLAG_AF_XDP_ZC_DISABLE` | `MTL_OPT_AF_XDP_COPY` | MS1 | R; RXTX 1, KT 1 |
| U-084 | mono RX/TX mempool: `MTL_FLAG_RX_MONO_POOL`, `_TX_MONO_POOL` | `MTL_OPT_RX_MONO_POOL`, `MTL_OPT_TX_MONO_POOL` | MS1 | R; RXTX 1, KT 1 |
| U-085 | force TX copy (no chained mbuf): `MTL_FLAG_TX_NO_CHAIN` | `MTL_OPT_TX_COPY` (per session; with `MTL_SESSION_REQUIRE_DIRECT` `-MTL_EINVAL`) | MS1 | R; RXTX 1, KT 1. CHANGED: per session |
| U-086 | skip the TX burst check: `MTL_FLAG_TX_NO_BURST_CHK` | `MTL_OPT_TX_NO_BURST_CHECK` | MS1 | R; RXTX 1 |
| U-087 | random or multiple UDP source ports: `MTL_FLAG_RANDOM_SRC_PORT`, `_MULTI_SRC_PORT` | `MTL_OPT_SRC_PORT_MODE` (`MTL_SRC_PORT_RANDOM/MULTI`), per session | MS1 | R; RXTX 1, KT 2 |
| U-088 | pacing engine per port (AUTO/RL/TSC/TSN/PTP/BE/TSC_NARROW): `mtl_init_params.pacing`, `enum st21_tx_pacing_way` | `MTL_OPT_PACING` per session (`MTL_PACING_HW_RATE`, `_HW_LAUNCH`, `_SW`, `_SW_NARROW`, `_PTP`, `_BEST_EFFORT`); `caps.pacing_req` = `MTL_REQ_REQUIRE` fails create instead of falling back | MS1 | Co; RXTX, KT, FF. CHANGED; on the instance = the default; `info.pacing_class` |
| U-089 | UDP transport remnants: `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | none | removed (D-87, CUT-11) | L; SMP 1 (`sample_util.c:276` sets the flag, no effect), RS 1; stack deleted in `2b182cd87` |
| U-090 | simulated loss on redundant TX: `MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS`, `port_packet_loss[]` | `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` per session and leg | debug (MS1) | R; KT 1 |

### 2.5 Ports and backends

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-092 | DPDK PMD port (PCI BDF), PF or VF: `MTL_PMD_DPDK_USER` | `mtl_port_spec.name` = BDF; `MTL_BACKEND_DPDK_PMD` | MS1 | C; all |
| U-093 | native AF_XDP: `native_af_xdp:<if>` | name prefix; `MTL_BACKEND_AF_XDP`; `MTL_OPT_XSK_MAP` in pods | MS1 | Co; RXTX, KT |
| U-094 | kernel socket (experimental): `kernel:<if>` | name prefix; `MTL_BACKEND_KERNEL_SOCKET` | MS1 | R; RXTX, KT |
| U-095 | DPDK AF_XDP and AF_PACKET PMDs: `dpdk_af_xdp:`, `dpdk_af_packet:` | none; the prefix fails `mtl_instance_open` with `-MTL_ENOTSUP` (`MTL_REASON_BACKEND_REMOVED`), the detail naming the replacement | removed (D-87, CUT-12) | L; RXTX |
| U-096 | backend by name; DPDK-based or AF_XDP test: `mtl_pmd_by_port_name`, `mtl_pmd_is_dpdk_based`, `mtl_pmd_is_af_xdp` | key `caps.backend` (`enum mtl_backend`) after open; before open the name prefix says it | MS2 (key) | Co; SMP 1, RXTX 3, FF 1, GST 1, PY 7, KT 2. CHANGED |
| U-097 | port NUMA node: `mtl_get_numa_id` | key `caps.numa` | MS2 (key) | R; KT 1 |
| U-098 | port address in use (incl. a DHCP lease): `mtl_port_ip_info` | `mtl_port_get_spec` (address, prefix, gateway, MAC in use) | MS2 | R; RXTX 1, KT 1 |
| U-099 | port I/O counters: `mtl_get_port_stats`, `struct mtl_port_status` | keys `port.rx_pkts`, `tx_pkts`, `rx_bytes`, `tx_bytes`, `rx_errors`, `tx_errors`, `rx_missed`, `rx_nombuf` ([migration.md](migration.md) §4.14) | MS2 (keys) | Co; RXTX 1, ext:bobi |
| U-100 | reset port counters: `mtl_reset_port_stats` | none: cumulative counters, deltas by the reader | MS1 | R; RXTX 1. CHANGED |
| U-101 | interface IP helper: `mtl_get_if_ip` | none | removed (D-87, CUT-8) | L; 0 consumers |
| U-102 | init-param setters for bindings: `mtl_para_*_set/get`, `mtl_p_port`, `mtl_r_port`, `mtl_p/r_sip_addr` | plain POD fields, `MTL_ADDR(T)`, `MTL_PORTS` | MS1 | Co; PY 7, MXL 6, SMP 1, RXTX 1. CHANGED; spec strings are gone (D-97) |
| U-103 | Windows (DPDK only), `lib/windows/` | a queue's descriptor (`mtl_queue_create`) = a manual-reset event `HANDLE` (MS2a); Linux errno values on every OS; compile-only CI (G-74) | MS2 | R; MSVC sample |
| U-104 | many processes on one NIC (SR-IOV + MtlManager) | manager model unchanged; `MTL_OPT_CPU_ARBITRATION` | MS1 | Co; every multi-app deployment |

### 2.6 Memory, DMA and zero-copy plumbing

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-110 | hugepage alloc: `mtl_hp_malloc`, `mtl_hp_zmalloc`, `mtl_hp_free` | `mtl_mem_alloc` (region + va), `mtl_mem_close`; no malloc-style pointer API | MS2 | Co; SMP 24, RXTX 5, KT 4. CHANGED |
| U-111 | IOVA of memory or a plane: `mtl_hp_virt2iova`, `st_frame.iova[]`, `st_frame_iova`, `st_ext_frame.iova[]`, `st20_ext_frame.buf_iova` | `mtl_session_attach` (it takes regions, MTL maps them: no IOVA call, D-16); `mtl_mem_info.va` gives a library-pool plane's offset | MS2 | Co; SMP 18, PY 4, KT 2; t. CHANGED. Non-hugepage import IOVA: OI-41 |
| U-112 | DMA-map app memory: `mtl_dma_map`, `mtl_dma_unmap` | `mtl_mem_import`, `mtl_mem_close` | MS2 | Co; SMP 3, MXL 2, KT 5. Today it maps port P only (SF-07) |
| U-113 | page-aligned DMA block: `mtl_dma_mem_alloc/free/addr/iova` | `mtl_mem_alloc` + `mtl_session_attach` (no IOVA call) | MS2 | R; SMP 5, KT 2 |
| U-114 | page size and alignment: `mtl_page_size`, `mtl_size_page_align` | `mtl_mem_info.page_size`, keys `caps.page_size`, `caps.hugepage_sizes`; the application rounds to the page size itself | MS2 | R; SMP 3, MXL 1, KT 5 |
| U-115 | fast memcpy: `mtl_memcpy`, `mtl_memcpy_action` | `mtl_unit_copy_in/out`, `mtl_unit_copy_plane_in/out` for leased units only; a public `mtl_memcpy` is CUT-8 | removed (D-87, CUT-8) | Co; SMP 21, RXTX 15, FF 6, OBS 1, PLG 2, RS 2, PY 2, KT 4 |
| U-116 | user DMA engine: `mtl_udma_create/free/copy/fill/fill_u8/submit/completed` | none (CUT-5); sessions use DMA through `MTL_OPT_DMA` | removed (D-87, CUT-5) | R; SMP 15 (dma samples), KT 2 |
| U-117 | RX frames in GPU VRAM: `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`, `gpu_context`, `st20_rx_ops.gpu_direct_framebuffer_in_vram_device_address` | `mtl_mem_open` with `MTL_MEM_DEVICE` (`-MTL_ENOTSUP`, `NOT_IMPLEMENTED`, until then) | MS6 | R; SMP 1, FF 1 (session field: none); NG4 |

### 2.7 Session plumbing common to every essence

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-120 | create / free: `st*_tx/rx_create`, `*_free` | `mtl_session_create`, `mtl_session_open`, `mtl_session_close(s, timeout)` | MS1; MS4 (other essences) | C; every consumer |
| U-121 | live from create (no start) | explicit `mtl_session_start` (`struct mtl_when`: `MTL_NOW`, `MTL_AT_TAI`, `MTL_AT_INDEX`), `mtl_session_stop` (DRAIN, FLUSH); `mtl_session_open` keeps "live at once" | MS1; MS3 (`MTL_AT_TAI`, `MTL_AT_INDEX`) | C; all. CHANGED |
| U-122 | TX destination IP + UDP per leg: `dip_addr[]`, `udp_port[]`, `st_tx_port` | `sc.flows[i].ip`, `.udp_port`; `mtl_flow_ipv4()` | MS1 | C; all |
| U-123 | RX group or unicast source + UDP per leg: `ip_addr[]`, `udp_port[]`, `st_rx_port` | `sc.flows[i].ip`, `.udp_port` | MS1 | C; all |
| U-124 | SSM source filter: `mcast_sip_addr[]` | `sc.flows[i].source_filter` | MS1 | Co; RXTX 10, MXL 1 |
| U-125 | bind a leg to a named port: `port[][]` | `sc.flows[i].port` (`MTL_INDEX(port)`), `mtl_port_find` | MS1 | C; all |
| U-126 | ST 2022-7 dual-leg TX and RX merge: `num_port = 2` | `sc.flows[1]`, `sc.legs_disabled`, `mtl_leg_status`, `pkts_received[leg]` | MS1 | C; many |
| U-127 | payload type (RX 0 = off): `payload_type` | `sc.payload_type` (one value for every leg, ST 2022-7) | MS1 | C; all |
| U-128 | SSRC (TX 0 = random, RX 0 = off): `ssrc` | `sc.ssrc` (one SSRC for every leg, ST 2022-7) | MS1 | Co; RXTX 4, KT 5, RS 1 |
| U-129 | UDP source port: `udp_src_port[]` | `sc.flows[i].udp_src_port` | MS1 | R; none |
| U-130 | static destination MAC: `tx_dst_mac[]` + `*_TX_FLAG_USER_P_MAC/USER_R_MAC` | `MTL_FLOWF_USER_MAC` + `sc.flows[i].dst_mac` | MS1 | Co; SMP 2, RXTX 10 |
| U-131 | session name: `name` | `sc.name` (copied, unique, `""` = generated `<essence>_<tx or rx>_<n>`, OI-11) | MS1 | Co; many |
| U-132 | callback private pointer: `priv` | per-unit `u.cookie`; the session is `mtl_tx_result.session` / the event origin | MS1 | C; every callback user. CHANGED: a per-unit cookie only (OI-42) |
| U-133 | per-session NUMA: `*_FLAG_FORCE_NUMA` + `socket_id` (not ST40/41) | `MTL_OPT_NUMA` | MS1 | R; RXTX 11 |
| U-134 | dedicated TX queue for audio, ANC, fastmeta: `ST30/40/41(P)_TX_FLAG_DEDICATE_QUEUE` | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | MS4 | R; RXTX 1, KT 1 |
| U-135 | change TX destination at runtime: `st*_tx_update_destination`, `st_tx_dest_info` | `mtl_session_update(s, &sc, MTL_UPDATE_FLOWS, &when, &planned)` (every leg atomically) | MS5 (CREATED and STOPPED: MS3) | Co; MXL 1, KT 1; t |
| U-136 | change RX source at runtime: `st*_rx_update_source`, `st_rx_source_info` | `mtl_session_update(…, MTL_UPDATE_FLOWS, …)` | MS5 (CREATED and STOPPED: MS3) | Co; KT 1; t |
| U-137 | RX burst size: `rx_burst_size` | `MTL_OPT_RX_BURST` | MS1 | R; SMP 2, RXTX 3, PY 1 |
| U-138 | number of frame buffers: `framebuff_cnt` | `sc.pool_count`; `info.max_count` | MS1 | C; all |
| U-139 | app-managed flows and IGMP; queue IDs: `*_RX_FLAG_DATA_PATH_ONLY`, `*_rx_get_queue_meta`, `st_queue_meta` | none | removed (D-87, CUT-9) | L; flag: none; queue meta KT 3; t. Suspected NULL dereference (`mt_queue.c:56`, SP-02) |
| U-140 | pcapng capture of RX packets: `st20/st22/st20p/st22p/st20rc_rx_pcapng_dump`, `st_pcap_dump_meta` | `mtl_session_capture(s, &p)` / `mtl_session_capture(s, NULL)` (stop) + `MTL_EVENT_CAPTURE_DONE` (asynchronous) | MS4 | R; RXTX 1, KT 1; t. CHANGED: no synchronous mode |
| U-141 | NACK retransmission ST20/ST22: `*_FLAG_ENABLE_RTCP`, `st_tx_rtcp_ops.buffer_size`, `st_rx_rtcp_ops` | `MTL_OPT_RTX`, `_RTX_BUFFER_PKTS`, `_RTX_NACK_INTERVAL_US`, `_RTX_SEQ_BITMAP`, `_RTX_SEQ_SKIP`; keys `rtx.*` | MS2; MS4 (cvideo) | R; RXTX 1, KT 4; t |
| U-142 | RTCP flags on ST30/40/41/st40p | none (no effect today) | removed (D-87, CUT-7) | L; RXTX 1 passes them through |
| U-143 | TX hang-detect timeout: `tx_hang_detect_ms` | `MTL_OPT_TX_HANG_DETECT_NS` | MS2 | R; none. ANC and fastmeta have no detection today: OI-45 |
| U-144 | TX queue recovery / fatal: `ST_EVENT_RECOVERY_ERROR`, `ST_EVENT_FATAL_ERROR` (video only) | `MTL_EVENT_RECOVERY`, `MTL_EVENT_SESSION_STATE` to ERROR, `MTL_REASON_TX_QUEUE_FATAL`, `_HANG` (every essence) | MS2 (status); MS3 (events); MS6 (worker) | Co; RXTX 2, KT 2; t |
| U-145 | create fails with a reason (today NULL + a log line) | negative `MTL_E*`, `mtl_last_error`, `mtl_reasons.h` | MS1 | C; all |

### 2.8 TX timing and pacing (every essence)

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-150 | library-paced TX on the epoch grid (default) | `sc.media_mode = MTL_MEDIA_AUTO` | MS1 | C; all |
| U-151 | user pacing at a TAI time: `*_TX_FLAG_USER_PACING` + `timestamp`/`tfmt` | `MTL_MEDIA_TAI` (snapped), or `MTL_SUBMIT_NOT_BEFORE` + `u.launch_tai_ns` | MS1 (video) | Co; RXTX, GST 1, KT 4. MEDIA_CLK input is converted, not ignored |
| U-152 | exact user pacing: `ST20/ST20P/ST40/ST40P_TX_FLAG_EXACT_USER_PACING` | `MTL_SESSION_EXACT_LAUNCH` in `sc.flags` at create, then per unit `MTL_SUBMIT_EXACT` + `u.launch_tai_ns`; `MTL_INFO_NON_COMPLIANT` | MS2 (video) | R; RXTX 1, KT 1, UT 3; audio and fastmeta `-MTL_ENOTSUP` (`NOT_IMPLEMENTED`) until MS6 |
| U-153 | user-chosen RTP timestamp: `*_TX_FLAG_USER_TIMESTAMP` (absent on st30p) | `MTL_SUBMIT_RTP_TS` + `u.rtp` (TAI: the user's TAI in ticks, rounded to nearest, for the legacy RTP bytes; TAI mode alone if a snapped RTP is fine); needs INDEX or TAI (`RTP_TS_AUTO`); late units drop, not slip | MS3 (video, with E1) | Co; SMP 3, KT 2, UT 3. CHANGED |
| U-154 | RTP exactly on the epoch: `ST20(P)_TX_FLAG_RTP_TIMESTAMP_EPOCH` | the default (D-10) | MS1 | R; RXTX 1, UT 2 |
| U-155 | today's default ST20 RTP from the TX cursor (`st_tx_video_session.c`) | not the default (D-114); a positive `tx.rtp_trim_ns` (TRO − VRX0 × TRS) approximates it, flagged `MTL_INFO_RTP_OFF_GRID` | MS1; compared in the nightly (MS2, G-99) | Co; every legacy ST20 sender. CHANGED, wire-visible; mixed-API hazard ([migration.md](migration.md) §6.4) |
| U-156 | RTP delta / TROFF trim: `rtp_timestamp_delta_us` (st20, st20p, st30, st30p) | `tx.rtp_trim_ns` = 1000 × the delta (RTP = floor((M + trim) × rate), launch unchanged; AUTO, INDEX), `MTL_OPT_INDEX_OFFSET`, `sc.video.troffset_us` (whole µs; TROFFSET 0 cannot be signalled) | MS3; TR offset: MS1 (the default), MS2 (others) | R; RXTX 2, KT 2, ext:bobi |
| U-157 | drop late frames: `*P_TX_FLAG_DROP_WHEN_LATE` (pipelines, needs USER_PACING) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` (every mode) | MS1 | Co; RXTX 1, KT 1 |
| U-158 | late notification: `notify_frame_late(priv, epoch_skipped)` | `mtl_tx_reap` results: INDEX and TAI `MTL_TX_DROPPED` + `reason`, `margin_ns`; AUTO `MTL_TX_ON_TIME` + `MTL_TXR_DEFERRED`, `mtl_tx_result_full.indices_skipped_before` (`mtl_tx_reap_full`) | MS1 | Co; KT 1 |
| U-159 | per-epoch vsync: `*_FLAG_ENABLE_VSYNC`, `ST_EVENT_VSYNC`, `st10_vsync_meta` (ST20/22 only) | `MTL_OPT_EPOCH_TICK` + `MTL_EVENT_EPOCH_TICK` | MS3 | R; RXTX 1, KT 1; t |
| U-160 | ST 2110-21 sender type: `enum st21_pacing`, `transport_pacing` | `v.sender_type`, `c.sender_type` (`MTL_SENDER_N/NL/W`) | MS1 | Co; RXTX 2, MXL 2, KT 1; t |
| U-161 | TR offset, TRS, VRX: `st20_tx_get_pacing_params`, `st20p_tx_get_pacing_params` | keys `info.troffset_ns`, `info.trs_ps`, `info.vrx_full`; `info.min_submit_lead_ns` | MS1 | R; KT 3. CHANGED: keys, not typed fields |
| U-162 | RL tuning: `start_vrx`, `pad_interval`, `*_ENABLE_STATIC_PAD_P`, `*_DISABLE_BULK` | `MTL_OPT_VIDEO_START_VRX`, `_PAD_INTERVAL`, `_STATIC_PAD_P`, `_DISABLE_BULK` (the `video.*` keys also apply to cvideo) | MS1 | R; RXTX 2 |
| U-163 | next frame due, is a frame late: `st_frame_is_late`, `st30_frame_is_late`, `st40_frame_is_late`; RxTxApp `st_app_user_time()` | `mtl_tx_get_next` (`mtl_tx_next`), `mtl_epoch_index_at`, `mtl_tx_row_deadline` (MS2) | MS3 | Co; KT 1, RXTX |

### 2.9 Completion, notification and blocking

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-170 | blocking get with a timeout: `*P_*_FLAG_BLOCK_GET`, `*_set_block_timeout` | the `timeout_ns` argument of `mtl_tx_acquire`, `mtl_rx_dequeue` | MS1 | C; SMP 4, RXTX, FF, GST, PY 3, MXL 2, KT |
| U-171 | wake a blocked getter: `*_wake_block` | `mtl_session_interrupt(s, 1)` (sticky `-MTL_ECANCELED`, cleared with 0) | MS1 | C; SMP 4, RXTX, MXL 2, KT, ext NUDA9A |
| U-172 | non-blocking poll (get without BLOCK_GET → NULL) | timeout 0 → `-MTL_EAGAIN` (R2) | MS1 | Co; GST st40p (1 ms poll) |
| U-173 | "frame available": `notify_frame_available` | `mtl_session_wait`; for an event loop a queue report (`mtl_queue_arm`; eventfd, HANDLE) | MS1 (WT), MS2a (queues) | Co; SMP 10, OBS 1, MXL 1, PLG 2, RS 1, KT 4 |
| U-174 | pipeline TX done with status: `notify_frame_done(priv, st_frame*)` | `MTL_SESSION_RESULTS` + `mtl_tx_reap` (`mtl_tx_result.status`) | MS1 | Co; SMP 14, GST 1, MXL 1, KT 10 |
| U-175 | session TX done by index: `notify_frame_done(priv, idx, meta)` | `mtl_tx_result.slot`, `mtl_tx_reap` | MS1 | Co; SMP, RXTX 5, RS 1; S! |
| U-176 | zero-hop callback on the tasklet | `mtl_session_set_inline_notify` (`mtl_events.h`, `MTL_LATER`, D-04); meanwhile busy polling with timeout 0 | LATER | R; MXL (bridge), ext:bobi (slice); S! (H-15) |
| U-177 | session event callback: `notify_event(priv, st_event, args)` | `mtl_session_read_events` (an event loop arms many sessions on one queue, MS2a) | MS3 | Co; RXTX 2, KT 2; t |
| U-178 | abort a got frame: `*_put_frame_abort` | `mtl_tx_release`, `mtl_rx_release` | MS1 | Co; KT 1, UT 2 |

### 2.10 ST 2110-20 video TX

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-180 | pipeline TX loop: `st20p_tx_get_frame`, `st20p_tx_put_frame` | `mtl_tx_acquire` + `mtl_tx_submit`; `mtl_session_open` | MS1 | C; SMP 9, FF, GST, OBS, MXL 2, PY 2, RS, KT 4 |
| U-181 | session pull model: `st20_tx_ops.get_next_frame` + `st20_tx_frame_meta` | push: acquire and submit (D-02); the meta becomes `struct mtl_unit` + `mtl_tx_get_next` | MS1; MS3 (`mtl_tx_get_next`) | Co; SMP 8, RXTX 1, RS 1, KT 12; S!. CHANGED |
| U-182 | session framebuffers by index: `st20_tx_get_framebuffer/_size/_count` | `mtl_session_get_slot`, `mtl_tx_acquire_slot`, `info.unit_bytes`, `info.pool_count` | MS1; MS2 (by index) | Co; SMP 4, RXTX 1, RS 1, KT 6; S! |
| U-183 | pipeline framebuffer address and size: `st20p_tx_get_fb_addr`, `st20p_tx_frame_size` | as U-182 | MS1 | Co; SMP 4, FF, GST, MXL 2, PY 2, KT 2 |
| U-184 | session TX app frames per index: `ST20_TX_FLAG_EXT_FRAME`, `st20_tx_set_ext_frame`, `st20_ext_frame` | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` + `mtl_tx_acquire_slot` | MS2 | Co; SMP 5, KT 3; S! |
| U-185 | pipeline TX app memory per frame (any address): `ST20P_TX_FLAG_EXT_FRAME`, `st20p_tx_put_ext_frame`, `st_ext_frame` | attached slots (≤ `info.max_count`); a new layout per acquire `mtl_tx_acquire_layout`; from caller memory without a slot of its own, `MTL_SUBMIT_SRC_PLANES` (a copy or the conversion during submit, MS1) | MS2 | Co; SMP 2, GST 1, KT 1 (D-56) |
| U-186 | two-phase release: `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE`, `st20p_tx_notify_ext_frame_free` | app memory always produces results: the result frees the slot, `mtl_tx_acquire_slot` takes it again | MS2 | Co; SMP 2, GST 1, KT 1, UT 3 |
| U-187 | interlaced TX (`interlaced`, `second_field`) | `raster.scan = MTL_INTERLACED`: the unit is a field, parity from the media index | MS1 | Co; many |
| U-188 | line padding: `linesize`, `transport_linesize` | `v.linesize[]` (library pools), `mtl_plane.stride`, `mtl_attach` strides | MS2 | Co; SMP 7, GST 2, OBS 1, MXL 1, KT 4 |
| U-189 | split-forward TX tiles from an RX buffer (`app/sample/fwd/rx_st20_tx_st20_split_fwd.c:127-131`) | `mtl_session_get_pool_region` + `mtl_session_attach` + `u.hold`, `mtl_tx_send_slot` (ex09) | MS2 | R; SMP |
| U-190 | per-frame user metadata: `st20_tx_frame_meta.user_meta`, `st_frame.user_meta` | meta record `MTL_META_USER` (≤ 1332 B, `tag`, `tag_version`) | MS1 | Co; SMP 7, RXTX 4, KT 1 |
| U-191 | slice TX: `ST20_TYPE_SLICE_LEVEL`, `query_frame_lines_ready`, `st20_tx_slice_meta` | `MTL_UNIT_ROWS`: resubmit the lease with a larger `u.used`; `mtl_tx_row_deadline`, `MTL_OPT_ROWS_LATE` | MS2 | R; SMP 1, RXTX 1, KT 2, ext:bobi; S!. CHANGED: push, not a pull callback |
| U-192 | packing BPM / GPM / GPM_SL: `packing`, `transport_packing` | `v.packing` (`enum mtl_packing`, same values) | MS1 | Co; SMP 2, RXTX 2, MXL 2, PY 2; t |
| U-193 | RFC 4175 transport formats (16 values): `enum st20_fmt` | `enum mtl_video_format` (all 16, legacy + 1) | MS1 | Co; many; t |
| U-194 | non-RFC 4175 transport (planar 10LE, V210): `ST20_FMT_YUV_422_PLANAR10LE`, `ST20_FMT_V210` | `MTL_YUV422P10LE_NONSTD`, `MTL_V210_NONSTD` (`mtl_video_format_ext`; V210: the legacy pgroup layout, rows of width / 6 × 16 B; framework v210 only at widths that are a multiple of 48), `MTL_INFO_NON_COMPLIANT` | MS1 | L→kept; RXTX 2, KT 1 |
| U-195 | resolution and frame rate: `width`, `height`, `enum st_fps` | `struct mtl_raster` (`fps`, a rational only; `mtl_fps_rational(MTL_FPS_*)` for the named ones, adds 47.95, 48) | MS1 | C; all; t |
| U-196 | size, pixel group, bandwidth helpers: `st20_frame_size`, `st20_get_pgroup`, `st20_pgroup`, `st20_get_bandwidth_bps`, `st20_1080p59_yuv422_10bit_bandwidth_mps` | `mtl_format_describe` (`plane_bytes[]`, `pg_bytes`, `pg_pixels`); `mtl_session_info.wire_bps` (from `mtl_session_query` or `mtl_session_get_info`); `mtl_session_query(…, req)` | MS1 | R; SMP 6, RXTX 4, KT 8; t |
| U-197 | transport format names: `st20_fmt_name`, `st20_name_to_fmt` | `mtl_format_describe` (names), `mtl_format_parse` | MS1; MS2 (`mtl_format_parse`) | R; SMP 1, PY 1; t |

### 2.11 ST 2110-20 video RX

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-200 | pipeline RX loop: `st20p_rx_get_frame`, `st20p_rx_put_frame` | `mtl_rx_dequeue` + `mtl_rx_release` | MS1 | C; SMP 12, FF 2, GST, OBS, MXL, PY 3, RS, KT 4 |
| U-201 | session push callback, return later or reject: `notify_frame_ready` + `st20_rx_put_framebuff` | `mtl_rx_dequeue` / `mtl_rx_release` (any thread, any order) | MS1 | Co; SMP 5, RXTX 4, MXL 4, RS 1, KT 11; S!. CHANGED |
| U-202 | deliver incomplete frames: `*_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | `MTL_OPT_RX_INCOMPLETE`: DELIVER is the default (D-114), `MTL_RX_DISCARD` opt-in | MS1 | Co; SMP 1, RXTX 1, MXL 2, KT 6, ext:bobi. CHANGED, wire-visible default |
| U-203 | frame status: `enum st_frame_status`, `st_is_frame_complete` | `u.status` (`MTL_RX_COMPLETE/INCOMPLETE`), `MTL_UNITF_USED_REDUNDANCY` (= RECONSTRUCTED) | MS1 | C; SMP 5, RXTX 2, MXL 2, KT 4; t |
| U-204 | packets per leg: `pkts_total`, `pkts_recv[]` | `mtl_rx_get_detail` (`pkts_expected`, `pkts_received[]`, `pkts_recovered`, `missing_ranges`) | MS2 | Co; KT, RXTX |
| U-205 | first/last packet arrival: `timestamp_first_pkt`, `timestamp_last_pkt`, `receive_timestamp` | `mtl_rx_detail.arrival_first_tai_ns[]`, `arrival_last_tai_ns[]` | MS2 | Co; RXTX 4, GST, OBS |
| U-206 | RX RTP and media time: `rtp_timestamp`, `timestamp` + `tfmt` | `u.rtp`, `u.media_tai_ns`, `u.media_index` (`MTL_UNITF_*_VALID`) | MS1; MS3 (`media_index`) | C; GST, OBS, RXTX |
| U-207 | bytes per frame: `frame_total_size`, `frame_recv_size`, `uframe_total_size` | `mtl_rx_detail.bytes_received` | MS2 | R; KT |
| U-208 | RX into fixed app buffers: `st20_rx_ops.ext_frames[]`, `st20p_rx_ops.ext_frames` | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach` | MS2 | Co; SMP 4, KT 6 |
| U-209 | RX into an app buffer chosen per frame: `query_ext_frame` (st20, st20p, st22p) + `*_RX_FLAG_EXT_FRAME` | fixed sets: attached pool (`RX_BY_INDEX` for MXL); per-unit destinations: `mtl_rx_provide` (D-152); frameworks: wrapping (D-150) | MS1 (wrap), MS2 | Co; SMP 1, GST 1, MXL 3, KT 3 (H-09) |
| U-210 | per-buffer identity: `st20_ext_frame.opaque`, `st_frame.opaque` | `u.slot` (stable index, D-76) | MS1 | Co; GST, MXL. The slot index is the identity (D-76) |
| U-211 | RX DMA offload: `ST20(P)_RX_FLAG_DMA_OFFLOAD`, `st20_rx_dma_enabled` | `MTL_OPT_DMA`, `MTL_OPT_DMA_DEVICES`, `MTL_PATH_DIRECT_DMA`, `rx.pkts_dma` | MS2 | Co; RXTX 1, FF 1 (flag misused, `mtl_st20p_rx.c:176`), GST 2, KT 5 |
| U-212 | auto-detect raster, fps, packing, interlace: `*_RX_FLAG_AUTO_DETECT`, `notify_detected`, `st20_detect_meta/reply` | `MTL_DETECT_ON` with the raster as the maximum; `MTL_UNITF_FORMAT_CHANGED`, `mtl_rx_detail.format_seq` and `raster`, `MTL_EVENT_RX_FORMAT`, `rx.detected.*`; the `notify_detected` reply's `slice_lines` → `rx.rows_step` | MS3 | R; SMP 1, RXTX 1, KT 1; t (H-06) |
| U-213 | header split: `*_RX_FLAG_HDR_SPLIT`, `nb_rx_hdr_split_queues` | none (CUT-1) | removed (D-87, CUT-1) | L; SMP 1, RXTX 1, KT 1; needs a DPDK patch absent for 26.07; single port, BPM only |
| U-214 | ST 2110-21 timing parser in stats: `*_TIMING_PARSER_STAT` | `MTL_OPT_RX_TIMING_PARSER`, keys `tp.*` | MS2 | R; RXTX 1 |
| U-215 | timing result per frame and port: `ST20(P)_RX_FLAG_TIMING_PARSER_META`, `st20_rx_tp_meta`, `st_frame_tp_meta` | `mtl_rx_get_detail(s, lease, …)`, `timing[leg]` (`MTL_RXF_TIMING_LEG*`): compliance, `failed_cause`, cinst, vrx, `fpt_ns`, latency, RTP offset and delta, ipt | MS2 | R; SMP 1, PY 1, KT 2, UT 1, ext:bobi; t (H-07) |
| U-216 | pass thresholds: `st20(p)_rx_timing_parser_critical`, `st20_rx_tp_pass` | keys `tp.pass.*` | MS2 | R; SMP 1; t |
| U-217 | two RX threads above 40 Gb/s: `*_RX_FLAG_USE_MULTI_THREADS` | `MTL_OPT_RX_THREADS` (an explicit 2 with two legs or rows units: `-MTL_ENOTSUP`) | MS2 | R; RXTX 1 |
| U-218 | slice RX: `ST20_TYPE_SLICE_LEVEL`, `slice_lines`, `notify_slice_ready`, `st20_rx_slice_meta` | dequeue at the first rows with `MTL_UNITF_PARTIAL`, then `mtl_rx_wait_rows`; step `MTL_OPT_RX_ROWS_STEP` | MS2 | R; SMP 1, RXTX 1, KT 3, ext:bobi; S!. CHANGED |
| U-219 | app converts pixel groups on the tasklet: `uframe_size`, `uframe_pg_callback`, `st20_rx_uframe_pg_meta` | none (CUT-2: app code on a tasklet) | removed (D-87, CUT-2) | L; RXTX 1, KT 3; S! |
| U-220 | per-packet conversion in st20p: `ST20P_RX_FLAG_PKT_CONVERT` (3 formats) | `MTL_OPT_RX_CONVERT_PER_PACKET` (library code on the RX tasklet) | MS2 | L→kept; KT 1 (H-16) |
| U-221 | RX user metadata: `st20_rx_frame_meta.user_meta` | meta record `MTL_META_USER` | MS1 | Co; SMP 7, RXTX 4 |
| U-222 | first-packet time per frame: `st20_rx_frame_meta.fpt` | `mtl_rx_timing_result.fpt_ns` (parser on); else `arrival_first_tai_ns − media_tai_ns` | MS2 | R; none |
| U-223 | simulated RX loss: `*_RX_FLAG_SIMULATE_PKT_LOSS`, `burst_loss_max`, `sim_loss_rate` | `mtl_debug_inject` with `MTL_FAULT_DROP_PKTS` (pattern) or `MTL_FAULT_DROP_RANDOM` (rate) | debug (MS1) | R; KT 4 |
| U-224 | RX framebuffer size and count: `st20_rx_get_framebuffer_size/_count`, `st20p_rx_get_fb_addr`, `st20p_rx_frame_size` | `info.unit_bytes`, `info.pool_count`, `mtl_session_get_slot` | MS1 | Co; SMP 4, RXTX 1, FF, GST, RS 1 |

### 2.12 ST 2110-22 compressed video

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-230 | pipeline TX through a codec plugin: `st22p_tx_ops` (`input_fmt`, `codec`, `quality`, `codestream_size`, `codec_thread_cnt`, `device`) | `sc.essence = MTL_CVIDEO`: `c.codec`, `c.app_format`, `c.codestream_bytes`; `MTL_OPT_CVIDEO_QUALITY`, `_THREADS`; `MTL_OPT_VIDEO_CONVERT_DEVICE` (the codec device) | MS4 | Co; SMP 2, RXTX 1, FF 1, PY 1, RS 1, KT 1 |
| U-231 | pipeline RX decoded: `st22p_rx_ops` (`output_fmt`, `max_codestream_size`) | `struct mtl_cvideo_config` (RX) | MS4 | Co; SMP 1, RXTX 1, FF 1, PY 1, RS 1, KT 1 |
| U-232 | codestream passthrough: st22p with a codestream format; `framebuff_max_size`, `st22_tx_frame_meta.codestream_size` | `c.app_format = 0`, `u.used` (bytes) | MS4 | Co; SMP 3, RXTX 2, MXL 2, KT 2 |
| U-233 | constant vs variable bytes per frame (today VBR-like) | `c.rate_mode`: `MTL_CVIDEO_CBR` default (padding), `MTL_CVIDEO_VBR_MAX` = today (non-compliant) | MS4 | Co; every ST22 user. CHANGED, wire-visible (D-114, D-32) |
| U-234 | JPEG-XS box headers off: `ST22(P)_TX/RX_FLAG_DISABLE_BOXES` | none (CUT-6); `info.box_hdr_bytes` stays reported | removed (D-87, CUT-6) | R; no consumer (st22p forces it for non-JPEG-XS codecs) |
| U-235 | packetization mode: `pack_type` (SLICE unsupported) | `MTL_OPT_CVIDEO_PACK` (`enum mtl_cvideo_pack`) | MS4 | Co; FF 1; t |
| U-236 | session-level ST22: `st22_tx/rx_ops`, `st22_tx_get_fb_addr`, `st22_rx_get_fb_addr`, `st22_rx_put_framebuff` | as U-181, U-201 | MS4 | R; SMP 1, RXTX 1, MXL 1, KT 1; S!. CHANGED |
| U-237 | interlaced ST22 (a codestream per field) | `c.raster.scan` (CBR per field) | MS4 | R; RXTX |
| U-238 | ST22 pipeline app memory TX / RX: `ST22P_TX_FLAG_EXT_FRAME` + `st22p_tx_put_ext_frame`; `ST22P_RX_FLAG_EXT_FRAME` + `query_ext_frame` | as U-185, U-209 | MS4 | R; KT 1 |
| U-239 | deliver incomplete ST22 frames | `MTL_OPT_RX_INCOMPLETE` | MS4 | R; none |

### 2.13 ST 2110-30/31 audio

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-240 | PCM8/16/24 and AM824: `enum st30_fmt` | `enum mtl_audio_format` | MS4 | C; FF, GST, RXTX, KT; t |
| U-241 | 48 / 96 / 44.1 kHz: `enum st30_sampling` | `a.sample_rate` (Hz) | MS4 | C; all audio; t |
| U-242 | channel count: `channel` | `a.channels` | MS4 | C; all audio |
| U-243 | packet time incl. ST 2110-31 values (1 ms, 125/250/333 µs, 4 ms, 80 µs, 1.09/0.14/0.09 ms): `enum st30_ptime` | `a.ptime` (`enum mtl_ptime`, legacy + 1; 0 = 1 ms) | MS4 | C; all audio; t. Non-integer samples (80 µs at 48 kHz): 4 samples paced at 4 / Fs, the granted packet time in `info.ptime_ps` |
| U-244 | pipeline get/put: `st30p_tx/rx_get_frame`, `put_frame`, `struct st30_frame` | `mtl_tx_acquire`/`mtl_tx_submit` (`u.used` bytes), `mtl_rx_dequeue`, `mtl_tx_write` | MS4 | C; SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 2 |
| U-245 | buffer size for a frame duration: `st30_calculate_framebuff_size`, `framebuff_size` | `a.unit_samples` (0 = 10 ms); `info.buffer_capacity_bytes`, `info.unit_samples` from `mtl_session_query` | MS4 | Co; SMP 2, RXTX 2, FF 2, GST 2, RS 1, KT 2; t |
| U-246 | size/time math: `st30_get_packet_size`, `_sample_size`, `_sample_num`, `_sample_rate`, `_packet_time` | `mtl_audio_bytes`, `mtl_session_query` | MS4 | Co; RXTX 2, KT 5, UT 1; t |
| U-247 | per-session pacing engine AUTO/RL/TSC: `pacing_way` | `MTL_OPT_PACING` | MS4 | Co; RXTX 1, FF 2, PY 7, KT 1; t |
| U-248 | RL warm-up: `rl_accuracy_ns`, `rl_offset_ns` | `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` | MS4 | R; RXTX 1 |
| U-249 | pace in the builder too: `ST30_TX_FLAG_BUILD_PACING` | `MTL_OPT_AUDIO_BUILD_PACING` | MS4 | R; RXTX 1; S! |
| U-250 | builder-to-pacer FIFO: `fifo_size` (packets) | `MTL_OPT_AUDIO_FIFO_MS` (ms, default 10) | MS4 | R; RXTX 1. CHANGED: unit |
| U-251 | audio user timestamp: `ST30_TX_FLAG_USER_TIMESTAMP` (no st30p flag) | media mode TAI or INDEX + `MTL_SUBMIT_RTP_TS`; `MTL_OPT_AUDIO_ABSORB_SAMPLES` | MS6 | Co; KT 1, UT 3; S! |
| U-252 | audio user pacing: `ST30(P)_TX_FLAG_USER_PACING` | `MTL_MEDIA_TAI` (+ `MTL_SUBMIT_NOT_BEFORE` in MS6) | MS6 | Co; RXTX 1, GST 1, KT 3 |
| U-253 | lost packets read as silence: `ST30(P)_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | `MTL_OPT_RX_INCOMPLETE`; zero fill of library pools by default (`MTL_SESSION_RX_NO_FILL` opts out) | MS4 | R; KT 1, UT 1 |
| U-254 | audio timing parser (dpvr, ipt, tsdf per 200 ms): `ST30_RX_FLAG_TIMING_PARSER_STAT/META`, `notify_timing_parser_result`, `st30_rx_tp_meta` | `MTL_OPT_RX_TIMING_PARSER`; per unit `mtl_rx_detail.timing[]` (`dpvr_max_ns`, `ipt_max_ns`, `tsdf_ns`); keys `tp.*` | MS4 | R; RXTX 1; S!. CHANGED: per unit, not a 200 ms callback |
| U-255 | AM824 / AES3 subframe layouts: `struct st31_am824`, `st31_aes3` (+ convert `st31_*`) | `struct mtl_am824_subframe`; `mtl_convert` between `MTL_AM824` and `MTL_AES3` (`MTL_FORMAT_AUDIO`, one plane) | MS4 | R; KT; t |
| U-256 | deprecated `sample_size`, `sample_num` | none ("Not use anymore, plan to remove") | removed (D-87, CUT-11) | L; none |
| U-257 | audio RX arrival time: `timestamp_first_pkt`, `receive_timestamp` | `mtl_rx_detail.arrival_first_tai_ns[]` | MS4 | Co; RXTX |
| U-258 | session-level audio: `st30_tx_ops.get_next_frame`, `st30_tx_get_framebuffer`, `st30_rx_ops.notify_frame_ready`, `st30_rx_put_framebuff` | as U-181, U-201 | MS4 | R; RXTX 4, KT 2; S!. CHANGED |

### 2.14 ST 2110-40 ancillary

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-260 | ANC TX/RX with a packet table + UDW: `st40p_*`, `st40_frame_info`, `struct st40_meta` | `MTL_ANC`: plane 0 = `struct mtl_anc_packet` table, plane 1 = the words, plane 2 (RAW) = the header words, `u.used` = entries (contract §5.7) | MS4a2 | Co; SMP 1, RXTX 1, GST 1, KT 3; t. CHANGED (the table moved from the meta area to plane 0). `stream` byte: bit 7 = S, bits 0–6 = StreamNum |
| U-261 | packets per frame: `ST40_MAX_META = 20` (excess truncated) | `n.max_packets` (1–65 535, default 32) | MS4a2 | Co; all ANC; t. CHANGED |
| U-262 | UDW capacity: `max_udw_buff_size`, `framebuff_size`, `st40p_*_max_udw_buff_size`, `get_udw_buff_addr` | `n.max_udw_words` (0 = max(255, 32 × `max_packets`)), `u.plane[1]`, `info.buffer_capacity_bytes` | MS4a2 | Co; SMP 2, RXTX 2, GST 2, KT 2 |
| U-263 | one ANC packet per RTP packet: `ST40(P)_TX_FLAG_SPLIT_ANC_BY_PKT` | `MTL_ANCF_NEW_RTP` per entry | MS4a2 | Co; SMP 1, GST 2, KT 1; t. CHANGED (flag → per entry) |
| U-264 | RX interlace auto-detect and its off switch: `ST40(P)_RX_FLAG_DISABLE_AUTO_DETECT` | `sc.anc.detect` (`MTL_DETECT_AUTO` = on; `MTL_DETECT_OFF`) | MS4 | Co; GST 1. A typed field like `v.detect` (D-105) |
| U-265 | interlaced ANC TX (`second_field`) | `n.video.scan` + index parity | MS4 | Co; RXTX, KT |
| U-266 | per-port sequence loss and discontinuity, marker: `port_seq_lost[]`, `port_seq_discont[]`, `seq_lost`, `seq_discont`, `rtp_marker` | `mtl_rx_detail.marker_seen`, `seq_discont[leg]`, `pkts_received[leg]`, `units_missing_before`; key `leg.pkts_lost` | MS4 | R; KT, GST |
| U-267 | TX test mutations: `st40_tx_test_config` in `st40_tx_ops` / `st40p_tx_ops` | `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` + `enum mtl_tx_mutation` | debug (MS4) | R; GST (test element), KT; t |
| U-268 | RFC 8331 helpers: `st40_get_udw`, `st40_set_udw`, `st40_calc_checksum`, `st40_add/check_parity_bits`, `st40_rfc8331_*` | `mtl_anc_udw_get/set`, `mtl_anc_parity`, `_parity_ok`, `mtl_anc_checksum`, `mtl_anc_rfc8331_bytes`, `_encode`, `_decode`, `mtl_anc_table`, `_words`, `_raw_hdr`, `_put` (`mtl_util.h`, `static inline`) | MS1 | Co; RXTX 2, GST 1, KT 2, UT 3; S! (H-13) |
| U-269 | session-level ANC: `st40_tx_ops.get_next_frame` + `st40_frame`, `st40_rx_ops.notify_frame_ready`, `st40_rx_put_framebuff` | as U-181, U-201 | MS4 | R; RXTX 1, KT 2, UT 5; S!. CHANGED |
| U-270 | ANC user pacing, timestamp, exact: `ST40(P)_TX_FLAG_USER_PACING/USER_TIMESTAMP/EXACT_USER_PACING` | as U-151 … U-153 | MS6 | R; RXTX 1, GST 1, KT 2 |
| U-271 | `ST40P_*_FLAG_FORCE_NUMA` ("NOT SUPPORTED YET", `st40_pipeline_api.h:117-121`) | none | removed (D-87, CUT-11) | L; none |
| U-272 | `st40p_rx_ops.rtp_ring_size` (documented mandatory, unused) | none | removed (D-87, CUT-11) | L; none |
| U-420 | ANC with 10-bit words or verbatim relays | `n.word_mode` 10BIT, RAW | MS4a2 | new (SF-78) |

### 2.15 ST 2110-41 fast metadata (no pipeline API today)

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-280 | TX one data item per frame: `st41_tx_ops.get_next_frame`, `st41_frame`, `st41_tx_get_framebuffer` | `sc.essence = MTL_FASTMETA`, `u.used` | MS4 | R; RXTX 1; S! |
| U-281 | data item type and K bit: `fmd_dit`, `fmd_k_bit` | `f.data_item_type`, `f.k_bit` | MS4 | R; RXTX 3; S! |
| U-282 | RX filter on DIT / K (`0xffffffff` / `0xff` = off) | `MTL_FASTMETA_RX_MATCH_DIT`, `_MATCH_K` | MS4 | R; RXTX 3; S!. Today the filter cannot change at runtime: `st41_rx_update_source` takes `st_rx_source_info` (`include/st_api.h:159`), which has no filter field; unified, it changes with `MTL_UPDATE_MEDIA` in CREATED or STOPPED |
| U-283 | RX (RTP only today): `st41_rx_ops.notify_rtp_ready` + `st41_rx_get_mbuf` | frame-level RX is new; the raw path is U-345 | MS4 | R; RXTX 1; S!. CHANGED |
| U-284 | user pacing, timestamp: `ST41_TX_FLAG_USER_PACING/USER_TIMESTAMP` | `sc.media_mode`, `MTL_SUBMIT_*` | MS6 | R; none; S! |
| U-285 | rate and interlace: `fps`, `interlaced` | `f.video` (raster), `MTL_FASTMETA_FREE_RUNNING` | MS4 | R; RXTX; S! |
| U-286 | stats, update, queue meta: `st41_*_get/reset_session_stats`, `st41_*_update_*`, `st41_rx_get_queue_meta` | stats keys, `mtl_session_update`; queue meta: U-139 | MS4; MS5 (`mtl_session_update`) | R; none; S! |

### 2.16 RTP passthrough (app-built packets)

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-340 | TX app-built RTP video (MTL adds L2–L4, paces): `ST20_TYPE_RTP_LEVEL`, `st20_tx_get_mbuf/put_mbuf`, `rtp_ring_size`, `rtp_frame_total_pkts`, `rtp_pkt_size`, `notify_rtp_done` | `unit = MTL_UNIT_PACKETS`, `mtl_packet_config` (`packets_per_chunk`, `slot_bytes`, `set_fields`, `pacing`), `mtl_pkt_tx`, `MTL_SUBMIT_UNIT_END` | MS5 | Co; SMP 1, RXTX 1, KT 2, UT 1; S! (H-01) |
| U-341 | RX raw RTP video (2022-7 de-duplicated): `st20_rx_get_mbuf/put_mbuf`, `notify_rtp_ready` | `mtl_rx_dequeue` of a chunk, `struct mtl_pkt_rx`, `MTL_PKT_RX_NO_DEDUP`, `sc.packet.rx_ring_packets` | MS5 | Co; SMP 1, RXTX 1, KT 1; S! |
| U-342 | ST22 RTP (exact packet count per frame): `ST22_TYPE_RTP_LEVEL`, `st22_tx/rx_get_mbuf/put_mbuf` | `MTL_CVIDEO` + `MTL_UNIT_PACKETS`, `packets_per_unit` | MS5 | Co; KT 1; S! |
| U-343 | ST30 RTP: `ST30_TYPE_RTP_LEVEL`, `st30_tx/rx_get_mbuf/put_mbuf` | `MTL_AUDIO` + `MTL_UNIT_PACKETS` | MS5 | Co; RXTX 1, KT 1; S! |
| U-344 | ST40 RTP: `ST40_TYPE_RTP_LEVEL`, `st40_tx/rx_get_mbuf/put_mbuf` | `MTL_ANC` + `MTL_UNIT_PACKETS` | MS5 | Co; RXTX 1, KT 1; S! |
| U-345 | ST41 RTP: `ST41_TYPE_RTP_LEVEL`, `st41_tx/rx_get_mbuf/put_mbuf` | `MTL_FASTMETA` + `MTL_UNIT_PACKETS` | MS5 | Co; RXTX 1; S! |
| U-346 | ST 2022-6 and custom payloads through RTP level (`doc/design.md:326-330`) | `sc.essence = MTL_RTP`, `struct mtl_rtp_config` (`clock_rate`, `profile`, `encoding`) | MS5 | Co; external, not in tree; S! |
| U-347 | RTP and payload headers: `st_rfc3550_rtp_hdr`, `st20_rfc4175_rtp_hdr`, `st20_rfc4175_extra_rtp_hdr`, `ST20_SRD_OFFSET_CONTINUATION`, `ST20_SECOND_FIELD`, `ST20_RETRANSMIT`, `st22_rfc9134_rtp_hdr`, `st40_rfc8331_*_hdr`, `st41_rtp_hdr` | `mtl_rtp_hdr`, `mtl_rfc4175_hdr`, `mtl_rfc4175_srd`, `mtl_rfc9134_hdr`, `mtl_rfc8331_hdr`, `mtl_st41_hdr` | MS5 | Co; SMP, RXTX, KT, GST; S! |
| U-348 | pixel-group bit layouts: `st20_rfc4175_422_10_pg2_be` … `st20_rfc4175_444_12_pg2_le` | `mtl_format_describe` gives bytes and pixels per group only (`pg_bytes`, `pg_pixels`) | MS4 | R; SMP (perf), KT; t. Byte-layout structs in `mtl_format.h`: OI-42 |
| U-349 | packet size limits: `MTL_PKT_MAX_RTP_BYTES`, `MTL_UDP_MAX_BYTES`, `MTL_MTU_MAX_BYTES` | `MTL_UDP_HDR_BYTES`, `MTL_IPV4_HDR_BYTES` + the rule "MTU minus IP and UDP"; `sc.max_udp_payload` (instance default `MTL_OPT_MAX_UDP_PAYLOAD`), `MTL_REASON_PKT_CONFIG` | MS5 | R; RXTX, KT. CHANGED: no fixed constant |

### 2.17 Pipeline conversion, formats and frame helpers

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-360 | convert app layout and transport format in the library: st20p `input_fmt` / `output_fmt` vs `transport_fmt` | `v.app_format` + `v.format`, `mtl_convert` with `MTL_CONVERT_CHECK`, `MTL_INFO_DIRECT`, `MTL_TXR_COPIED`, key `info.convert_context` | MS1; MS4 (`mtl_convert`) | C; FF, GST, OBS, RXTX, PY, RS. Today conversion runs in the app thread inside `put_frame`/`get_frame` |
| U-361 | app frame formats: `enum st_frame_fmt` | `enum mtl_app_format` (every legacy raw format + NV12, RGBA); codestreams through `enum mtl_codec` | MS1; MS2a (E15) | Co; many. CHANGED (V210): `MTL_APP_V210` rows are 128 B per 48 pixels; widths that are not a multiple of 48 (1280, DCI 2048, DCI 4096) are `-MTL_ENOTSUP` until E15 (MS2a); legacy st20p keeps its contiguous layout |
| U-362 | non-compliant 8-bit passthrough: `ST_FRAME_FMT_YUV420CUSTOM8`, `_YUV422CUSTOM8` | `MTL_APP_YUV420_CUSTOM8`, `MTL_APP_YUV422_CUSTOM8` (`MTL_INFO_NON_COMPLIANT`) | MS1 | Co; FF 2, GST 1, OBS 1, RXTX 1, RS 1, SMP 1 |
| U-363 | zero-copy (derive) when formats match | `MTL_SESSION_REQUIRE_DIRECT`, `MTL_INFO_DIRECT`, `MTL_PATH_DIRECT` | MS1 | Co; GST, KT |
| U-364 | converter device for st20p: `st20p_*_ops.device` (`enum st_plugin_device`) | `MTL_OPT_VIDEO_CONVERT_DEVICE` (`MTL_CODEC_DEVICE_*`) | MS2 | R; RXTX, KT |
| U-365 | `struct st_frame`: planes, strides, size, format, timestamps, status | `struct mtl_unit` + `mtl_tx_result` / `mtl_rx_detail` | MS1 | C; every pipeline user |
| U-366 | buffer requirements before create: `st_frame_size`, `*_frame_size` | `mtl_session_query(…, info, req)`, `mtl_format_describe` (`plane_bytes[]`) | MS1 | Co; SMP 8, KT 4 |
| U-367 | standalone frame allocation: `st_frame_create/free`, `st_frame_create_by_malloc` | `mtl_mem_alloc` (`mtl_mem_open` with `va` NULL) + `mtl_session_attach`, or app memory + `mtl_format_describe` for `mtl_convert` | MS2; MS4 (`mtl_convert`) | R; KT 1, PY 2. CHANGED |
| U-368 | format helpers: `st_frame_fmt_*`, `st_frame_name_to_fmt`, `st_frame_least_linesize`, `st_frame_plane_size`, `st_frame_data_height`, `st_frame_sanity_check` | `mtl_format_describe` (`row_bytes[]`, `rows[]`, names, `plane_bytes[]`), `mtl_format_parse` | MS1; MS2 (`mtl_format_parse`) | Co; SMP, FF, GST, OBS, PLG 3, PY, KT 4 |
| U-369 | software frame operations: `st_frame_convert`, `st_frame_downsample`, `st_draw_logo`, `st_field_merge`, `st_field_split` | `mtl_convert` with `MTL_CONVERT_FIELD_SPLIT`, `_FIELD_MERGE`, `_HALF_SCALE`; `st_draw_logo` is CUT-8 | MS4 | R; SMP 4, PY 1, KT 1 |
| U-370 | codec name and enum: `st_name_to_codec`, `enum st22_codec` | `enum mtl_codec`; `mtl_format_parse` and `mtl_format_describe` with `MTL_FORMAT_CODEC` | MS4 | R; SMP 1, FF 2, PY 1 |
| U-371 | SWIG helpers: `st_frame_addr_cpuva`, `st_frame_addr`, `st_rxp/st_txp_para_*` | POD structs, `MTL_ADDR(T)`, `mtl_flow_ipv4` | MS6 | Co; PY 8. CHANGED; no spec strings or parsers (D-97) |

### 2.18 Codec and converter plugins

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-380 | register an ST22 encoder or decoder in-process: `st22_encoder_register/unregister`, `st22_decoder_register/unregister`, `st22_encoder_dev`, `st22_decoder_dev`, `*_create_req` | `mtl_plugin_open(mt, NULL, &dev, &h)` + `struct mtl_plugin_device` (`MTL_PLUGIN_ENCODER`, `_DECODER`) | MS4 | Co; PLG 2, KT 1 (H-19) |
| U-381 | encoder/decoder worker loop: `st22_encoder/decoder_get_frame`, `put_frame`, `wake_block`, `set_block_timeout`, `*_RESP_FLAG_BLOCK_GET`, `st22_encode/decode_frame_meta` | `mtl_plugin_host.get_work(…, timeout)` / `put_work` (ABI v2) | MS4 | Co; PLG 2, KT 1. CHANGED |
| U-382 | st20p format converter: `st20_converter_register/unregister/get_frame/put_frame`, `st20_converter_dev` | `MTL_PLUGIN_CONVERTER` | MS4 | R; PLG 1, KT 1 |
| U-383 | load plugins from `.so` (JSON list at init): `st_plugin_register/unregister`, `st_get_plugins_nb`, `ST_PLUGIN_*_API`, `st_plugin_meta`, `ST_PLUGIN_VERSION_V1_MAGIC` | `mtl_plugin_open(mt, path, NULL, &h)`, `MTL_PLUGIN_ENTRY_SYMBOL`, `mtl_plugin_unload`; no list at open | MS4 | Co; KT 2; `plugins/st22_avcodec`, `plugins/sample`. CHANGED |
| U-384 | format capability masks: `ST_FMT_CAP_*` | `struct mtl_plugin_format_pair` lists | MS4 | Co; PLG. CHANGED: no 64-value limit |
| U-385 | test-only plugin devices: `ST_PLUGIN_DEVICE_TEST`, `_TEST_INTERNAL` | `MTL_CODEC_DEVICE_TEST`, `MTL_CODEC_DEVICE_TEST_INTERNAL` | MS4 | R; KT |

### 2.19 Time, rate and conversion helpers

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-390 | frame rate enum, number and name: `st_frame_rate`, `st_frame_rate_to_st_fps`, `st_name_to_fps` | `enum mtl_fps`, `mtl_fps_rational`; no name parser (D-97) | MS1 | C; RXTX 11, KT 18, FF 1, GST 3, PY 1; t |
| U-391 | TAI ↔ media clock and unwrap: `st10_tai_to_media_clk`, `st10_media_clk_to_ns`, `st10_media_clk_to_tai`, `st10_get_tai`, `st10_get_media_clk`, `enum st10_timestamp_fmt` | `mtl_media_ticks`, `mtl_media_tai` (`mtl_sync.h`) | MS2 | Co; RXTX 4, KT 6, UT 8; t (H-12) |
| U-392 | sampling-rate constants: `ST10_VIDEO_SAMPLING_RATE_90K`, `ST10_AUDIO_SAMPLING_RATE_*` | implicit (Hz numbers) | MS1 | R; RXTX, KT; t |
| U-393 | standalone colour conversion library (RFC 4175 ↔ planar / V210 / Y210 / LE, AM824 ↔ AES3; SIMD and DMA variants): `st_convert_api.h` (47), `st_convert_internal.h` (58) | `mtl_convert(&desc)` (one call, optional DMA; AM824 ↔ AES3 with audio formats); per-pair functions internal (CUT-4) | MS4 | Co; SMP (perf, tools), PLG 1, PY, KT (70 functions in `cvt_test.cpp`). CHANGED |

### 2.20 Stats and observability

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-400 | session stats per port and family: `st*_get_session_stats`, `st_tx/rx_user_stats`, `st_tx/rx_port_stats`, `st20/30/40/41_*_user_stats` | `mtl_stat_list/find/read/get`, keys `tx.*`, `rx.*`, `leg.*` (one schema; ST22 gains stats) | MS2 (ST20); MS4 | Co; RXTX 1, KT 3, UT 3, ext NUDA9A; t |
| U-401 | reset session stats: `st*_reset_session_stats` | none: cumulative (D-80) | MS1 | Co; RXTX 1, UT 2. CHANGED |
| U-402 | family-specific counters (no slot, wrong interlace, burst sizes, …): `st20_rx_user_stats` (~35 fields) | `rx.pkts_rejected{cause}` and the map of [migration.md](migration.md) §4.14 | MS3 | R; RXTX, KT; t. About 20 fields without a key: OI-40 |

### 2.21 Experimental and legacy remnants

| U-ID | Legacy | Unified home | When | Notes |
|---|---|---|---|---|
| U-410 | redundant-combined ST20 RX: `st20rc_rx_*`, `ST20RC_RX_FLAG_*` | a two-leg RX session; the API itself is CUT-3 | removed (D-87, CUT-3) | L; SMP 1, RXTX 1 |
| U-411 | deprecated `sip_addr` alias of RX `ip_addr` (`st*_rx_ops`, `st_rx_port`, `st_rx_source_info`) | none | removed (D-87, CUT-11) | L; none |
| U-412 | reserved RX `pacing` / `packing` fields of `st20_rx_ops` ("not in use") | none | removed (D-87, CUT-11) | L; none |
| U-413 | ST22 slice packetization `ST22_PACK_SLICE` ("not support now") | `MTL_OPT_CVIDEO_PACK = MTL_CVIDEO_PACK_SLICE` ("not yet", `-MTL_ENOTSUP`) | MS4 | L; none |
| U-414 | legacy callback guard pattern (`if (!ctx->handle)`: callbacks may fire before create returns) | no callbacks; the handle exists before start | — | note, not counted; KT 37 sites, OBS |
| U-415 | `MTL_FLAG_BIND_NUMA` set by apps (`mtl_api.h:340`, no effect) | none (see U-062) | removed (D-87, CUT-11) | L; SMP 2, FF, OBS 2, PY 7, RS 6 |
| U-416 | plain `mtl_udma_fill` (u64 pattern) | none (CUT-8) | removed (D-87, CUT-8) | L; none |
| U-417 | raw `st20_rx_ops.gpu_direct_framebuffer_in_vram_device_address` | `mtl_mem_open` with `MTL_MEM_DEVICE` (U-117) | MS6 | R; none |
| U-418 | fb address getters with no users: `st20p_rx_get_fb_addr`, `st22p_rx_get_fb_addr`, `st22_rx_get_fb_addr`, `st40p_tx/rx_get_udw_buff_addr`, `st40p_tx_get_fb_addr` | `mtl_session_get_slot` | MS1 (video); MS4 (cvideo, ANC) | R; none |
| U-419 | ignored ST22 format fields: `st22_tx_ops.fmt`, `st22_rx_ops.fmt` | none | removed (D-87, CUT-13) | L; ignored by today's library |


## 3. Rules for changing the inventory

- A row may move from a typed field to a helper or an option freely; it never loses its home
  without a cut recorded in D-87 and §4.5.
- A default that changes the wire (U-155, U-202, U-233) is CHANGED, with the legacy-compatible
  setting named (D-114).
- Merging two knobs keeps every legacy value reachable (`time_source` and the former PTP flag; the
  two RL warm-up values of U-248).
- Before any session header is hidden (MS7), every `S!` row has a working home and every `t` type
  a replacement (§4).

## 4. Hiding the session-level headers

The requirement (D-83): RTP passthrough stays, and `include/st20_api.h` and its siblings stop
being public with no loss of function. The mechanism (three tiers, version nodes, stages F, F+1,
≥ F+2, the gate header) is in [migration.md](migration.md) §8, the plugin ABI v2 in §9 there;
this section is the inventory behind it.

### 4.1 Today's public header set

| Fact | Evidence |
|---|---|
| all 13 top-level headers install into `<prefix>/include/mtl/`, `st20_combined_api.h` into `mtl/experimental/`; the generated `mtl_build_config.h` too, because `mtl_api.h:27` includes it | `include/meson.build:4-8`, `:11-16`; `meson.build:39-53` |
| pkg-config emits `-I${includedir} -I${includedir}/mtl`, so `<mtl/st20_api.h>` and `<st20_api.h>` both resolve; apps use the first, the MXL POC the bare form | `meson.build:77-84`; `app/sample/sample_util.h:11`, `tests/tools/RxTxApp/src/app_base.h:12`, `ecosystem/MTL_with_MXL/poc/src/include/poc_mtl_rx.h:8-9` |
| no tiering: `st_convert_internal.h` says "for internal test usage only" yet is installed and SWIG-wrapped; the experimental header has no opt-in | `include/st_convert_internal.h:5-9`, `python/swig/pymtl.i:21`, `:30`; `st20_combined_api.h:6-8` |
| include graph: `mtl_api.h` ← `st_api.h` ← `st20/30/40/41_api.h` (`:12`); `st_pipeline_api.h` ← `st20_api.h` (`:14`); `st30_pipeline_api.h` ← `st30_api.h` + `st_pipeline_api.h` (`:12-13`); `st40_pipeline_api.h` ← `st40_api.h` + `st_pipeline_api.h` (`:8-9`) | the headers |
| `st_convert_api.h` ← `st_convert_internal.h` (`:13`) ← `st20_api.h`, `st30_api.h` (`:13-14`); `st20_combined_api.h` ← `st20_api.h` (`:12`); `mtl_sch_api.h`, `mtl_lcore_shm_api.h` ← `mtl_api.h` (`:12`) | the headers |
| function counts: `mtl_api.h` 70, `st_api.h` 12, `st20_api.h` 51 (ST20 and ST22), `st30_api.h` 23, `st40_api.h` 27, `st41_api.h` 16, `st_pipeline_api.h` 106, `st30_pipeline_api.h` 24, `st40_pipeline_api.h` 27, `mtl_sch_api.h` 6, `mtl_lcore_shm_api.h` 2, convert 47 + 58 (≈ 40 public `static inline` wrappers call `*_simd`, `st_convert_api.h:41-45`), `st20_combined_api.h` 6 | `-aux-info` |
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

The U-row of §2 gives the symbol and the milestone.

| # | Feature (legacy, `st20_api.h` lines unless named) | In-tree consumers | Home |
|---|---|---|---|
| V1 | frame TX by index callback, `get_next_frame`/`notify_frame_done` `:1186-1195` | `legacy/tx_video_sample.c:220`; `app/v4l2_to_ip/v4l2_to_ip.c:1963`; RXTX `legacy/tx_video_app.c:858`; KT `st20/*` (13 creates); Rust `rust/src/imtl/video.rs:543` | COVERED (U-181) |
| V2 | frame RX callback + `st20_rx_put_framebuff` `:1544`, `:2262` | samples, RXTX legacy, MXL `poc/src/sender/mxl_bridge.c:499`, Rust `video.rs:858` | COVERED (U-201); MXL by `MTL_SESSION_RX_BY_INDEX` |
| V3 | tasklet-context latency of V1/V2 (bobi, MXL, fwd chose callbacks for latency) | MXL POC, fwd samples, ext bobi | LATER (U-176, H-15) |
| V4 | RTP level `:356`, `:1932-1948`, `:2279-2290`, `:1258`, `:1619` | `low_level/{tx,rx}_rtp_video_sample.c:161/107`; RXTX `legacy/tx_video_app.c:287`; KT (61 `RTP_LEVEL` sites) | COVERED (U-340, U-341, H-01) |
| V5 | slice TX/RX `:362`, `:1254`, `:1610`, `:1603` | `low_level/*slice*`; RXTX `legacy/tx_video_app.c:784`, `rx_video_app.c:548`; KT `st20/{digest,uframe,detect}` | COVERED (U-191, U-218, H-02) |
| V6 | TX ext frame `st20_tx_set_ext_frame` `:1882`, `ST20_TX_FLAG_EXT_FRAME` `:45` | samples ×4, `v4l2_to_ip.c:1457`, KT `st20_common.cpp:148` | COVERED (U-184); a new layout per acquire: `mtl_tx_acquire_layout` (U-185) |
| V7 | RX static `ext_frames[]` `:1550` | KT | COVERED (U-208) |
| V8 | RX `query_ext_frame` `:1599` (needs `RECEIVE_INCOMPLETE_FRAME`) | KT `st20/st20_ext_frame.cpp:203`; MXL `poc/src/sender/mtl_rx.c:199`; GST via st20p (`gst_mtl_st20p_rx.c:140`) | COVERED (U-209, H-09) |
| V9 | `uframe_pg_callback` + `uframe_size` `:1586`, `:1574` | RXTX `legacy/rx_video_app.c:593`; KT `st20_uframe.cpp:151`, `st20_detect.cpp:167` | CUT-2; per-packet conversion kept (`st_pipeline_api.h:590`, H-16) |
| V10 | auto-detect `:219`, `notify_detected` `:1592` | RXTX `legacy/rx_video_app.c:534,550`; KT `st20_detect.cpp:171`; `rx_st20p_auto_detect_sample.c:195` | COVERED (U-212, H-06) |
| V11 | timing parser STAT `:238` | RXTX `legacy/rx_video_app.c:565`; acceptance `tests/single/rx_timing` | COVERED (U-214) |
| V12 | timing parser META `:243`, `:475`, `:2328` | `rx_st20p_timing_parser_sample.c:85`; Python `rx_timing_parser.py:135`; UT `st20_harness.h:147` | COVERED (U-215, H-07) |
| V13 | RX DMA offload `:212`, `:2314` | RXTX, KT ×5, FF (misused flag) | COVERED (U-211) |
| V14 | header split `:226` | RXTX `legacy/rx_video_app.c:560`; KT `st20_digest.cpp:155`; `ext_frame/rx_st20p_hdr_split_gpu_direct.c:311` | CUT-1 (NG6) |
| V15 | GPU VRAM frames `:1624`; st20p `USE_GPU_DIRECT_FRAMEBUFFERS` (`st_pipeline_api.h:642`) | FF `mtl_st20p_rx.c:204-220`; `app/sample/gpu_direct/` | COVERED (U-117, H-10) |
| V16 | NACK retransmission `:87`, `:186`, `:1119`, `:1441` | RXTX `legacy/{tx,rx}_video_app.c:801/562`; KT `st20_digest.cpp:86,157`; `doc/rtcp.md` | COVERED (U-141, H-05) |
| V17 | two RX threads `:249` | RXTX | COVERED (U-217) |
| V18 | user pacing, exact, user timestamp, epoch RTP, RTP delta `:56-100` | KT `st20_user_pacing.cpp:75`, `tx_timestamp_sync_test.cpp:227`; `fwd/rx_st20_tx_st20_split_fwd.c:271` | COVERED (U-151…U-156) |
| V19 | VSYNC `:75`, `:181`; `st_api.h:208` | RXTX `legacy/tx_video_app.c:9,792`; KT `tests.cpp:955` | COVERED (U-159) |
| V20 | `notify_frame_late` `:1204` | KT | COVERED (U-158) |
| V21 | `update_destination` / `update_source` `:1866`, `:2187` | KT `st20/st20_update_source.cpp:146` | COVERED (U-135, U-136) |
| V22 | `get_sch_idx`, `get_pacing_params` `:1959`, `:1976`, `:2198` | RXTX `legacy/tx_video_app.c:139` | COVERED (U-070, U-161) |
| V23 | queue meta + `DATA_PATH_ONLY` `:2303`, `:176` | KT `st20_digest.cpp:212` and two more; `st22_test.cpp:1412` | CUT-9 (H-17) |
| V24 | pcapng dump `:2216` (+ st20p/st22p/st20rc) | RXTX `legacy/rx_video_app.c:700`; KT `st20/st20_dump.cpp:113` | COVERED (U-140, H-11) |
| V25 | stats get/reset `:1993`, `:2344` | RXTX, KT | CHANGED (no reset) |
| V26 | static pad, bulk, NUMA, user MACs, migrate off, burst, simulated loss `:35-249` | RXTX, KT | COVERED |
| V27 | linesize, `user_meta` | KT `st20_linesize_digest.cpp` | COVERED (U-188, U-190) |
| V28 | `st20_get_pgroup`, `st20_frame_size`, `st20_get_bandwidth_bps`, names `:2020-2517` | samples ×6, RXTX ×3, KT ×15, fuzz | COVERED (U-196, U-197, H-04) |
| V29 | transport-format set (18 `st20_fmt` values) `:321-351` | every video user | COVERED (U-193, U-194, H-03) |
| CV1 | ST22 codestream frames `:373`, `st22_tx_get_fb_addr` `:2161` | `legacy/{tx,rx}_st22_video_sample.c:196/179`; RXTX `legacy/{tx,rx}_st22_app.c`; MXL `poc_8k/…/main.c:355,813`; KT `st22_test.cpp` | COVERED (U-232) |
| CV2 | ST22 RTP level `:2120-2136`, `:2443-2454` | KT `st22_test.cpp` (×12) | COVERED (U-342) |
| CV3 | `DISABLE_BOXES` `:127`, `:266` | none (`git grep DISABLE_BOXES`) | CUT-6 |
| CV4 | `ST22_PACK_SLICE` `:386-387` | none | COVERED (U-413) |
| CV5 | ST22 RTCP, pcapng, queue meta, VSYNC, update | KT `st22_test.cpp:1327,1152,1412,740` | as V16, V24, V23, V19, V21 |
| AU1 | audio frame sessions (`st30_api.h:676`, `:847`) | RXTX `legacy/{tx,rx}_audio_app.c:484/416`; KT `st30_test.cpp:372/563` | COVERED (U-244) |
| AU2 | audio RTP level (`:729-745`, `:902-913`) | RXTX `legacy/tx_audio_app.c:141`; KT `st30_test.cpp:72` | COVERED (U-343) |
| AU3 | pacing way, RL warm-up, FIFO (`:178`) | RXTX | COVERED (U-247, U-248, U-250) |
| AU4 | audio timing parser STAT/META (`:118` ff.) | RXTX `legacy/rx_audio_app.c:380,383` | CHANGED (U-254) |
| AU5 | size helpers (`:756-832`) | FF `mtl_st30p_{rx,tx}.c:126/129`; GST `gst_mtl_st30p_rx.c:229`; Rust `audio.rs:215` | COVERED (U-245, U-246) |
| AU6 | audio RTCP flags | RXTX `legacy/rx_audio_app.c:379` | CUT-7 |
| AU7 | AM824/AES3 conversion (`st_convert_api.h:1064`) | — | COVERED (U-255) |
| AN1 | ANC frame sessions, ≤ 20 meta (`st40_api.h:612`, `:725`) | RXTX `tx_ancillary_app.c:480`, `rx_ancillary_app.c:159` (main app, not legacy/); KT `st40_test.cpp:409/586` | COVERED (U-260, U-261) |
| AN2 | ANC RTP level (`:695-711`, `:766-777`) | RXTX `tx_ancillary_app.c:147`, `rx_ancillary_app.c:67`; KT | COVERED (U-344) |
| AN3 | RFC 8331 and UDW helpers (`:816-925`) | GST `gst_mtl_st40p_tx.c:655,706-707`; RXTX `rx_ancillary_app.c:17,30`; UT, fuzz | COVERED (U-268, H-13) |
| AN4 | test mutation config (`:90-108`) | GST test element | COVERED (U-267) |
| AN5 | split by packet, EXACT, DEDICATE_QUEUE | RXTX `tx_ancillary_app.c:475-478` | COVERED |
| AN6 | ANC RTCP flags | RXTX `rx_ancillary_app.c:155` | CUT-7 |
| FM1 | fastmeta TX (`st41_api.h:377`) | RXTX `tx_fastmetadata_app.c:475`; acceptance `tests/single/st41` | COVERED (U-280) |
| FM2 | fastmeta RX, RTP only (`st41_rx_ops` `:232`, `:501-512`) | RXTX `rx_fastmetadata_app.c:166,277` | COVERED (U-283, U-345) |
| FM3 | DIT/K filters | RXTX | COVERED (U-282) |
| FM4 | fastmeta RTCP flags | RXTX `rx_fastmetadata_app.c:258` | CUT-7 |
| ST1 | `st_fps`, `st_frame_rate*`, `st_name_to_fps` | FF `mtl_common.c:36`; plugins | COVERED (U-390) |
| ST2 | media-clock helpers | RXTX latency code | COVERED (U-391, H-12) |
| ST3 | `struct st_frame`, `st_ext_frame` | every pipeline consumer | COVERED (U-365) |
| ST4 | `st_frame_fmt` (≈ 28 app formats) | GST `gst_mtl_st20p_tx.c:370,666`; OBS `mtl-output.c:217`; samples | COVERED (U-361) |
| ST5 | `st_frame_*` helpers | samples, GST, plugins (`st_frame_fmt_name` ×10, `st_frame_plane_size` ×6) | COVERED (U-368) |
| ST6 | `st_draw_logo` (`st_pipeline_api.h:2365`) | samples ×5, KT `st22p_test.cpp:435` | CUT-8 (sample-local code) |
| ST7 | `st_name_to_codec` | FF `mtl_st22p_rx.c:92` | COVERED (U-370) |
| ST8 | SWIG setters `st_txp_*`, `st_rxp_*`, `mtl_para_*` | Python | COVERED (U-102, U-371) |
| ST9 | port and user stats structs, `st_var_info` | RXTX | COVERED (U-033, U-400) |
| ST10 | `st_rfc3550_rtp_hdr` | RTP-level users | COVERED (U-347) |
| MT1 | log hooks (`mtl_api.h:1086`, `:1100`) | RXTX `args.c`, `rxtx_app.c` | COVERED (U-019…U-021, H-14) |
| MT2 | lcore borrow (`mtl_api.h:1142`, `:1157`) | `app/perf/*` ×14, RXTX, KT `st_test.cpp` | CUT-5b |
| MT3 | user DMA `mtl_udma_*` (`mtl_api.h:1400-1505`) | `app/sample/dma/dma_sample.c:24`, `app/perf/*`, KT `dma_test.cpp`, `cvt_test.cpp` | CUT-5 |
| MT4 | hugepage and DMA memory (`mtl_dma_map` `:1310`) | samples, fwd | COVERED (U-110…U-113) |
| MT5 | PTP read, time function, sync notify | RXTX, FF | COVERED (U-045, U-046, U-048) |
| MT6 | `mtl_sch_enable_sleep`, `_set_sleep_us` | RXTX `rxtx_app.c:176`; KT `st20p_test.cpp:929` | COVERED (U-052, U-053, H-18) |
| MT7 | `mtl_memcpy` (`:1185`, ≈ 60 files), `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_size_page_align` | widely | CUT-8; `mtl_size_page_align`: U-114 |
| MT8 | port and backend queries, manager alive, port stats, `mtl_abort` | samples, RXTX | COVERED (U-006, U-035, U-096, U-099) |
| MT9 | SIMD level | perf tools | COVERED (U-066) |
| X1 | user schedulers and tasklets (`mtl_sch_register_tasklet`, `mtl_sch_api.h:128`) | KT `sch_test.cpp:16,147` | CUT-10 (H-08) |
| X2 | `mtl_lcore_shm_print`/`clean` (`mtl_lcore_shm_api.h:44`, `:59`) | `app/tools/lcore_shmem_mgr.c:60,63` | internal tool; SysV table removed (D-92) |
| X3 | ≈ 105 per-pair converters (`_simd`, `_simd_dma`, `_2way_cpuva`) | `app/tools/convert_app.c` (≈ 20), `app/perf/*`, RXTX `fmt.c:27`, `cvt_test.cpp` (≈ 70), `plugins/sample/convert_plugin_sample.c:24`, Python (`st_convert_internal.h:1423`) | COVERED by `mtl_convert` (CUT-4 for the per-pair functions) |
| X4 | `st20rc` redundant-combined RX | `experimental/rx_st20_redundant_combined_sample.c:191`; RXTX `experimental/rx_st20r_app.c:341` | CUT-3 (two legs, D-59) |

### 4.3 Who blocks hiding

| Consumer | Session-header dependency | Path to unified |
|---|---|---|
| FFmpeg plugin | pipelines, plus `st30_calculate_framebuff_size`, `st_frame_rate_to_st_fps`, `st_name_to_codec`, the misused `ST20_RX_FLAG_DMA_OFFLOAD` (`mtl_st20p_rx.c:176`) | the st20p path in MS3, the rest in MS6; the flag fixed as a bugfix first ([migration.md](migration.md) §11.4) |
| GStreamer plugin | pipelines, plus `st20_rx_frame_meta`, st40 UDW/parity helpers, `st_frame_fmt_*` | the MS6 port; needs H-09 and H-13 |
| OBS | `st_pipeline_api.h` only (`linux-mtl/linux-mtl.h:10`) | a small port (MS6) |
| `plugins/` | implements `st22_encoder_dev`, `st22_decoder_dev`, `st20_converter_dev` (`plugins/sample/st22_plugin_sample.c:231,251`; `plugins/st22_avcodec/st22_avcodec_plugin.c:519,535`; `plugins/sample/convert_plugin_sample.c:144`) | plugin ABI v2 (MS4); the v1 loader stays until F+2 |
| MXL POC | `st20_rx` + `query_ext_frame`, `st22` sessions | `MTL_SESSION_RX_BY_INDEX` (MS2); cvideo (MS4) |
| Rust `imtl-rs` | bindgen over `wrapper.h:1-6`; session `st20_tx/rx_create` frame mode (`video.rs:543,815`) | rebind on `mtl.h` (MS6) |
| Python `pymtl` | SWIG over six headers incl. the internal convert header (`pymtl.i:16-32`) | the Python wrapper (MS6); `_2way_cpuva` replaced by `mtl_convert` |
| `app/sample` | `sample_util.h:11-15` pulls every legacy header; session samples in `legacy/`, `low_level/`, `ext_frame/`, `fwd/`, `experimental/`, `dma/` | replaced by new samples on the new API that together call every exported function ([implementation-plan.md](implementation-plan.md) §4.2, D-138); each legacy sample goes in the commit that adds its replacement |
| `app/v4l2_to_ip` | `st20_api.h` (`v4l2_to_ip.c:31`), session TX with ext frames | video TX with attached slots (MS2) |
| `app/tools`, `app/perf` | lcore shm, convert, `*_simd_dma`, lcore borrow, udma | the internal dependency (in-tree tools) |
| RxTxApp | session ANC and fastmeta apps in the main tree; legacy video, audio, st22; experimental st20rc; JSON kinds `video`, `audio`, `ancillary`, `fastmetadata`, `st22` (`tests/acceptance/mtl_engine/rxtxapp_config.py:20-41`) | `UnifiedRxTxApp` beside the frozen RxTxApp: `st20p` MS1, slice MS2, the other kinds MS4, RTP MS5; the JSON schema kept ([migration.md](migration.md) §11.1) |
| KahawaiTest, unit, fuzz | 752 session call sites; unit and fuzz compile lib `.c` (`tests/unit/session/st20_harness.c:22`, `tests/fuzz/st20/st20_rx_frame_fuzz.c:26`; `tests/unit/meson.build:20-21`, `tests/fuzz/meson.build:43-44`) | not blockers: the internal dependency; they keep testing the engine |

The acceptance suite hard-codes `.local_install` and RxTxApp's legacy JSON kinds, and an `#error`
at stage F+1 breaks builds that include a legacy header only for a type (GStreamer
`st20_rx_frame_meta`), so the F+1 CI job runs on the ecosystem trees first.
`mtl_instance_from_legacy` stays exported for the whole legacy window. It is in its own node,
never in `MTL_1.0`, and leaves with `mtl_init` (D-190).

### 4.4 Pre-hide gaps H-01…H-19

The work stage F (MS7) waits for; the homes are in [migration.md](migration.md) §8.4.

| Gap | Home | When |
|---|---|---|
| H-01 RTP level, ST 2022-6 | `MTL_UNIT_PACKETS`, `MTL_RTP` (D-82) | MS5 |
| H-02 slice / rows | `MTL_UNIT_ROWS`, `mtl_rx_wait_rows` (D-101) | MS2 |
| H-03 format parity (+7 transport, ≈ +16 app formats) | `enum mtl_video_format`, `mtl_video_format_ext`, `enum mtl_app_format` | MS1 |
| H-04 format helpers | `mtl_format_describe`, `mtl_format_parse`; no fps name parser (D-97) | MS2 |
| H-05 NACK retransmission | `MTL_OPT_RTX*` options | MS2; MS4 (cvideo) |
| H-06 video auto-detect (deliver nothing until accepted) | `v.detect`, `MTL_EVENT_RX_FORMAT` | MS2 |
| H-07 per-unit ST 2110-21 results | `mtl_rx_detail.timing[]` per lease and leg | MS2 |
| H-08 user tasklets | removed (CUT-10) | — |
| H-09 RX destination per unit | `MTL_SESSION_RX_BY_INDEX`; `mtl_rx_provide` | MS2 |
| H-10 GPU VRAM | `mtl_mem_open` with `MTL_MEM_DEVICE` (`-MTL_ENOTSUP` until then) | MS6 |
| H-11 pcapng capture | `mtl_session_capture`, asynchronous only | MS4 |
| H-12 media-clock helpers | `mtl_media_ticks`, `mtl_media_tai` | MS2 |
| H-13 conversion, AM824, ANC helpers | `mtl_convert`; the ANC helpers of `mtl_util.h` | MS4; MS1 (ANC helpers) |
| H-14 log sinks with an instance prefix | `mtl_log_add_sink` (the origin names the instance) | MS2 |
| H-15 inline notify | `mtl_session_set_inline_notify` (`MTL_LATER`, D-04) | LATER |
| H-16 per-packet RX conversion | `MTL_OPT_RX_CONVERT_PER_PACKET` | MS2 |
| H-17 queue meta | removed (CUT-9) | — |
| H-18 scheduler sleep interval | `MTL_OPT_SCHED_SLEEP_US` | MS1 |
| H-19 plugin ABI v2 | `mtl_plugin.h` | MS4 |

H-15 (under `MTL_LATER`) has no milestone; its legacy route stays in the legacy opt-in tier until
it has one.

### 4.5 The cut list

Every capability removed from the public API, and nothing else (D-87, D-112). Each was used by no
in-tree consumer beyond tests, a sample or RxTxApp.

| Cut | What | U-rows | Evidence | Replacement |
|---|---|---|---|---|
| CUT-1 | header split RX (`ST20_RX_FLAG_HDR_SPLIT`, `ST20P_RX_FLAG_HDR_SPLIT`, `nb_rx_hdr_split_queues`) | U-213 | needs a DPDK patch absent for the pinned 26.07 (no `patches/dpdk/26.07/hdr_split/`); one sample, one KT case, an RxTxApp option; NG6 | none; zero-copy RX through regions |
| CUT-2 | `uframe_pg_callback`, `uframe_size` | U-219 | application code on the tasklet (D-04); one RxTxApp option, two KT files | per-packet conversion (U-220); user formats through a converter plugin |
| CUT-3 | `st20rc` and `experimental/st20_combined_api.h` | U-410 | one sample, RxTxApp `rx_st20r_app.c` | a two-leg RX session (D-59) |
| CUT-4 | the public per-pair converters (≈ 105) | U-393 | `st_convert_internal.h:5-9` says internal; the public set is mostly `static inline` wrappers | `mtl_convert`; per-pair functions internal for `cvt_test.cpp`, `app/perf` |
| CUT-5 | public user DMA (`mtl_udma_*`), the `*_simd_dma` converters | U-116 | one sample, perf tools, two KT files | DMA inside sessions (`MTL_OPT_DMA`) |
| CUT-5b | public lcore borrowing (`mtl_get_lcore`, `mtl_bind_to_lcore`, `mtl_put_lcore`) | U-067 | perf tools, RxTxApp, one KT case | the internal tier for in-tree tools |
| CUT-6 | `ST22_TX/RX_FLAG_DISABLE_BOXES`, `ST22P_TX/RX_FLAG_DISABLE_BOXES` | U-234 | no consumer | none |
| CUT-7 | RTCP flags on ST30, ST40, ST41 and st40p | U-142 | no effect in `lib/` | none (video and cvideo keep NACK) |
| CUT-8 | `st_draw_logo`, `mtl_memcpy`, `mtl_sleep_us`, `mtl_delay_us`, `mtl_thread_setname`, `mtl_get_if_ip`, `mtl_udma_fill` | U-071, U-073, U-101, U-115, U-369, U-416 | thin wrappers, zero or one consumer outside samples | sample code, libc, `mtl_unit_copy_in/out` |
| CUT-9 | queue meta (`*_rx_get_queue_meta`, `st_queue_meta`) and `*_RX_FLAG_DATA_PATH_ONLY` | U-139 | KT only; a suspected NULL dereference on non-socket backends (SP-02) | none |
| CUT-10 | user schedulers and tasklets (`mtl_sch_*`, `mtl_tasklet_ops`) | U-068, U-069 | one KT file (`sch_test.cpp`) | none |
| CUT-11 | dead fields: `tx/rx_sessions_cnt_max`, `sample_size`, `sample_num`, the RX `sip_addr` alias, RX `pacing`/`packing`, `MTL_FLAG_BIND_NUMA`, ST40P `FORCE_NUMA`, `st40p_rx_ops.rtp_ring_size`, the UDP transport remnants | U-032, U-089, U-256, U-271, U-272, U-411, U-412, U-415 | documented as unused, or no effect | none |
| CUT-12 | the DPDK AF_XDP and AF_PACKET PMD backends (`dpdk_af_xdp:`, `dpdk_af_packet:`) | U-095 | native AF_XDP and kernel sockets cover them; the removed prefixes fail open with `BACKEND_REMOVED` | `native_af_xdp:`, `kernel:` |
| CUT-13 | `st22_tx_ops.fmt`, `st22_rx_ops.fmt` | U-419 | ignored by today's library | none |

## 5. Today's modes: asymmetries the unified API removes

Today: five families × two directions × up to three units × two layers, plus `st20rc` (20 session
and 8 pipeline create functions, each with its own ops struct and flag space; all cells verified
at `545a266a`). Pipeline flag bits are not aligned with session bits (`ENABLE_RTCP` is bit 2 in
`ST20_RX_FLAG_*`, bit 4 in `ST20P_RX_FLAG_*`), so each pipeline translates flags one by one
(`st20_pipeline_rx.c:508-546`). The mode-by-mode map with milestones is [migration.md](migration.md)
§4.16.

| Feature | Where it exists today | Unified |
|---|---|---|
| exact user pacing | ST20, ST40, st20p, st40p | `MTL_SESSION_EXACT_LAUNCH` + `MTL_SUBMIT_EXACT` on every essence |
| user timestamp | all but st30p | `MTL_SUBMIT_RTP_TS` everywhere |
| epoch RTP, RTP delta | ST20 (+ delta on ST30, st20p, st30p) | default; `tx.rtp_trim_ns` everywhere |
| `notify_event` (VSYNC, recovery, fatal) | ST20, ST22 and their pipelines | events for every essence |
| drop when late | pipelines only | `MTL_OPT_LATE_POLICY` everywhere |
| session stats | absent on ST22, st22p | one schema, ST22 included |
| RTCP | ST20, ST22 (st20p, st22p); no effect on ST30/40/41 | NACK for video and cvideo; CUT-7 |
| per-session pacing engine | ST30 and st30p only (video inherits the port's; ST22 always TSC, `st_tx_video_session.c:3396-3398`, `:3412-3414`) | `MTL_OPT_PACING` per session |
| zero-copy TX | partial; ST22 always copies; st20p derive only | reported per unit (`MTL_TXR_COPIED`, `MTL_INFO_DIRECT`) |
| app memory TX | ST20 session, st20p, st22p | attached pools for every essence |
| incomplete delivery | a flag on ST20, ST22, ST30 and the pipelines; ST40 always | `MTL_OPT_RX_INCOMPLETE` everywhere |
| auto-detect | ST20 (raster, fps, packing, interlace; not format), ST40 (interlace, on by default) | `v.detect`, `sc.anc.detect` |
| timing parser | ST20 STAT/META, ST30 STAT/META (200 ms), st20p; not st30p | `MTL_OPT_RX_TIMING_PARSER` everywhere |
| DMA, two RX threads, `rx_burst_size`, DISABLE_MIGRATE | ST20 and st20p only | options |
| FORCE_NUMA | not ST40, ST41; st40p "NOT SUPPORTED" | `MTL_OPT_NUMA` everywhere |
| frame RX | not ST41 (RTP only) | frame RX for fastmeta |

Silent downgrades today, reported in the unified API: RL → TSC pacing (AUTO, ST22, shared TX
queue), chain → copy TX, DMA → CPU, 2 → 1 port pruning. Network headers are fixed today (TTL 64,
DSCP 0, no VLAN, IPv4 only, MTU ≤ 1500); unified: `mtl_flow.dscp`, `.ttl`, with `ip_family` and
`vlan` reserved.
