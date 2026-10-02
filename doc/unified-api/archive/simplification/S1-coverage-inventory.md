# S1 — Coverage inventory: what today's MTL lets users do, and where the unified sketch covers it

| Field | Value |
|---|---|
| Purpose | Safety net for the simplification pass: a checkable list of every use case today's public API offers, so no simplification silently drops one |
| Baseline | `main` @ `545a266a`; legacy headers in `include/`; unified sketch revision 3 (`sketch/include/mtl/experimental/mtl_unified.h`, `mtl_simple.h`, `mtl_debug.h`) |
| Date | 2026-10-01 |
| Status | Inventory only. No item here is a decision; every "candidate cut" needs the project owner's approval |
| Requirement update applied | RTP passthrough (`*_TYPE_RTP_LEVEL` + `get_mbuf`/`put_mbuf`) MUST stay: classed COMMON, not LEGACY-ONLY. Session-level headers are to be hidden, so every use case reachable only through `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h` or `st_api.h` is marked and listed in §3 |

## 0. Method and legend

1. Read every installed header end-to-end (`include/*.h`, `include/experimental/st20_combined_api.h`); `st_convert_api.h` and `st_convert_internal.h` were counted, not read in detail (47 + 58 functions).
2. Read research notes 02, 07 and 08, and the design docs where they map legacy features (03 §7.2, 06 §13, 09 §1–§8, 11 §5–§6, DECISIONS.md).
3. Counted consumers with `git grep -l -w <symbol>` per consumer tree at HEAD, for all 212 flag names, all 377 public function names and ~80 ops/init fields. The counts are **files**, not call sites.
4. Checked each item against the sketch header symbol by symbol. The header is normative (sketch/README.md "header wins"): a feature described in prose but absent from the header is **NOT COVERED** here, with the prose cited.

**Consumer codes** (number = files that reference the symbol):

- `SMP` = `app/` (samples, perf, tools), `RXTX` = `tests/tools/RxTxApp`, `FF` = FFmpeg plugin, `GST` = GStreamer plugin, `OBS` = OBS plugin, `MXL` = `ecosystem/MTL_with_MXL`.
- `PLG` = `plugins/`, `PY` = Python SWIG, `RS` = Rust, `KT` = KahawaiTest (`tests/integration_tests`), `UT` = unit tests.
- `ext:bobi` = the public bobi.studio engine (R08 §1.3). "none" = no in-tree consumer found.

**Hdr codes**: `C` mtl_api.h, `St` st_api.h, `S20` st20_api.h (incl. ST22 session), `S30` st30_api.h, `S40` st40_api.h, `S41` st41_api.h, `P` st_pipeline_api.h, `P30` st30_pipeline_api.h, `P40` st40_pipeline_api.h, `SCH` mtl_sch_api.h, `SHM` mtl_lcore_shm_api.h, `EXP` experimental/st20_combined_api.h, `CVT` st_convert_api.h + st_convert_internal.h.

**Sess column** (requirement update): `S!` = the capability is reachable **only** through a session-level header (no pipeline API offers it); `t` = a type, enum or helper declared in a session-level header that pipeline users also need (hiding the header removes it from them too). Blank = not declared in a session-level header.

**Coverage column**:

- `COVERED`: the sketch has a direct home (symbol given). `CHANGED`: covered, but the model or default differs (difference stated). `PARTIAL`: part covered (missing part stated).
- `DEBUG`: moved to `mtl_debug.h`, debug builds only. `NOT COVERED`: no symbol in the sketch header; "(later: …)" when the design defers it.
- `BRIDGE`: reachable only by opening a legacy `mtl_init` handle and wrapping it with `mtl_instance_from_legacy`, which needs `mtl_api.h`.

**Class**: `CORE` (most apps need it), `COMMON` (a significant minority: frameworks, gateways, test tools), `RARE` (specialist, tuning, debugging), `LEGACY-ONLY` (looks obsolete or unused; a **candidate cut that NEEDS APPROVAL**, evidence in the row and in §6).

## 1. Summary

| Count | Value |
|---|---|
| Use cases inventoried | 291 rows (U-001 … U-418; IDs are grouped by area, so numbers have gaps); 290 classed, U-414 is a note |
| CORE | 33 |
| COMMON | 113 |
| RARE | 122 |
| LEGACY-ONLY (candidate cuts, need approval) | 22 |
| COVERED / CHANGED / DEBUG | 135 / 25 / 3 |
| PARTIAL | 34 |
| NOT COVERED by the sketch header (incl. "later") | 93 |
| Reachable only through session-level headers (`S!`) | 31 |
| Declared in session-level headers and needed by pipeline users (`t`) | 38 |

The largest uncovered clusters:

- RTP passthrough (U-340…U-349), COMMON by owner requirement.
- Instance and port tuning that today lives in `mtl_init_params` and `MTL_FLAG_*` (U-014…U-031, U-041…U-044, U-057…U-086).
- The codec/converter plugin ABI (U-380…U-385) and the standalone conversion library (U-393).
- Lcore borrowing and user tasklets (U-067…U-069), IOVA access and user DMA (U-111, U-116), log routing (U-019…U-021).

## 2. Inventory

### 2.1 Instance lifecycle and init parameters

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-001 | Open an instance on a list of ports | `mtl_init`, `mtl_init_params.port[]`, `num_ports` | C | | every consumer (SMP 51, KT 8, MXL 8, PY 7, FF 4) | COVERED `mtl_instance_open`, `mtl_instance_params.ports[].name`, `port_count`; `mtl_instance_open_simple("bdf=ip,…")` | CORE |
| U-002 | Close an instance | `mtl_uninit` | C | | every consumer | COVERED `mtl_instance_release` (refcounted; the instance outlives DESTROYING sessions) | CORE |
| U-003 | Re-open an instance in the same process after closing it (#1341, broken today) | `mtl_uninit` then `mtl_init` | C | | FF, GST, dvled (worked around) | CHANGED: works by design — EAL kept alive across the last release (03 §7.1) | COMMON |
| U-004 | Share one instance between independent components (plugin elements, demuxers) | app-side singletons `mtl_common.c:25-28`, `gst_mtl_common.c:12-18` | C | | FF, GST, ext dvled | COVERED `mtl_instance_acquire_default` + published merge table, `mtl_instance_info.ignored_fields` | COMMON |
| U-005 | Start/stop the device separately from init; auto start/stop | `mtl_start`, `mtl_stop`, `MTL_FLAG_DEV_AUTO_START_STOP` | C | | SMP 35, KT 31, FF 6, MXL 7, PY 7 | CHANGED: no instance start/stop; each session has `mtl_session_start/stop` (and groups); the flag has no unified equivalent and is not needed | CORE |
| U-006 | Abort everything from a signal handler | `mtl_abort` | C | | SMP 3, RXTX 1 | PARTIAL: `mtl_instance_interrupt_all` (AS-safe) wakes every waiter; process exit without release is supported; no "abort the device" verb | COMMON |
| U-007 | Static source IP per port | `sip_addr[]`, `mtl_para_sip_set` | C | | all | COVERED `mtl_port_spec.sip` | CORE |
| U-008 | Netmask per port | `netmask[]` | C | | SMP 1, RXTX 2, RS 7 | COVERED `mtl_port_spec.prefix_len` (0 = /24) | COMMON |
| U-009 | Default gateway per port | `gateway[]` | C | | SMP 1, RXTX 2, RS 7 | COVERED `mtl_port_spec.gateway` | COMMON |
| U-010 | DHCP address on a DPDK port | `net_proto[] = MTL_PROTO_DHCP` | C | | SMP 1, RXTX 2 | PARTIAL: `sip` all-zero = "DHCP on kernel backends"; the built-in DHCP client on DPDK PMD ports is not stated | RARE |
| U-011 | TX/RX queue counts per port | `tx_queues_cnt[]`, `rx_queues_cnt[]`, `st_tx/rx_sessions_queue_cnt` | C, St | | SMP 3, RXTX 3, FF 2, GST 1, OBS 2, KT 3 | COVERED `mtl_port_spec.tx_queues/rx_queues` (0 = auto), `max_queues` | CORE |
| U-012 | Force a port's NUMA node | `port_params[].flags MTL_PORT_FLAG_FORCE_NUMA` + `socket_id` | C | | RXTX 1 | COVERED `mtl_port_spec.numa = MTL_NUMA(n)` | RARE |
| U-013 | Bring up with some ports link-down | `MTL_FLAG_ALLOW_DOWN_PORTS`, `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION` | C | | GST 2, RXTX 1 | CHANGED: ports always open with oper DOWN; legs never pruned (09 §7.2) | COMMON |
| U-014 | ICE PF rate-limit burst size devarg | `port_params[].rl_burst_size` | C | | none | NOT COVERED (`port_flags` reserved) | RARE |
| U-015 | Restrict MTL to an lcore list | `lcores` | C | | SMP 2, OBS 2, MXL 7, PY 7, RXTX 1 | COVERED `mtl_instance_params.lcores` (ignored on a second acquire) | COMMON |
| U-016 | Choose the DPDK main lcore | `main_lcore` | C | | none (#1179) | NOT COVERED | RARE |
| U-017 | Log level at init | `log_level` | C | | SMP 3, RXTX 2, FF, GST, OBS, MXL 7, RS 6, KT 5 | COVERED `log_level` (`enum mtl_unified_log_level`, renumbered) | CORE |
| U-018 | Change/read the log level at runtime | `mtl_set_log_level`, `mtl_get_log_level` | C | | KT 1 | NOT COVERED (open time only) | RARE |
| U-019 | Route MTL logs into the app's logger (#657) | `mtl_set_log_printer` | C | | RXTX 1, UT 3 | NOT COVERED | COMMON |
| U-020 | Custom log line prefix | `mtl_set_log_prefix_formatter` | C | | RXTX 1 | NOT COVERED | RARE |
| U-021 | Log to a FILE stream | `mtl_openlog_stream` | C | | RXTX 1 | NOT COVERED | RARE |
| U-022 | Periodic stats dump with a user callback | `stat_dump_cb_fn`, `dump_period_s`, `priv` | C | | RXTX 1, KT 1 | CHANGED: no periodic callback; observers poll getters / EQ (08 §6); the library's own periodic log dump period is not configurable | COMMON |
| U-023 | Force IOVA mode VA/PA; query it | `iova_mode`, `mtl_iova_mode_get` | C | | RXTX 1, KT 1, SMP 1 | PARTIAL: `mtl_port_caps.iova_va` reports; cannot be set | RARE |
| U-024 | RSS classification mode; query it | `rss_mode`, `mtl_rss_mode_get` | C | | SMP 1, RXTX 2, PY 7, KT 1 | NOT COVERED (shared RSS automatic only) | RARE |
| U-025 | Schedulers for RSS dispatch | `rss_sch_nb[]` | C | | RXTX 1 | NOT COVERED | RARE |
| U-026 | NIC descriptor ring sizes | `nb_tx_desc`, `nb_rx_desc` | C | | SMP 1, RXTX 1, MXL 3, PY 7, KT 1 | NOT COVERED | RARE |
| U-027 | RX mempool data room size (avoid mbuf split) | `rx_pool_data_size` | C | | RXTX 1 | NOT COVERED | RARE |
| U-028 | DPDK memzone limit | `memzone_max` | C | | none | NOT COVERED | RARE |
| U-029 | Maximum UDP payload size | `pkt_udp_suggest_max_size` | C | | none | NOT COVERED (03 §7.2 says "per-session in the unified API"; no field in the header) | RARE |
| U-030 | ARP cache timeout for unicast | `arp_timeout_s` | C | | RXTX 1 | NOT COVERED (unified ARP never blocks) | RARE |
| U-031 | Name the DMA devices (CBDMA/DSA) the instance may use | `dma_dev_port[]`, `num_dma_dev_port`, `mtl_para_dma_port_set` | C | | SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 1 | NOT COVERED: sessions can request `caps.dma_offload`, but nothing names or probes DMA devices (BRIDGE only) | COMMON |
| U-032 | Deprecated session-count sizing | `tx_sessions_cnt_max`, `rx_sessions_cnt_max` | C | | MXL 1 | NOT COVERED (deprecated "Use tx_queues_cnt") | LEGACY-ONLY |
| U-033 | Instance fixed/varying info and per-type session counts | `mtl_get_fix_info`, `mtl_get_var_info`, `st_get_var_info` | C, St | t | RXTX 1, KT 7 | COVERED `mtl_instance_get_info/status`, `mtl_instance_list_sessions` + `session_info.essence` | RARE |
| U-034 | Library version string | `mtl_version()`, `MTL_VERSION` | C | | SMP 1, PY 1, RS 1, KT 1 | PARTIAL: `mtl_version_num()` numeric only | COMMON |
| U-035 | Is MtlManager alive | `mtl_is_manager_alive` | C | | none outside lib | COVERED `mtl_instance_status.manager` | RARE |
| U-036 | Survive MtlManager loss | implicit shm fallback | C | | implicit | COVERED `MTL_INSTANCE_MANAGER_OPTIONAL`, `MTL_EVENT_MANAGER_LOST` | COMMON |

### 2.2 Time source and PTP

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-040 | Built-in PTP client | `MTL_FLAG_PTP_ENABLE` | C | | SMP 2, RXTX, FF, GST, PY 7, KT 2 | COVERED `time_source = MTL_TIME_SOURCE_PTP_BUILTIN` and flag `MTL_INSTANCE_PTP_BUILTIN` (duplicate knobs) | COMMON |
| U-041 | PTP PI servo and its gains | `MTL_FLAG_PTP_PI`, `kp`, `ki` | C | | RXTX 1, FF 1, RS 1 | NOT COVERED | RARE |
| U-042 | PTP unicast delay request | `MTL_FLAG_PTP_UNICAST_ADDR` | C | | RXTX 1, FF 1 | NOT COVERED | RARE |
| U-043 | Built-in phc2sys | `MTL_FLAG_PHC2SYS_ENABLE` | C | | SMP 1, RXTX 1 | NOT COVERED (`PHC_EXTERNAL` assumes an external phc2sys) | RARE |
| U-044 | PTP time from TSC | `MTL_FLAG_PTP_SOURCE_TSC` | C | | SMP 1, RXTX 1, KT 1 | NOT COVERED (TSC clock is not public, 06 §2) | RARE |
| U-045 | App-supplied time function | `ptp_get_time_fn` + `priv` | C | | SMP 1, RXTX 2, FF 1 (CLOCK_TAI), KT 5 | CHANGED: `MTL_TIME_SOURCE_USER` + `mtl_time_user_update` (pushed pairs, not a pull callback); `MTL_TIME_SOURCE_CLOCK_TAI` / `SYSTEM_TAI` | COMMON |
| U-046 | Notify on each PTP sync | `ptp_sync_notify`, `mtl_ptp_sync_notify_meta` | C | | RXTX 1 | COVERED `MTL_EVENT_TIME_STATE`, `TIME_STEP`, `mtl_time_get_status` (`offset_ns`, `utc_offset_s`) | RARE |
| U-047 | HW RX timestamps | `MTL_FLAG_ENABLE_HW_TIMESTAMP` | C | | SMP 1, RXTX 1, PY 1, RS 1, KT 4 | COVERED `MTL_INSTANCE_HW_TIMESTAMP`, `caps.hw_timestamps`, `MTL_RXT_HW_ARRIVAL` | RARE |
| U-048 | Read current PTP/TAI time | `mtl_ptp_read_time`, `mtl_ptp_read_time_raw` | C | | SMP 2, RXTX 4, KT 11, ext:bobi | COVERED `mtl_time_now`, `mtl_time_cross_timestamp`, `mtl_time_convert` | COMMON |

### 2.3 Scheduler, lcores, threading

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-050 | Schedulers as pthreads (containers) | `MTL_FLAG_TASKLET_THREAD` | C | | RXTX 1, RS 1, KT 1 | COVERED `MTL_INSTANCE_TASKLET_THREAD` | COMMON |
| U-051 | Schedulers sleep when idle | `MTL_FLAG_TASKLET_SLEEP` | C | | RXTX 1 | COVERED `MTL_INSTANCE_TASKLET_SLEEP` | COMMON |
| U-052 | Toggle sleep per scheduler at runtime | `mtl_sch_enable_sleep` | C | | KT 1, UT 2 | NOT COVERED | RARE |
| U-053 | Maximum scheduler sleep | `mtl_sch_set_sleep_us` | C | | RXTX 1 | NOT COVERED | RARE |
| U-054 | Separate lcore for RX video | `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` | C | | SMP 4, RXTX 2, FF 1, KT 3 | PARTIAL: flag `MTL_INSTANCE_RX_SEPARATE_VIDEO_LCORE` exists but is a "legacy scheduler policy" (ignored on second acquire) | COMMON |
| U-055 | Migrate busy video sessions; opt a session out | `MTL_FLAG_TX/RX_VIDEO_MIGRATE`, `ST20(P)_RX_FLAG_DISABLE_MIGRATE` | C, S20, P | | FF 1, RXTX 2, KT 1 | CHANGED: flags exist; unified sessions never migrate (Q-THR-6), so DISABLE_MIGRATE is moot | RARE |
| U-056 | Data quota per scheduler | `data_quota_mbs_per_sch` | C | | RXTX 1, MXL 5, KT 1 | COVERED `sched_quota_mbs` | COMMON |
| U-057 | Audio sessions per scheduler cap | `tx/rx_audio_sessions_max_per_sch` | C | | RXTX 2 | NOT COVERED | RARE |
| U-058 | Tasklets per scheduler | `tasklets_nb_per_sch` | C | | none | NOT COVERED | RARE |
| U-059 | Dedicated lcore for system (CNI) tasks | `MTL_FLAG_DEDICATED_SYS_LCORE` | C | | RXTX 1 | NOT COVERED | RARE |
| U-060 | CNI on a thread vs a tasklet | `MTL_FLAG_CNI_THREAD`, `MTL_FLAG_CNI_TASKLET` | C | | RXTX 1, RS 1, KT 3 | NOT COVERED | RARE |
| U-061 | Tasklet time measurement | `MTL_FLAG_TASKLET_TIME_MEASURE` | C | | RXTX 1, RS 1 | COVERED `MTL_INSTANCE_TASKLET_TIME_MEASURE`, `mtl_sched_status.tasklet_p9999_ns` | RARE |
| U-062 | Bind MTL threads to the NIC NUMA node (default) or not | `MTL_FLAG_BIND_NUMA` (never read), `MTL_FLAG_NOT_BIND_NUMA` | C | | BIND: SMP 2, FF, OBS 2, PY 7, RS 6; NOT_BIND: RXTX 1 | PARTIAL: `MTL_INSTANCE_BIND_NUMA` exists, but with the zero-default rule a clear bit would mean "not bound", the inverse of today; no opt-out flag | COMMON |
| U-063 | Do not bind the process to the NIC NUMA node | `MTL_FLAG_NOT_BIND_PROCESS_NUMA` | C | | none | NOT COVERED | RARE |
| U-064 | Allow cores across NUMA nodes | `MTL_FLAG_ALLOW_ACROSS_NUMA_CORE` | C | | RXTX 1, KT 1 | NOT COVERED | RARE |
| U-065 | 512-bit SIMD RX/TX burst | `MTL_FLAG_RXTX_SIMD_512` | C | | RXTX 1, KT 1, UT 1 | NOT COVERED | RARE |
| U-066 | Query CPU SIMD level | `mtl_get_simd_level`, `mtl_get_simd_level_name` | C | | SMP 14 (print), RS 1, KT 1 | NOT COVERED | RARE |
| U-067 | Borrow an MTL lcore for an app thread | `mtl_get_lcore`, `mtl_bind_to_lcore`, `mtl_put_lcore` | C | | SMP 14, RXTX 1, KT 1 | NOT COVERED | COMMON |
| U-068 | App-created scheduler (dedicated core) | `mtl_sch_create/start/stop/free`, `mtl_sch_ops` | SCH | | KT 1 | NOT COVERED (`mtl_sch_run_once` under `MTL_UNIFIED_LATER` is a different thing) | RARE |
| U-069 | App tasklet on an MTL scheduler | `mtl_sch_register_tasklet/unregister`, `mtl_tasklet_ops` (start, stop, handler, `advice_sleep_us`) | SCH | | KT 1 | NOT COVERED: the design lets a user tasklet call the inline-safe DP subset (C10, D-47) but there is no unified way to register one | RARE |
| U-070 | Session's scheduler index | `st20/st22/st20p_*_get_sch_idx` | S20, P | t | RXTX 1, KT 1 | COVERED `mtl_session_info.sched_index` | RARE |
| U-071 | Name a thread | `mtl_thread_setname` | C | | RXTX 20 | NOT COVERED (helper) | RARE |
| U-072 | Legacy shm lcore allocator: print / clean stale entries | `mtl_lcore_shm_print`, `mtl_lcore_shm_clean` | SHM | | SMP 1 (tool) | NOT COVERED | LEGACY-ONLY |
| U-073 | Sleep / busy-delay helpers | `mtl_sleep_us`, `mtl_delay_us` | C | | KT 1 / none | NOT COVERED | LEGACY-ONLY |

### 2.4 Port data-path tuning (instance flags today)

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-075 | Shared TX queue | `MTL_FLAG_SHARED_TX_QUEUE` | C | | SMP 1, RXTX 1, RS 1 | COVERED per session `options.tx_queue = MTL_TXQ_SHARED` | COMMON |
| U-076 | Shared RX queue | `MTL_FLAG_SHARED_RX_QUEUE` | C | | SMP 1, RXTX 1, RS 1 | NOT COVERED | RARE |
| U-077 | RX through the CNI queue | `MTL_FLAG_RX_USE_CNI` | C | | RXTX 1 | NOT COVERED | RARE |
| U-078 | Flow rules on UDP port only | `MTL_FLAG_RX_UDP_PORT_ONLY` | C | | RXTX 1 | NOT COVERED | RARE |
| U-079 | No system RX queues | `MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES` | C | | RXTX 1 | NOT COVERED | RARE |
| U-080 | Promiscuous RX | `MTL_FLAG_NIC_RX_PROMISCUOUS` | C | | RXTX 1 | NOT COVERED | RARE |
| U-081 | No IGMP join (SDN fabrics deliver directly) | `MTL_FLAG_NO_MULTICAST` | C | | RXTX 1 | NOT COVERED | RARE |
| U-082 | virtio_user exception path | `MTL_FLAG_VIRTIO_USER` | C | | RXTX 1 | NOT COVERED | RARE |
| U-083 | AF_XDP copy mode only | `MTL_FLAG_AF_XDP_ZC_DISABLE` | C | | RXTX 1, KT 1 | NOT COVERED | RARE |
| U-084 | One mempool for all RX / TX queues | `MTL_FLAG_RX_MONO_POOL`, `MTL_FLAG_TX_MONO_POOL` | C | | RXTX 1, KT 1 | NOT COVERED | RARE |
| U-085 | Force TX copy (no chained mbuf) | `MTL_FLAG_TX_NO_CHAIN` | C | | RXTX 1, KT 1 | NOT COVERED (`pool.data_path` can require DIRECT, not force COPY) | RARE |
| U-086 | Skip TX burst packet check | `MTL_FLAG_TX_NO_BURST_CHK` | C | | RXTX 1 | NOT COVERED | RARE |
| U-087 | Random / multiple UDP source ports | `MTL_FLAG_RANDOM_SRC_PORT`, `MTL_FLAG_MULTI_SRC_PORT` | C | | RXTX 1, KT 2 | COVERED `options.src_port_mode` (now per session) | RARE |
| U-088 | Pacing engine per port (AUTO/RL/TSC/TSN/PTP/BE/TSC_NARROW) | `mtl_init_params.pacing`, `enum st21_tx_pacing_way` | C | | RXTX, KT, FF (via session fields too) | PARTIAL: per-session `caps.pacing_class` (HW_RATE / HW_LAUNCH / SW / BEST_EFFORT) and granted `pacing_profile`; TSC_NARROW and PTP ways cannot be requested; no port default | COMMON |
| U-089 | UDP transport remnants | `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | C | | SMP 1 (`sample_util.c:276` sets the flag), RS 1 | NOT COVERED (stack deleted in `2b182cd87`) | LEGACY-ONLY |
| U-090 | Simulated loss on redundant TX streams | `MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS`, `port_packet_loss[]` | C | | KT 1 | DEBUG `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` per leg | RARE |

### 2.5 Ports and backends

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-092 | DPDK PMD port (PCI BDF), PF or VF | `MTL_PMD_DPDK_USER` | C | | all | COVERED `MTL_BACKEND_DPDK_PMD` | CORE |
| U-093 | Native AF_XDP port | `native_af_xdp:<if>`, `MTL_PMD_NATIVE_AF_XDP` | C | | RXTX, KT | COVERED name prefix, `MTL_BACKEND_AF_XDP` | COMMON |
| U-094 | Kernel socket port (experimental) | `kernel:<if>` | C | | RXTX, KT | COVERED `MTL_BACKEND_KERNEL_SOCKET` | RARE |
| U-095 | DPDK AF_XDP and DPDK AF_PACKET PMDs (experimental) | `dpdk_af_xdp:`, `dpdk_af_packet:` | C | | RXTX | COVERED in `enum mtl_backend` (not in the `mtl_port_spec.name` comment) | LEGACY-ONLY |
| U-096 | Backend type by name, DPDK-based or AF_XDP test | `mtl_pmd_by_port_name`, `mtl_pmd_is_dpdk_based`, `mtl_pmd_is_af_xdp` | C | | SMP 1, RXTX 3, FF 1, GST 1, PY 7, KT 2 | COVERED `mtl_port_get_caps().backend` after open; no pre-open name parse | COMMON |
| U-097 | Port NUMA node | `mtl_get_numa_id` | C | | KT 1 | COVERED `mtl_port_caps.numa` | RARE |
| U-098 | Port IP / netmask / gateway in use (incl. DHCP lease) | `mtl_port_ip_info` | C | | RXTX 1, KT 1 | NOT COVERED (`mtl_port_find` by IP only) | RARE |
| U-099 | Port I/O counters | `mtl_get_port_stats`, `struct mtl_port_status` (pkts, bytes, errors, hw drops, nombuf) | C | | RXTX 1, ext:bobi | PARTIAL: `mtl_port_get_status` (pkts, errors, `rx_missed`); bytes and nombuf only if `mtl_port_stat_get` keys exist | COMMON |
| U-100 | Reset port counters | `mtl_reset_port_stats` | C | | RXTX 1 | CHANGED: cumulative counters only; deltas from snapshots | RARE |
| U-101 | Interface IP helper | `mtl_get_if_ip` | C | | none | NOT COVERED | LEGACY-ONLY |
| U-102 | Init-param setters for bindings | `mtl_para_*_set/get`, `mtl_p_port`, `mtl_r_port`, `mtl_p/r_sip_addr` | C | | PY 7, MXL 6, SMP 1, RXTX 1 | CHANGED: plain POD fields, `mtl_instance_open_simple`; SWIG helpers become unnecessary | COMMON |
| U-103 | Windows (DPDK only) | `lib/windows/` | C | | MSVC sample | COVERED (portable wait object, `MTL_E*`) | RARE |
| U-104 | Multi-process on one NIC via SR-IOV + MtlManager | implicit | C | | all multi-app deployments | COVERED (manager state, merge table) | COMMON |

### 2.6 Memory, DMA and zero-copy plumbing

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-110 | Hugepage alloc/zalloc/free | `mtl_hp_malloc`, `mtl_hp_zmalloc`, `mtl_hp_free` | C | | SMP 24, RXTX 5, KT 4 | CHANGED: `mtl_mem_alloc` returns a region; buffers via `mtl_buffer_create`; no malloc-style pointer API | COMMON |
| U-111 | Get the IOVA of memory or of a frame plane | `mtl_hp_virt2iova`, `st_frame.iova[]`, `st_frame_iova`, `st_ext_frame.iova[]`, `st20_ext_frame.buf_iova` | C, P, S20 | t | SMP 18, PY 4, KT 2 | NOT COVERED: views carry `addr` and `device_handle`, never an IOVA | COMMON |
| U-112 | DMA-map app memory for zero-copy | `mtl_dma_map`, `mtl_dma_unmap` | C | | SMP 3, MXL 2, KT 5 | COVERED `mtl_mem_import`, `mtl_mem_destroy`, `mtl_mem_map_device` | COMMON |
| U-113 | Page-aligned DMA-mapped block | `mtl_dma_mem_alloc/free/addr/iova` | C | | SMP 5, KT 2 | PARTIAL: `mtl_mem_alloc`; no IOVA getter | RARE |
| U-114 | Page size and alignment | `mtl_page_size`, `mtl_size_page_align` | C | | SMP 3, MXL 1, KT 5 | COVERED `mtl_port_caps.page_size`, `hugepage_sizes` (align helper not) | RARE |
| U-115 | Fast memcpy used by the library | `mtl_memcpy`, `mtl_memcpy_action` | C | | SMP 21, RXTX 15, FF 6, OBS 1, PLG 2, RS 2, PY 2, KT 4 | PARTIAL: `mtl_lease_copy_in/out` for leased buffers only | COMMON |
| U-116 | User DMA engine: copy, fill, submit, completions | `mtl_udma_create/free/copy/fill/fill_u8/submit/completed` | C | | SMP 15 (dma samples), KT 2 | NOT COVERED | RARE |
| U-117 | RX frames in GPU VRAM | `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS`, `gpu_context`; `st20_rx_ops.gpu_direct_framebuffer_in_vram_device_address` | P, S20 | | SMP 1, FF 1 (session field: none) | NOT COVERED (later: `MTL_MEM_DEVICE`; GPU-pinned host memory import is v1, 05 §10) | RARE |

### 2.7 Session plumbing common to every essence

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-120 | Create / free a session | `st*_tx/rx_create`, `*_free` | S20–S41, P* | | every consumer | COVERED `mtl_<essence>_session_create`, `mtl_session_destroy` | CORE |
| U-121 | Session is live from create (no start) | implicit | all | | all | CHANGED: explicit `mtl_session_start` (now / at TAI / at media index) and `mtl_session_stop` (DRAIN / FLUSH) | CORE |
| U-122 | TX destination IP + UDP per leg | `dip_addr[]`, `udp_port[]`, `st_tx_port` | S*, P | | all | COVERED `mtl_flow.ip`, `udp_port` | CORE |
| U-123 | RX multicast group / unicast source + UDP per leg | `ip_addr[]`, `udp_port[]`, `st_rx_port` | S*, P | | all | COVERED `mtl_flow.ip`, `udp_port` | CORE |
| U-124 | SSM source filter | `mcast_sip_addr[]` | S*, P | | RXTX 10, MXL 1 | COVERED `mtl_flow.source_filter` | COMMON |
| U-125 | Bind a leg to a named instance port | `port[][]` names | S*, P | | all | COVERED `mtl_flow.port = MTL_FLOW_PORT(n)`, `mtl_port_find` | CORE |
| U-126 | ST 2022-7 dual-leg TX and RX merge | `num_port = 2` | S*, P | | many | COVERED `leg_count`, `flows[2]`, per-leg status/stats | CORE |
| U-127 | Payload type TX; RX check (0 = off) | `payload_type` | S*, P | | all | COVERED `mtl_flow.payload_type` | CORE |
| U-128 | SSRC TX (0 = random); RX filter (0 = off) | `ssrc` | S*, P | | RXTX 4, KT 5, RS 1 | COVERED `mtl_flow.ssrc` | COMMON |
| U-129 | UDP source port | `udp_src_port[]` | S*, P | | none | COVERED `mtl_flow.udp_src_port` | RARE |
| U-130 | Static destination MAC (no ARP) | `tx_dst_mac[]` + `*_TX_FLAG_USER_P_MAC/USER_R_MAC` | S*, P | | SMP 2, RXTX 10 | COVERED `MTL_FLOW_USER_MAC` + `dst_mac` | COMMON |
| U-131 | Session name | `name` | S*, P | | many | COVERED `mtl_session_config.name` (copied, unique) | COMMON |
| U-132 | Callback private pointer | `priv` | S*, P | | every callback user | CHANGED: `user_cookie` on config, buffer and submission; no callbacks | CORE |
| U-133 | Per-session NUMA override | `*_FLAG_FORCE_NUMA` + `socket_id` (not ST40/41) | S*, P | | RXTX 11 | COVERED `options.numa` | RARE |
| U-134 | Dedicated TX queue for audio/ANC/fastmeta | `ST30/40/41(P)_TX_FLAG_DEDICATE_QUEUE` | S30, S40, S41, P30, P40 | | RXTX 1, KT 1 | COVERED `options.tx_queue = MTL_TXQ_DEDICATED` | RARE |
| U-135 | Change TX destination at runtime | `st*_tx_update_destination`, `st_tx_dest_info` | S*, P* | t | MXL 1, KT 1 | COVERED `mtl_session_update_flows` (all legs atomically, `mtl_activation`) | COMMON |
| U-136 | Change RX source at runtime | `st*_rx_update_source`, `st_rx_source_info` | S*, P* | t | KT 1 | COVERED `mtl_session_update_flows` | COMMON |
| U-137 | RX burst size | `rx_burst_size` (ST20, st20p) | S20, P | | SMP 2, RXTX 3, PY 1 | COVERED `options.rx_burst_size` | RARE |
| U-138 | Number of frame buffers | `framebuff_cnt` | S*, P* | | all | COVERED `pool.count` (`max_count` reported, 8 until E11) | CORE |
| U-139 | App-managed flows and IGMP; queue ID query | `*_RX_FLAG_DATA_PATH_ONLY`, `*_rx_get_queue_meta`, `st_queue_meta` | S*, P, P40 | t | flag: none; get_queue_meta: KT 3 | NOT COVERED (Q-MODE-5: verify; NULL-flow dereference suspected, `mt_queue.c:56`) | LEGACY-ONLY |
| U-140 | pcapng capture of RX packets | `st20/st22/st20p/st22p/st20rc_rx_pcapng_dump`, `st_pcap_dump_meta` | S20, P, EXP | t | RXTX 1, KT 1 | NOT COVERED (later: 09 §7 "CP call that arms a capture") | RARE |
| U-141 | RTCP retransmission for ST20/ST22 | `*_FLAG_ENABLE_RTCP`, `st_tx_rtcp_ops.buffer_size`, `st_rx_rtcp_ops` (nack interval, bitmap, skip window) | S20, P | t | RXTX 1, KT 4 | NOT COVERED (later: `next` extension block, video/cvideo only) | RARE |
| U-142 | RTCP flags on ST30/40/41/st40p (no lib consumer) | `ST30/40/41(P)_*_FLAG_ENABLE_RTCP` | S30, S40, S41, P40 | | RXTX 1 (passes through) | NOT COVERED (removed, Q-MODE-8) | LEGACY-ONLY |
| U-143 | TX queue hang-detect timeout | `tx_hang_detect_ms` (ST20, st20p) | S20, P | | none | NOT COVERED | RARE |
| U-144 | TX queue recovery / fatal notification | `ST_EVENT_RECOVERY_ERROR`, `ST_EVENT_FATAL_ERROR` (video only) | St | t | RXTX 2, KT 2 | COVERED `MTL_EVENT_SESSION_RECOVERY`, `SESSION_STATE` → ERROR with reason (all essences) | COMMON |
| U-145 | Create fails with a reason | NULL + log line | all | | all | COVERED negative `MTL_E*` + `mtl_last_error` + `enum mtl_state_reason` | CORE |

### 2.8 TX timing and pacing (all essences)

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-150 | Library-paced TX on the epoch grid | default | all | | all | COVERED `media_mode = MTL_MEDIA_AUTO`, epoch timeline | CORE |
| U-151 | User pacing: send at a TAI time (VRX-aligned) | `*_TX_FLAG_USER_PACING` + `timestamp`/`tfmt` | S*, P* | | RXTX, GST 1, KT 4 | COVERED `media_mode = TAI`, `launch.mode = NOT_BEFORE` (MEDIA_CLK input now converted, not ignored) | COMMON |
| U-152 | Exact user pacing (first packet exactly at t) | `ST20/ST20P/ST40/ST40P_TX_FLAG_EXACT_USER_PACING` | S20, S40, P, P40 | | RXTX 1, KT 1, UT 3 | COVERED `launch.mode = MTL_LAUNCH_EXACT` (flagged non-compliant); audio and fastmeta "later" | RARE |
| U-153 | User-chosen RTP timestamp (TAI or media clock) | `*_TX_FLAG_USER_TIMESTAMP` (absent on st30p) | S*, P, P40 | | SMP 3, KT 2, UT 3 | CHANGED: RTP derived from media time (`media_tai_ns` / `media_index`); verbatim via `rtp_mode = MTL_RTP_PASSTHROUGH` + `MTL_SUB_RTP` `rtp_override` | COMMON |
| U-154 | RTP exactly on the epoch (omit TR offset) | `ST20(P)_TX_FLAG_RTP_TIMESTAMP_EPOCH` | S20, P | | RXTX 1, UT 2 | COVERED (the unified default, D-10) | RARE |
| U-155 | Today's default ST20 RTP = TX cursor (media time + first-packet offset) | default `st_tx_video_session.c` | S20, P | | every legacy ST20 sender | CHANGED: not the default; approximated with `media_time_offset_ns` (wire-visible, 11 §5 mixed-API hazard) | COMMON |
| U-156 | RTP timestamp delta / TROFF trim | `rtp_timestamp_delta_us` (st20, st20p, st30, st30p) | S20, S30, P, P30 | | RXTX 2, KT 2, ext:bobi | COVERED `timing.media_time_offset_ns`, `media_index_offset`; `video_config.troffset_ns` | RARE |
| U-157 | Drop frames handed over late | `*P_TX_FLAG_DROP_WHEN_LATE` (pipelines only, needs USER_PACING) | P, P30, P40 | | RXTX 1, KT 1 | COVERED `late_policy = MTL_LATE_DROP` (every mode) | COMMON |
| U-158 | Late-frame notification | `notify_frame_late(priv, epoch_skipped)` | S20–S40, P* | | KT 1 | COVERED TX result status `LATE`/`DROPPED`, `reason`, `slots_skipped_before`, `margin_ns`; counters | COMMON |
| U-159 | Per-epoch vsync event | `*_FLAG_ENABLE_VSYNC` + `ST_EVENT_VSYNC` + `st10_vsync_meta` (ST20/22 only) | St, S20, P | t | RXTX 1, KT 1 | PARTIAL: `MTL_EVENT_EPOCH_TICK` + `MTL_EQ_SUB_EPOCH_TICK` ("sessions that enable it", but no enable switch in the header); `mtl_slot_hint` | RARE |
| U-160 | ST 2110-21 sender type narrow / wide / narrow-linear | `enum st21_pacing`, `transport_pacing` | S20, P | t | RXTX 2, MXL 2, KT 1 | COVERED `sender_type` N / NL / W (video, cvideo) | COMMON |
| U-161 | Query TR offset, TRS, VRX | `st20_tx_get_pacing_params`, `st20p_tx_get_pacing_params` | S20, P | | KT 3 | COVERED `mtl_session_info.troffset_ns`, `trs_ps`, `vrx_full`, `pickup_lead_ns` | RARE |
| U-162 | RL pacing tuning: start VRX, pad interval, static pad, no bulk | `start_vrx`, `pad_interval`, `*_ENABLE_STATIC_PAD_P`, `*_DISABLE_BULK` | S20, P | | RXTX 2 | COVERED `MTL_OPT_VIDEO_START_VRX`, `_PAD_INTERVAL`, `_STATIC_PAD_P`, `_DISABLE_BULK` (ST22 `DISABLE_BULK`: no cvideo key) | RARE |
| U-163 | When is the next frame due / is a frame late | `st_frame_is_late`, `st30_frame_is_late`, `st40_frame_is_late`; RxTxApp `st_app_user_time()` | P, P30, P40 | | KT 1, RXTX | COVERED `mtl_slot_hint` (next index, TAI, submit deadline), `mtl_timeline_index_at`, `mtl_session_row_deadline` | COMMON |

### 2.9 Completion, notification and blocking

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-170 | Blocking get with a timeout | `*P_*_FLAG_BLOCK_GET`, `*_set_block_timeout` | P, P30, P40 | | SMP 4, RXTX, FF, GST, PY 3, MXL 2, KT | COVERED per-call `timeout_ns` on `mtl_tx_acquire` / `mtl_rx_dequeue` | CORE |
| U-171 | Wake a blocked getter for shutdown | `*_wake_block` | P* | | SMP 4, RXTX, MXL 2, KT, ext NUDA9A | COVERED sticky `mtl_session_interrupt` / `uninterrupt` (`-MTL_ECANCELED`) | CORE |
| U-172 | Non-blocking poll for a frame | get without BLOCK_GET → NULL | P* | | GST st40p (1 ms poll) | COVERED timeout 0 → `-MTL_EAGAIN` | COMMON |
| U-173 | "Frame available" notification | `notify_frame_available` | P* | | SMP 10, OBS 1, MXL 1, PLG 2, RS 1, KT 4 | COVERED `mtl_session_get_wait_object` (eventfd / HANDLE), `trywait`, `wait` | COMMON |
| U-174 | Pipeline TX frame done with status | `notify_frame_done(priv, st_frame*)` | P* | | SMP 14, GST 1, MXL 1, KT 10 | COVERED CQ `MTL_CQE_TX_RESULT` (completion mode ALL / EXCEPTIONS) | COMMON |
| U-175 | Session TX frame done by index | `notify_frame_done(priv, idx, meta)` | S20–S41 | S! | SMP, RXTX 5, RS 1 | COVERED `MTL_CQE_TX_RESULT` via `mtl_tx_reap` / `mtl_cq_read` | COMMON |
| U-176 | Zero-hop callback on the tasklet (latency-critical) | every session callback | S20–S41 | S! | MXL (bridge), ext:bobi (slice) | PARTIAL: W2 direct wake `MTL_SESSION_WAKE_DIRECT`; inline notification `mtl_session_set_inline_notify` is `MTL_UNIFIED_LATER` | RARE |
| U-177 | Session event callback | `notify_event(priv, st_event, args)` | S20, P | t | RXTX 2, KT 2 | COVERED event queues (`mtl_eq_*`) for every essence | COMMON |
| U-178 | Abort a frame got for TX/RX | `*_put_frame_abort` | P* | | KT 1, UT 2 | COVERED `mtl_tx_release`, `mtl_rx_release` | COMMON |

### 2.10 ST 2110-20 video TX

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-180 | Pipeline TX loop: get, fill, put | `st20p_tx_get_frame`, `st20p_tx_put_frame` | P | | SMP 9, FF, GST, OBS, MXL 2, PY 2, RS, KT 4 | COVERED `mtl_tx_acquire` + `mtl_tx_submit`; `mtl_simple_tx_*` | CORE |
| U-181 | Session TX pull model: library asks for the next frame index | `st20_tx_ops.get_next_frame` + `st20_tx_frame_meta` | S20 | S! | SMP 8, RXTX 1, RS 1, KT 12 | CHANGED: app push `acquire → submit` (D-02); the in/out meta becomes `mtl_tx_submission` + slot hint | COMMON |
| U-182 | Session framebuffers by index | `st20_tx_get_framebuffer/_size/_count` | S20 | S! | SMP 4, RXTX 1, RS 1, KT 6 | COVERED `mtl_session_get_buffers`, `mtl_buffer_get_view`, `info.unit_bytes`, `pool_count` | COMMON |
| U-183 | Pipeline framebuffer address / size | `st20p_tx_get_fb_addr`, `st20p_tx_frame_size` | P | | SMP 4, FF, GST, MXL 2, PY 2, KT 2 | COVERED as U-182 | COMMON |
| U-184 | Session TX app-owned frames per index | `ST20_TX_FLAG_EXT_FRAME`, `st20_tx_set_ext_frame`, `st20_ext_frame` | S20 | S! | SMP 5, KT 3 | COVERED `mtl_mem_import` + `mtl_buffer_create` + `mtl_session_attach_buffers` + `mtl_tx_acquire_buffer` | COMMON |
| U-185 | Pipeline TX app memory per frame (any address) | `ST20P_TX_FLAG_EXT_FRAME`, `st20p_tx_put_ext_frame`, `st_ext_frame` | P | | SMP 2, GST 1, KT 1 | PARTIAL: attached buffers (≤ `max_count`); arbitrary per-frame layouts need `MTL_POOL_DYNAMIC` / `mtl_tx_acquire_dynamic` (Phase 4) | COMMON |
| U-186 | Two-phase release of app memory | `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE`, `st20p_tx_notify_ext_frame_free` | P | | SMP 2, GST 1, KT 1, UT 3 | COVERED by leases + TX result (+ optional `MTL_CQE_TX_SOURCE_RELEASED`) | COMMON |
| U-187 | Interlaced TX (field = unit, `second_field`) | `interlaced`, `second_field` | S20, P | | many | COVERED `MTL_SCAN_INTERLACED`; parity from the media index | COMMON |
| U-188 | Line padding / stride | `linesize`, `transport_linesize` | S20, P | | SMP 7, GST 2, OBS 1, MXL 1, KT 4 | COVERED `mtl_plane_desc.stride` (DIRECT) | COMMON |
| U-189 | Split-forward: TX tiles from an RX buffer | `app/sample/fwd/rx_st20_tx_st20_split_fwd.c` | S20 | | SMP | COVERED `mtl_session_get_pool_region` + sub-rectangle buffers + `submission.hold` | RARE |
| U-190 | Per-frame user metadata to the receiver | `st20_tx_frame_meta.user_meta`, `st_frame.user_meta` | S20, P | | SMP 7, RXTX 4, KT 1 | COVERED `submission.user_meta` (≤ 1332 B, type/version tag) | COMMON |
| U-191 | Slice-level TX (lines ready so far) | `ST20_TYPE_SLICE_LEVEL`, `query_frame_lines_ready`, `st20_tx_slice_meta` | S20 | S! | SMP 1, RXTX 1, KT 2, ext:bobi | PARTIAL: `MTL_UNIT_ROWS` + `mtl_tx_publish` + `progressive_late`; implementation Phase 6 | RARE |
| U-192 | Packing BPM / GPM / GPM_SL | `packing`, `transport_packing` | S20, P | t | SMP 2, RXTX 2, MXL 2, PY 2 | COVERED `mtl_video_config.packing` | COMMON |
| U-193 | RFC 4175 transport formats (YUV 4:2:2/4:2:0/4:4:4, RGB, 8–16 bit) | `enum st20_fmt` (16 RFC values) | S20 | t | many | PARTIAL: `mtl_video_format` has 11; missing YUV 4:2:0 12/16-bit, YUV 4:4:4 8/16-bit, RGB 16-bit (all implemented in `st_fmt.c`) | COMMON |
| U-194 | Non-RFC 4175 transport (planar 10LE, V210 on the wire) | `ST20_FMT_YUV_422_PLANAR10LE`, `ST20_FMT_V210` | S20 | | RXTX 2, KT 1 | NOT COVERED | LEGACY-ONLY |
| U-195 | Resolution and frame rate | `width`, `height`, `enum st_fps` | St, S20, P | t | all | COVERED `width`, `height`, `mtl_rational fps` + `MTL_FPS_*` (adds 47.95, 48) | CORE |
| U-196 | Frame size, pixel group, bandwidth helpers | `st20_frame_size`, `st20_get_pgroup`, `st20_pgroup`, `st20_get_bandwidth_bps`, `st20_1080p59_yuv422_10bit_bandwidth_mps` | S20 | t | SMP 6, RXTX 4, KT 8 | PARTIAL: `mtl_video_layout_query` (sizes, rows, strides); no pgroup or bandwidth helper | RARE |
| U-197 | Transport format names | `st20_fmt_name`, `st20_name_to_fmt` | S20 | t | SMP 1, PY 1 | PARTIAL: `mtl_video_format_names` (FourCC, GStreamer, FFmpeg names); no MTL name ↔ enum | RARE |

### 2.11 ST 2110-20 video RX

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-200 | Pipeline RX loop: get, read, put | `st20p_rx_get_frame`, `st20p_rx_put_frame` | P | | SMP 12, FF 2, GST, OBS, MXL, PY 3, RS, KT 4 | COVERED `mtl_rx_dequeue` + `mtl_rx_release`; `mtl_simple_rx_*` | CORE |
| U-201 | Session RX push callback, return the buffer later (or reject) | `notify_frame_ready` + `st20_rx_put_framebuff` | S20 | S! | SMP 5, RXTX 4, MXL 4, RS 1, KT 11 | CHANGED: `mtl_rx_dequeue` / `mtl_rx_release` (any thread, any order) | COMMON |
| U-202 | Deliver incomplete frames | `*_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | S20, S30, P, P30 | | SMP 1, RXTX 1, MXL 2, KT 6, ext:bobi | CHANGED: `timing.rx_incomplete` DELIVER is now the default; DISCARD opt-in | COMMON |
| U-203 | Frame integrity status | `enum st_frame_status`, `st_is_frame_complete` | St | t | SMP 5, RXTX 2, MXL 2, KT 4 | COVERED `hdr.status` COMPLETE / INCOMPLETE + `MTL_RX_F_USED_REDUNDANCY` (= RECONSTRUCTED) | CORE |
| U-204 | Packets per leg for signal quality | `pkts_total`, `pkts_recv[]` | S20, P | | KT, RXTX | COVERED `pkts_expected`, `pkts_received[leg]`, `pkts_recovered`, missing ranges | COMMON |
| U-205 | First/last packet arrival TAI | `timestamp_first_pkt`, `timestamp_last_pkt`, `receive_timestamp` | S20, P | | RXTX 4, GST, OBS | COVERED `arrival_first_tai_ns[leg]`, `arrival_last_tai_ns[leg]` | COMMON |
| U-206 | RX RTP timestamp and media time | `rtp_timestamp`, `timestamp` + `tfmt` | S20, P | | GST, OBS, RXTX | COVERED `rx_timing.rtp`, `media_tai_ns`, `media_index` | CORE |
| U-207 | Received byte counts per frame | `frame_total_size`, `frame_recv_size`, `uframe_total_size` | S20 | | KT | PARTIAL: `pkts_*` and missing ranges; no byte count for video | RARE |
| U-208 | RX into a fixed set of app buffers | `st20_rx_ops.ext_frames[]`, `st20p_rx_ops.ext_frames` | S20, P | | SMP 4, KT 6 | COVERED `MTL_POOL_ATTACHED` | COMMON |
| U-209 | RX into an app buffer chosen per frame | `query_ext_frame` (st20, st20p, st22p) + `*_RX_FLAG_EXT_FRAME` | S20, P | | SMP 1, GST 1, MXL 3, KT 3 | PARTIAL: attached pools with `rx_slot_select` ANY_FREE / BY_INDEX; per-frame provide `mtl_rx_provide` is `MTL_UNIFIED_LATER` | COMMON |
| U-210 | Per-buffer app identity | `st20_ext_frame.opaque`, `st_frame.opaque` | S20, P | | GST, MXL | COVERED buffer `user_cookie` | COMMON |
| U-211 | RX copy offload to a DMA engine | `ST20(P)_RX_FLAG_DMA_OFFLOAD`, `st20_rx_dma_enabled` | S20, P | | RXTX 1, FF 1, GST 2, KT 5 | PARTIAL: `caps.dma_offload` request, `path = DIRECT_DMA` reported; no way to name DMA devices (U-031) | COMMON |
| U-212 | Auto-detect width/height/fps/packing/interlace | `*_RX_FLAG_AUTO_DETECT`, `notify_detected`, `st20_detect_meta/reply` | S20, P | t | SMP 1, RXTX 1, KT 1 | PARTIAL: `MTL_EVENT_RX_FORMAT`, `format_changed`; no video auto-detect switch in 0.1 (later) | RARE |
| U-213 | Header split | `*_RX_FLAG_HDR_SPLIT`, `nb_rx_hdr_split_queues` | S20, P, EXP, C | | SMP 1, RXTX 1, KT 1 | NOT COVERED (X for v1; `MTL_PATH_KIND_DIRECT_HDS` is `-MTL_ENOTSUP`) | LEGACY-ONLY |
| U-214 | ST 2110-21 timing parser in stats | `*_TIMING_PARSER_STAT` | S20, S30, P | | RXTX 1 | COVERED `MTL_OPT_RX_TIMING_PARSER` + `mtl_rx_stats.tp_*` | RARE |
| U-215 | Timing parser result per frame and port | `ST20(P)_RX_FLAG_TIMING_PARSER_META`, `st20_rx_tp_meta` (cinst, vrx, ipt, fpt, latency, rtp offset/delta, compliant, failed cause), `st_frame_tp_meta` | S20, P | t | SMP 1, PY 1, KT 2, UT 1, ext:bobi | NOT COVERED per unit (windowed maxima in stats only) | RARE |
| U-216 | Timing parser pass thresholds | `st20(p)_rx_timing_parser_critical`, `st20_rx_tp_pass` | S20, P | t | SMP 1 | NOT COVERED | RARE |
| U-217 | Two RX threads above 40 Gbps | `*_RX_FLAG_USE_MULTI_THREADS` | S20, P | | RXTX 1 | COVERED `options.rx_threads` | RARE |
| U-218 | Slice-level RX (every N lines) | `ST20_TYPE_SLICE_LEVEL`, `slice_lines`, `notify_slice_ready`, `st20_rx_slice_meta` | S20 | S! | SMP 1, RXTX 1, KT 3, ext:bobi | PARTIAL: `MTL_UNIT_ROWS`, `MTL_CQE_RX_PROGRESS`, `mtl_rx_wait_progress`; Phase 6 | RARE |
| U-219 | App converts pixel groups on the tasklet as packets land | `uframe_size`, `uframe_pg_callback`, `st20_rx_uframe_pg_meta` | S20 | S! | RXTX 1, KT 3 | NOT COVERED (deliberate: no app code on tasklets) | LEGACY-ONLY |
| U-220 | Per-packet conversion in st20p | `ST20P_RX_FLAG_PKT_CONVERT` (3 output formats) | P | | KT 1 | NOT COVERED (09 §4) | LEGACY-ONLY |
| U-221 | RX user metadata | `st20_rx_frame_meta.user_meta`, `st_frame.user_meta` | S20, P | | SMP 7, RXTX 4 | COVERED meta area, `meta_bytes`, `user_meta_type/version` | COMMON |
| U-222 | First-packet time to epoch per frame | `st20_rx_frame_meta.fpt` | S20 | | none | PARTIAL: computable from `arrival_first_tai_ns − media_tai_ns`; `tp_fpt_max` | RARE |
| U-223 | Simulated RX packet loss | `*_RX_FLAG_SIMULATE_PKT_LOSS`, `burst_loss_max`, `sim_loss_rate` | S20, S30, P, P30 | | KT 4 | DEBUG `mtl_debug_inject(MTL_FAULT_DROP_PKTS)` | RARE |
| U-224 | Session RX framebuffer size/count | `st20_rx_get_framebuffer_size/_count`, `st20p_rx_get_fb_addr`, `st20p_rx_frame_size` | S20, P | | SMP 4, RXTX 1, FF, GST, RS 1 | COVERED `info.unit_bytes`, `pool_count`, `mtl_session_get_buffers` | COMMON |

### 2.12 ST 2110-22 compressed video

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-230 | Pipeline TX: raw frames encoded by a plugin | `st22p_tx_ops` (`input_fmt`, `codec`, `quality`, `codestream_size`, `codec_thread_cnt`, `device`) | P | | SMP 2, RXTX 1, FF 1, PY 1, RS 1, KT 1 | COVERED `mtl_cvideo_config` (`app_format`, `codec`, `quality`, `codestream_bytes`, `codec_threads`, `plugin_device`) | COMMON |
| U-231 | Pipeline RX: decoded frames | `st22p_rx_ops` (`output_fmt`, `max_codestream_size`) | P | | SMP 1, RXTX 1, FF 1, PY 1, RS 1, KT 1 | COVERED `mtl_cvideo_config` | COMMON |
| U-232 | Codestream passthrough (app brings the codestream) | st22p with codestream fmt; `st22_tx_ops.framebuff_max_size`, `st22_tx_frame_meta.codestream_size` | S20, P | | SMP 3, RXTX 2, MXL 2, KT 2 | COVERED `app_format = 0`, `submission.valid_bytes`; RX `valid_bytes` | COMMON |
| U-233 | Constant vs variable bytes per frame | today VBR-like (packets follow the codestream) | S20, P | | all ST22 users | CHANGED: `rate_mode` CBR by default (padding), `VBR_MAX` = today's behaviour, flagged non-compliant | COMMON |
| U-234 | JPEG-XS box headers off | `ST22(P)_TX/RX_FLAG_DISABLE_BOXES` | S20, P | | none (st22p forces it for non-JPEG-XS) | NOT COVERED | RARE |
| U-235 | Packetization mode | `pack_type` (CODESTREAM; SLICE unsupported) | S20, P | t | FF 1 | COVERED `mtl_cvideo_config.pack_type` (SLICE `-MTL_ENOTSUP`) | COMMON |
| U-236 | Session-level ST22 TX/RX (callbacks, fb address) | `st22_tx/rx_ops`, `st22_tx_get_fb_addr`, `st22_rx_get_fb_addr`, `st22_rx_put_framebuff` | S20 | S! | SMP 1, RXTX 1, MXL 1, KT 1 | CHANGED as U-181/U-201 | RARE |
| U-237 | Interlaced ST22 (codestream per field) | `interlaced` | S20, P | | RXTX | COVERED `interlaced` (CBR per field) | RARE |
| U-238 | ST22 pipeline app memory TX / RX | `ST22P_TX_FLAG_EXT_FRAME` + `st22p_tx_put_ext_frame`; `ST22P_RX_FLAG_EXT_FRAME` + `query_ext_frame` | P | | KT 1 | PARTIAL as U-185 / U-209 | RARE |
| U-239 | Deliver incomplete ST22 frames | `ST22(P)_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | S20, P | | none | COVERED `rx_incomplete` | RARE |

### 2.13 ST 2110-30/31 audio

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-240 | PCM8/16/24 and AM824 | `enum st30_fmt` | S30 | t | FF, GST, RXTX, KT | COVERED `mtl_audio_format` | CORE |
| U-241 | 48 / 96 / 44.1 kHz | `enum st30_sampling` | S30 | t | all audio | COVERED `sample_rate` (Hz) | CORE |
| U-242 | Channel count | `channel` | S30, P30 | | all audio | COVERED `channels` | CORE |
| U-243 | Packet time incl. ST 2110-31 values (1 ms, 125/250/333 µs, 4 ms, 80 µs, 1.09/0.14/0.09 ms) | `enum st30_ptime` | S30 | t | all audio | COVERED `samples_per_packet` (ptime = S / rate) | CORE |
| U-244 | Pipeline audio get/put | `st30p_tx/rx_get_frame`, `put_frame`, `struct st30_frame` | P30 | | SMP 1, RXTX 1, FF 1, GST 1, RS 1, KT 2 | COVERED `acquire` + `submit(sample_count)`, `dequeue`; `mtl_tx_write` | CORE |
| U-245 | Frame buffer size for a frame duration | `st30_calculate_framebuff_size`, `framebuff_size` | S30, P30 | t | SMP 2, RXTX 2, FF 2, GST 2, RS 1, KT 2 | COVERED `buffer_capacity_bytes`, `rx_unit_samples`, granted in `mtl_session_info`; no pure helper | COMMON |
| U-246 | Audio size/time math | `st30_get_packet_size`, `_sample_size`, `_sample_num`, `_sample_rate`, `_packet_time` | S30 | t | RXTX 2, KT 5, UT 1 | PARTIAL: `mtl_audio_session_query` returns granted sizes; no pure helpers | COMMON |
| U-247 | Per-session pacing engine AUTO / RL / TSC | `pacing_way` (st30, st30p) | S30, P30 | t | RXTX 1, FF 2, PY 7, KT 1 | COVERED `caps.pacing_class` / `hw_pacing` request | COMMON |
| U-248 | RL warm-up accuracy and offset | `rl_accuracy_ns`, `rl_offset_ns` | S30, P30 | | RXTX 1 | PARTIAL: one `MTL_OPT_AUDIO_RL_WARMUP` key for two knobs | RARE |
| U-249 | Pace in the builder too | `ST30_TX_FLAG_BUILD_PACING` | S30 | S! | RXTX 1 | COVERED `audio_build_pacing` | RARE |
| U-250 | Builder-to-pacer FIFO | `fifo_size` (packets) | S30, P30 | | RXTX 1 | COVERED `audio_fifo_ms` (milliseconds) | RARE |
| U-251 | Audio user timestamp (first packet) | `ST30_TX_FLAG_USER_TIMESTAMP` (no st30p flag) | S30 | S! | KT 1, UT 3 | COVERED `media_mode` TAI / INDEX with `sample_count`, carry and absorb rules | COMMON |
| U-252 | Audio user pacing | `ST30(P)_TX_FLAG_USER_PACING` | S30, P30 | | RXTX 1, GST 1, KT 3 | COVERED `media_mode = TAI` | COMMON |
| U-253 | Lost packets read as silence | `ST30(P)_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | S30, P30 | | KT 1, UT 1 | COVERED `rx_incomplete` + `rx_fill = ZERO_MISSING` | RARE |
| U-254 | Audio RX timing parser (dpvr, ipt, tsdf per 200 ms) | `ST30_RX_FLAG_TIMING_PARSER_STAT/META`, `notify_timing_parser_result`, `st30_rx_tp_meta` | S30 | S! | RXTX 1 | PARTIAL: `MTL_OPT_RX_TIMING_PARSER`; dpvr / tsdf not in `mtl_rx_stats` | RARE |
| U-255 | AM824 / AES3 subframe layouts | `struct st31_am824`, `st31_aes3` (+ convert `st31_*`) | S30, CVT | t | KT | NOT COVERED | RARE |
| U-256 | Deprecated sample fields | `sample_size`, `sample_num` | S30 | | none | NOT COVERED | LEGACY-ONLY |
| U-257 | Audio RX arrival time | `timestamp_first_pkt`, `receive_timestamp` | S30, P30 | | RXTX | COVERED `arrival_first_tai_ns[leg]` | COMMON |
| U-258 | Session-level audio TX/RX (pull/push callbacks, fb by index) | `st30_tx_ops.get_next_frame`, `st30_tx_get_framebuffer`, `st30_rx_ops.notify_frame_ready`, `st30_rx_put_framebuff` | S30 | S! | RXTX 4, KT 2 | CHANGED as U-181/U-201 | RARE |

### 2.14 ST 2110-40 ancillary

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-260 | Pipeline ANC TX/RX with packet table + UDW | `st40p_*`, `st40_frame_info`, `struct st40_meta` | P40, S40 | t | SMP 1, RXTX 1, GST 1, KT 3 | COVERED `mtl_anc_session_*`: `submission.anc[]` (`mtl_anc_packet`) + UDW buffer; RX meta area + `anc_count` | COMMON |
| U-261 | ANC packets per frame | `ST40_MAX_META = 20` (excess truncated) | S40 | t | all ANC | CHANGED: `max_packets` (0 = 255) | COMMON |
| U-262 | UDW capacity | `max_udw_buff_size`, `framebuff_size`, `st40p_*_max_udw_buff_size`, `get_udw_buff_addr` | P40, S40 | | SMP 2, RXTX 2, GST 2, KT 2 | COVERED `max_udw_bytes`, buffer views | COMMON |
| U-263 | One ANC packet per RTP packet | `ST40(P)_TX_FLAG_SPLIT_ANC_BY_PKT` | S40, P40 | | SMP 1, GST 2, KT 1 | COVERED `anc_split_by_packet` | COMMON |
| U-264 | RX interlace auto-detect and its off switch | `ST40(P)_RX_FLAG_DISABLE_AUTO_DETECT` | S40, P40 | | GST 1 | COVERED `mtl_anc_config.detect` | COMMON |
| U-265 | Interlaced ANC TX (`second_field`) | `interlaced`, `second_field` | S40, P40 | | RXTX, KT | COVERED `interlaced` + index parity | COMMON |
| U-266 | RX per-port sequence loss/discontinuity, marker seen | `port_seq_lost[]`, `port_seq_discont[]`, `seq_lost`, `seq_discont`, `rtp_marker` | S40, P40 | | KT, GST | PARTIAL: `pkts_received[leg]`, missing ranges, `units_missing_before`; `rtp_marker` and per-port discontinuity flags absent | RARE |
| U-267 | ANC TX test mutations (no marker, seq gap, bad parity, paced) | `st40_tx_test_config` in `st40_tx_ops` / `st40p_tx_ops` | S40, P40 | t | GST (test element), KT | DEBUG `mtl_debug_inject(MTL_FAULT_TX_MUTATE)` | RARE |
| U-268 | RFC 8331 helpers: UDW get/set, checksum, parity, encode/decode, byte swap, sizes | `st40_get_udw`, `st40_set_udw`, `st40_calc_checksum`, `st40_add/check_parity_bits`, `st40_rfc8331_*` | S40 | S! | RXTX 2, GST 1, KT 2, UT 3 | NOT COVERED (the frame path returns decoded packets; RTP passthrough users need these) | COMMON |
| U-269 | Session-level ANC TX/RX frames | `st40_tx_ops.get_next_frame` + `st40_frame`, `st40_rx_ops.notify_frame_ready`, `st40_rx_put_framebuff` | S40 | S! | RXTX 1, KT 2, UT 5 | CHANGED as U-181/U-201 | RARE |
| U-270 | ANC user pacing / timestamp / exact | `ST40(P)_TX_FLAG_USER_PACING/USER_TIMESTAMP/EXACT_USER_PACING` | S40, P40 | | RXTX 1, GST 1, KT 2 | COVERED as U-151…U-153 | RARE |
| U-271 | `ST40P_*_FLAG_FORCE_NUMA` ("NOT SUPPORTED YET", create fails) | `st40_pipeline_api.h:117-121` | P40 | | none | NOT COVERED | LEGACY-ONLY |
| U-272 | `st40p_rx_ops.rtp_ring_size` (documented mandatory, unused) | `st40_pipeline_api.h` | P40 | | none | NOT COVERED | LEGACY-ONLY |

### 2.15 ST 2110-41 fast metadata (no pipeline API exists today)

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-280 | TX one data item per frame | `st41_tx_ops.get_next_frame`, `st41_frame`, `st41_tx_get_framebuffer` | S41 | S! | RXTX 1 | COVERED `mtl_fastmeta_session_create`, `submission.valid_bytes` | RARE |
| U-281 | TX data item type and K bit | `fmd_dit`, `fmd_k_bit` | S41 | S! | RXTX 3 | COVERED `data_item_type`, `k_bit` | RARE |
| U-282 | RX filter on DIT / K | `st41_rx_ops.fmd_dit` (0xffffffff = off), `fmd_k_bit` (0xff = off) | S41 | S! | RXTX 3 | COVERED `MTL_FLOW_MATCH_FMD_DIT/K`, `fmd_dit`, `fmd_k` | RARE |
| U-283 | RX fast metadata (RTP only today) | `st41_rx_ops.notify_rtp_ready` + `st41_rx_get_mbuf` | S41 | S! | RXTX 1 | CHANGED: frame-level RX is new; the RTP path is U-345 | RARE |
| U-284 | Fast metadata user pacing / timestamp | `ST41_TX_FLAG_USER_PACING/USER_TIMESTAMP` | S41 | S! | none | COVERED `media_mode` | RARE |
| U-285 | Fast metadata rate and interlace | `fps`, `interlaced` | S41 | S! | RXTX | COVERED `rate` (0/0 = video's), `interlaced`, `MTL_FASTMETA_FREE_RUNNING` | RARE |
| U-286 | Fast metadata stats, update, queue meta | `st41_*_get/reset_session_stats`, `st41_*_update_*`, `st41_rx_get_queue_meta` | S41 | S! | none | COVERED stats / `update_flows` (queue meta: U-139) | RARE |

### 2.16 RTP passthrough (app-built packets) — COMMON by owner requirement

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-340 | TX app-built RTP video packets, library adds L2–L4 and paces per frame | `ST20_TYPE_RTP_LEVEL`, `st20_tx_get_mbuf/put_mbuf`, `rtp_ring_size`, `rtp_frame_total_pkts`, `rtp_pkt_size`, `notify_rtp_done` | S20 | S! | SMP 1 (`low_level/`), RXTX 1, KT 2, UT 1 | NOT COVERED (`MTL_UNIT_PACKET_CHUNK` reserved, `-MTL_ENOTSUP` in 0.1, D-25; S8 designing) | COMMON |
| U-341 | RX raw RTP video packets (2022-7 de-duplicated) | `st20_rx_get_mbuf/put_mbuf`, `notify_rtp_ready` | S20 | S! | SMP 1, RXTX 1, KT 1 | NOT COVERED | COMMON |
| U-342 | ST22 RTP TX/RX (exact packet count per frame) | `ST22_TYPE_RTP_LEVEL`, `st22_tx/rx_get_mbuf/put_mbuf` | S20 | S! | KT 1 | NOT COVERED | COMMON |
| U-343 | ST30 RTP TX/RX | `ST30_TYPE_RTP_LEVEL`, `st30_tx/rx_get_mbuf/put_mbuf` | S30 | S! | RXTX 1, KT 1 | NOT COVERED | COMMON |
| U-344 | ST40 RTP TX/RX | `ST40_TYPE_RTP_LEVEL`, `st40_tx/rx_get_mbuf/put_mbuf` | S40 | S! | RXTX 1, KT 1 | NOT COVERED | COMMON |
| U-345 | ST41 RTP TX/RX | `ST41_TYPE_RTP_LEVEL`, `st41_tx/rx_get_mbuf/put_mbuf` | S41 | S! | RXTX 1 | NOT COVERED | COMMON |
| U-346 | ST 2022-6 and custom payloads through RTP level | `doc/design.md:326-330` | S20 | S! | external (not in tree) | NOT COVERED | COMMON |
| U-347 | RTP and payload header layouts | `st_rfc3550_rtp_hdr`, `st20_rfc4175_rtp_hdr`, `st20_rfc4175_extra_rtp_hdr`, `ST20_SRD_OFFSET_CONTINUATION`, `ST20_SECOND_FIELD`, `ST20_RETRANSMIT`, `st22_rfc9134_rtp_hdr`, `st40_rfc8331_rtp_hdr`, `st40_rfc8331_payload_hdr(_common)`, `st41_rtp_hdr` | St, S20, S40, S41 | S! | SMP, RXTX, KT, GST | NOT COVERED | COMMON |
| U-348 | Pixel-group bit layouts | `st20_rfc4175_422_10_pg2_be` … `st20_rfc4175_444_12_pg2_le` | S20 | t | SMP (perf), KT | NOT COVERED | RARE |
| U-349 | Packet size limits | `MTL_PKT_MAX_RTP_BYTES`, `MTL_UDP_MAX_BYTES`, `MTL_MTU_MAX_BYTES` | C | | RXTX, KT | NOT COVERED (`MTL_ENOSPC`) | RARE |

### 2.17 Pipeline conversion, formats and frame helpers

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-360 | Convert app layout ↔ transport format in the library | st20p `input_fmt` / `output_fmt` vs `transport_fmt` | P | | FF, GST, OBS, RXTX, PY, RS | COVERED `app_format` + `transport_format`; `convert_context`, `converter` reported | CORE |
| U-361 | App frame formats | `enum st_frame_fmt` | P | | many | PARTIAL: `mtl_app_format` lacks YUV422 planar 12LE and 16LE, YUV444 planar 12LE, GBR planar 12LE, ARGB; adds I420, NV12, RGBA; RFC 4175 PG formats = `MTL_APP_SAME_AS_TRANSPORT` | COMMON |
| U-362 | Non-compliant 8-bit 4:2:0 / 4:2:2 passthrough (I420/YUY2 bytes on the wire) | `ST_FRAME_FMT_YUV420CUSTOM8`, `ST_FRAME_FMT_YUV422CUSTOM8` | P | | FF 2, GST 1, OBS 1, RXTX 1, RS 1, SMP 1 | NOT COVERED (`MTL_APP_I420`/`NV12` convert to RFC 4175, so the wire differs) | COMMON |
| U-363 | Zero-copy (derive) pipelines when formats match | implicit | P | | GST, KT | COVERED `pool.data_path` DIRECT, `MTL_PATH_KIND_DIRECT` | COMMON |
| U-364 | Choose the converter device for st20p | `st20p_*_ops.device` (`enum st_plugin_device`) | P | | RXTX, KT | NOT COVERED for video (`plugin_device` exists only in `mtl_cvideo_config`) | RARE |
| U-365 | Frame struct: planes, strides, size, format, timestamps, status | `struct st_frame` | P | | all pipeline users | COVERED `mtl_buffer_view` + `mtl_rx_unit` / `mtl_tx_result` | CORE |
| U-366 | Buffer requirements before create | `st_frame_size`, `*_frame_size` after create | P | | SMP 8, KT 4 | COVERED `mtl_video_session_query`, `mtl_video_layout_query`, `mtl_buffer_requirements` | COMMON |
| U-367 | Standalone frame allocation | `st_frame_create/free`, `st_frame_create_by_malloc` | P | | KT 1, PY 2 | PARTIAL: `mtl_mem_alloc` + `mtl_buffer_create` | RARE |
| U-368 | Format helpers: names, planes, linesizes, transport mapping | `st_frame_fmt_*`, `st_frame_name_to_fmt`, `st_frame_least_linesize`, `st_frame_plane_size`, `st_frame_data_height`, `st_frame_sanity_check` | P | | SMP, FF, GST, OBS, PLG 3, PY, KT 4 | PARTIAL: `mtl_video_layout_query`, `mtl_video_format_enum`, `mtl_video_format_names` | COMMON |
| U-369 | Software frame operations | `st_frame_convert`, `st_frame_downsample`, `st_draw_logo`, `st_field_merge`, `st_field_split` | P | | SMP 4, PY 1, KT 1 | NOT COVERED | RARE |
| U-370 | Codec name ↔ enum | `st_name_to_codec`, `enum st22_codec` | P | | SMP 1, FF 2, PY 1 | PARTIAL: `enum mtl_codec`; no name helper | RARE |
| U-371 | SWIG-friendly helpers | `st_frame_addr_cpuva`, `st_frame_addr`, `st_rxp/st_txp_para_*` | P | | PY 8 | CHANGED: POD structs, `mtl_flow_parse`, `MTL_ADDR(T)` for bindings | COMMON |

### 2.18 Codec and converter plugins

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-380 | Register an ST22 encoder / decoder in-process | `st22_encoder_register/unregister`, `st22_decoder_register/unregister`, `st22_encoder_dev`, `st22_decoder_dev`, `*_create_req` | P | | PLG 2, KT 1 | NOT COVERED (plugin ABI frozen as legacy, Q-MODE-4) | COMMON |
| U-381 | Encoder/decoder worker loop | `st22_encoder/decoder_get_frame`, `put_frame`, `wake_block`, `set_block_timeout`, `*_RESP_FLAG_BLOCK_GET`, `st22_encode/decode_frame_meta` | P | | PLG 2, KT 1 | NOT COVERED | COMMON |
| U-382 | Register a st20p format converter | `st20_converter_register/unregister/get_frame/put_frame`, `st20_converter_dev` | P | | PLG 1, KT 1 | NOT COVERED | RARE |
| U-383 | Load codec plugins from shared objects (incl. JSON list at init) | `st_plugin_register/unregister`, `st_get_plugins_nb`, `ST_PLUGIN_*_API` entry points, `st_plugin_meta`, `ST_PLUGIN_VERSION_V1_MAGIC` | P | | KT 2; `plugins/st22_avcodec`, `plugins/sample` implement it | NOT COVERED | COMMON |
| U-384 | Plugin format capability masks | `ST_FMT_CAP_*` | P | | PLG | NOT COVERED | COMMON |
| U-385 | Test-only plugin devices | `ST_PLUGIN_DEVICE_TEST`, `_TEST_INTERNAL` | P | | KT | NOT COVERED | RARE |

### 2.19 Time, rate and conversion helpers

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-390 | Frame rate enum ↔ number ↔ name | `st_frame_rate`, `st_frame_rate_to_st_fps`, `st_name_to_fps` | St | t | RXTX 11, KT 18, FF 1, GST 3, PY 1 | COVERED `mtl_fps_rational`, `mtl_fps_parse` | CORE |
| U-391 | TAI ↔ media clock (RTP) conversion and unwrap | `st10_tai_to_media_clk`, `st10_media_clk_to_ns`, `st10_media_clk_to_tai`, `st10_get_tai`, `st10_get_media_clk`, `enum st10_timestamp_fmt` | St | t | RXTX 4, KT 6, UT 8 | PARTIAL: results carry TAI and RTP; `mtl_rational_index_at`, `mtl_timeline_index_at`; no TAI ↔ RTP helper | COMMON |
| U-392 | Sampling-rate constants | `ST10_VIDEO_SAMPLING_RATE_90K`, `ST10_AUDIO_SAMPLING_RATE_*` | St | t | RXTX, KT | COVERED implicitly (Hz numbers) | RARE |
| U-393 | Standalone colour conversion library (RFC 4175 ↔ planar / V210 / Y210 / LE, AM824 ↔ AES3; SIMD and DMA variants) | `st_convert_api.h` (47), `st_convert_internal.h` (58) | CVT | | SMP (perf, tools), PLG 1, PY, KT (70 functions in `cvt_test.cpp`) | NOT COVERED (conversion is a session property; library split is Q-MODE-4 "also") | COMMON |

### 2.20 Stats and observability

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-400 | Session stats per port and per family | `st*_get_session_stats`, `st_tx/rx_user_stats`, `st_tx/rx_port_stats`, `st20/30/40/41_*_user_stats` | St, S*, P* | t | RXTX 1, KT 3, UT 3, ext NUDA9A | COVERED `mtl_session_get_stats` (one schema, legs, media tail; ST22 gains stats) | COMMON |
| U-401 | Reset session stats | `st*_reset_session_stats` | S*, P* | | RXTX 1, UT 2 | CHANGED: cumulative only (D-20) | COMMON |
| U-402 | Family-specific diagnostic counters (no slot, wrong interlace, burst sizes, …) | `st20_rx_user_stats` (~35 fields) etc. | S20–S41 | t | RXTX, KT | PARTIAL: reject slots, reason slots, key-addressed `mtl_session_stat_get` + `mtl_stat_list` | RARE |

### 2.21 Experimental and legacy remnants

| ID | Use case | Legacy location | Hdr | Sess | Consumers | Unified coverage | Class |
|---|---|---|---|---|---|---|---|
| U-410 | Redundant-combined ST20 RX (two flows merged) | `st20rc_rx_*`, `ST20RC_RX_FLAG_*` | EXP | | SMP 1, RXTX 1 | COVERED by two-leg sessions | LEGACY-ONLY |
| U-411 | Deprecated `sip_addr` alias of RX `ip_addr` | `st*_rx_ops`, `st_rx_port`, `st_rx_source_info` | St, S*, P | | none (deprecated) | NOT COVERED | LEGACY-ONLY |
| U-412 | Reserved RX `pacing` / `packing` fields | `st20_rx_ops.pacing`, `.packing` ("not in use") | S20 | | none | NOT COVERED | LEGACY-ONLY |
| U-413 | ST22 slice packetization | `ST22_PACK_SLICE` ("not support now") | S20 | | none | COVERED as `-MTL_ENOTSUP` | LEGACY-ONLY |
| U-414 | Legacy TX session callback guard pattern (`if (!ctx->handle)`) | callbacks may fire before create returns | all | | KT 37 sites, OBS | CHANGED: no callbacks, handle returned before start | — (not counted) |
| U-415 | `MTL_FLAG_BIND_NUMA` set by apps (no effect) | `mtl_api.h:340` | C | | SMP 2, FF, OBS 2, PY 7, RS 6 | see U-062 | LEGACY-ONLY |
| U-416 | Plain `mtl_udma_fill` (u64 pattern) | `mtl_udma_fill` | C | | none | NOT COVERED (U-116) | LEGACY-ONLY |
| U-417 | Session `gpu_direct_framebuffer_in_vram_device_address` raw field | `st20_rx_ops` | S20 | | none | NOT COVERED | LEGACY-ONLY |
| U-418 | Pipeline RX/TX fb address getters with no users | `st20p_rx_get_fb_addr`, `st22p_rx_get_fb_addr`, `st22_rx_get_fb_addr`, `st40p_tx/rx_get_udw_buff_addr`, `st40p_tx_get_fb_addr` | P, P40, S20 | | none | COVERED by `mtl_session_get_buffers` anyway | RARE |

## 3. Use cases reachable only through session-level headers

The owner wants `st20_api.h`, `st30_api.h`, `st40_api.h`, `st41_api.h` and `st_api.h` hidden. Each row below needs a home in the unified headers before that can happen. "Home today" is the sketch coverage from §2.

### 3.1 Capabilities with no pipeline equivalent (`S!`)

| ID | Capability | Home today |
|---|---|---|
| U-175 | TX done by frame index | COVERED (`MTL_CQE_TX_RESULT`) |
| U-176 | Zero-hop callbacks on the tasklet | PARTIAL (`WAKE_DIRECT`; inline notify later) |
| U-181 | TX pull model (`get_next_frame`) | CHANGED (push) |
| U-182 | TX framebuffers by index | COVERED |
| U-184 | TX app-owned frames per index | COVERED (attached pool) |
| U-191 | Slice TX | PARTIAL (Phase 6) |
| U-201 | RX push callback + put | CHANGED (dequeue) |
| U-218 | Slice RX | PARTIAL (Phase 6) |
| U-219 | Per-pixel-group RX callback | NOT COVERED (candidate cut) |
| U-236 | ST22 session TX/RX | CHANGED |
| U-249 | Audio build pacing | COVERED |
| U-251 | Audio user timestamp | COVERED |
| U-254 | Audio RX timing parser | PARTIAL |
| U-258 | Audio session TX/RX | CHANGED |
| U-268 | RFC 8331 helpers | NOT COVERED |
| U-269 | ANC session TX/RX | CHANGED |
| U-280 … U-286 | All of ST 2110-41 (7 rows; no pipeline exists) | COVERED except RTP RX (U-283 → U-345) |
| U-340 … U-347 | All RTP passthrough (8 rows) | NOT COVERED |

### 3.2 Types and helpers that pipeline users need from session-level headers (`t`)

- Instance and scheduling: U-033 (`st_var_info`), U-070 (`*_get_sch_idx` of st20/st22).
- Memory and flows: U-111 (`st20_ext_frame` IOVA), U-135 / U-136 (`st_tx_dest_info`, `st_rx_source_info`), U-139 (`st_queue_meta`), U-140 (`st_pcap_dump_meta`), U-141 (`st_tx/rx_rtcp_ops`).
- Events and timing: U-144 and U-177 (`enum st_event`), U-159 (`st10_vsync_meta`), U-160 (`enum st21_pacing`), U-390 … U-392 (fps and media-clock helpers, `enum st10_timestamp_fmt`).
- Video: U-192 (`enum st20_packing`), U-193 (`enum st20_fmt`), U-195 (`enum st_fps`), U-196 / U-197 (st20 size and name helpers), U-203 (`enum st_frame_status`), U-212 (`st20_detect_meta/reply`), U-215 / U-216 (`st20_rx_tp_meta`, `st20_rx_tp_pass`), U-235 (`enum st22_pack_type`), U-348 (pixel groups).
- Audio: U-240 … U-247 (`enum st30_fmt/sampling/ptime`, `st30_*` helpers), U-255 (AM824/AES3).
- ANC: U-260 / U-261 (`st40_meta`, `ST40_MAX_META`), U-267 (`st40_tx_test_config`).
- Stats: U-400 / U-402 (`st_*_user_stats`, `st*_user_stats`).

Hiding the session headers without replacing these breaks every st20p/st22p/st30p/st40p consumer at compile time, because the pipeline ops structs and frame structs embed these types.

## 4. NOT COVERED by the sketch header

93 rows. Grouped; each needs either a unified home or an approved cut.

| Group | IDs |
|---|---|
| RTP passthrough (owner: must stay) | U-340, U-341, U-342, U-343, U-344, U-345, U-346, U-347, U-348, U-349 |
| Instance / port tuning only in `mtl_init_params` | U-014, U-016, U-024, U-025, U-026, U-027, U-028, U-029, U-030, U-031, U-057, U-058, U-098 |
| Instance flags with no unified flag | U-041, U-042, U-043, U-044, U-059, U-060, U-063, U-064, U-065, U-076, U-077, U-078, U-079, U-080, U-081, U-082, U-083, U-084, U-085, U-086 |
| Logging and runtime control | U-018, U-019, U-020, U-021, U-052, U-053 |
| Threads, lcores, user tasklets, helpers | U-066, U-067, U-068, U-069, U-071, U-072, U-073 |
| Memory and DMA | U-111, U-116, U-117 |
| Session features deferred or dropped | U-139, U-140, U-141, U-142, U-143, U-194, U-213, U-215, U-216, U-219, U-220, U-234, U-255, U-256, U-271, U-272 |
| Pipeline formats and helpers | U-362, U-364, U-369 |
| Plugin ABI and convert library | U-380, U-381, U-382, U-383, U-384, U-385, U-393 |
| ANC helpers | U-268 |
| Legacy remnants | U-032, U-089, U-101, U-411, U-412, U-416, U-417 |

Inconsistencies inside the sketch found while checking (not counted as rows):

- `MTL_EQ_SUB_EPOCH_TICK` says "of sessions that enable it", but no session flag or option enables EPOCH_TICK (U-159).
- `MTL_INSTANCE_BIND_NUMA` under the zero-default rule inverts today's default (bound unless `NOT_BIND_NUMA`) (U-062).
- 03 §7.2 says `pkt_udp_suggest_max_size` becomes per-session, but no session field exists (U-029).
- `MTL_OPT_VIDEO_DISABLE_BULK` has no cvideo counterpart although `ST22(P)_TX_FLAG_DISABLE_BULK` exists (U-162).
- Time source is set twice: `time_source = PTP_BUILTIN` and flag `MTL_INSTANCE_PTP_BUILTIN` (U-040).

## 5. Unified sketch features with no legacy counterpart (new)

Fine to keep; listed so simplifiers know they are not migration obligations.

| Area | New features |
|---|---|
| Lifecycle | explicit session states (ARMED … RETIRED), start now / at TAI / at media index with `preroll_ns`, stop DRAIN / FLUSH, `mtl_session_discard_queued`, `mtl_session_reconfigure`, deferred destroy, `mtl_session_set_leg_enabled`, `mtl_instance_list_sessions` |
| Timing | media modes AUTO / INDEX / TAI, source kinds, named timelines with anchors and step policy, groups (TX and RX) with one link offset, late policies SEND_LATE / RESLOT, underrun SILENCE / EMPTY_ANC / KEEPALIVE, snap modes and off-grid policy, tsmode, `link_offset_budget_ns`, horizon, RX `media_index`, presentation time, `mtl_rx_align`, `mtl_session_row_deadline`, PsF |
| Completion | CQ and EQ objects (private or shared), completion modes NONE / EXCEPTIONS / ALL, `MTL_CQE_RX_MISSING` ranges, `TX_SOURCE_RELEASED`, TX timing record (deadline, scheduled, enqueued, HW observed, margin), `mtl_cq_dispatch_start`, `mtl_eq_post` USER events, OVERFLOW accounting |
| Waiting | portable wait objects (eventfd / Windows HANDLE), multi-target `mtl_session_wait`, sticky interrupt per session / CQ / EQ / instance, `trywait` |
| Errors | `MTL_E*` codes, `mtl_last_error` with field detail, frozen `enum mtl_state_reason`, call classes enforced in debug builds, `-MTL_EDEADLK` from busy loops |
| ABI | `struct_size` versioning, `*_init()` and value macros, typed 64-bit handles with generations, `mtl_struct_known_size`, `mtl_version_num`, size checks, binding macros |
| Memory | regions (import anonymous, shmem, memfd, hugetlbfs, GPU-pinned host), access and NUMA checks, buffers and leases, `mtl_buffer_hold`, `mtl_rx_transfer` and `submission.hold` (1 → N zero-copy forward), `mtl_tx_withdraw`, exported pools, RX overflow RECLAIM_OLDEST_READY, slot select BY_INDEX, fill policy, dynamic pool, `mtl_lease_copy_in/out`, `mtl_instance_get_mem_status` |
| Discovery | `mtl_port_get_caps`, `get_status`, `get_capacity`, `mtl_sched_get_status`, dry-run `mtl_*_session_query` with `MTL_QUERY_CHECK_CAPACITY`, `mtl_buffer_requirements`, granted values in `mtl_session_info` (SDP values, `ts_refclk`) |
| Network | DSCP, TTL, IPv6 family field, VLAN reserved, `mtl_flow_parse`, per-leg flow state (WAITING_NEIGHBOUR, JOINED), atomic `update_flows` with activation time |
| Media | cvideo CBR padding, audio carry / absorb / DISCONTINUITY, `mtl_tx_write` stream write, ANC timing model CTM / LLTM, ANC window anchor, `total_lines`, fastmeta free-running rate and keep-alive, ST41 frame RX |
| Time | `mtl_time` record with clock / flags / accuracy, time status (lock state, grandmaster, offset), `mtl_time_convert`, `mtl_time_cross_timestamp`, validated `CLOCK_TAI`, labelled `SYSTEM_TAI` estimate, `ptp_domain` |
| Stats | one schema for all essences, histograms (log2 / linear), windowed maxima, queue gauges, `unsupported_mask`, key-addressed stats and options with enumeration |
| Test | null backend `null:<n>`, test time source, `mtl_debug_inject` fault matrix |
| Simple layer | `mtl_simple.h` (video only), `mtl_instance_open_simple` |
| Interop | `mtl_instance_from_legacy` / `to_legacy` bridge |

## 6. Candidate cuts (LEGACY-ONLY) — each NEEDS APPROVAL

| ID | Candidate | Evidence | If cut |
|---|---|---|---|
| U-032 | `tx/rx_sessions_cnt_max` | marked deprecated in `mtl_api.h`; only the MXL POC sets them | drop |
| U-072 | SysV shm lcore manager API | `doc/design.md:51` advises against; only one sample tool calls it; MtlManager supersedes | keep as tool, not API |
| U-073 | `mtl_sleep_us`, `mtl_delay_us` | 1 / 0 consumers; libc equivalents | drop |
| U-089 | `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | UDP stack deleted in `2b182cd87`; `sample_util.c:276` still sets the flag (no effect) | drop |
| U-095 | DPDK AF_XDP / AF_PACKET PMDs | experimental; native AF_XDP and kernel socket cover them (07 §9); still in `enum mtl_backend` | drop from enum or keep experimental |
| U-101 | `mtl_get_if_ip` | 0 consumers | drop |
| U-139 | DATA_PATH_ONLY + `get_queue_meta` | 0 users of the flag; suspected NULL dereference (`mt_queue.c:56`, SP-02) | verify, then cut or redesign |
| U-142 | RTCP flags on ST30/40/41/st40p | no consumer in `lib/` (R07 §3.1 note) | drop |
| U-194 | Non-RFC 4175 transport formats (planar 10LE, V210) | non-compliant on the wire; RXTX and one KT case | drop or keep as RARE |
| U-213 | Header split | needs a DPDK patch absent for the pinned 26.07; single port, BPM only | drop for v1 |
| U-219 | `uframe_pg_callback` | app code on the tasklet; RXTX 1, KT 3 | drop |
| U-220 | `ST20P_RX_FLAG_PKT_CONVERT` | tasklet conversion, three formats; KT 1 | drop |
| U-256 | `sample_size`, `sample_num` | "Not use anymore, plan to remove" | drop |
| U-271 | `ST40P_*_FLAG_FORCE_NUMA` | "NOT SUPPORTED YET", create fails | drop |
| U-272 | `st40p_rx_ops.rtp_ring_size` | documented mandatory, unused | drop |
| U-410 | `st20rc` combined RX | superseded by two-leg sessions; one sample, one RxTxApp mode | drop |
| U-411 | `sip_addr` alias | deprecated | drop |
| U-412 | `st20_rx_ops.pacing/packing` | "Not in use now" | drop |
| U-413 | `ST22_PACK_SLICE` | never implemented | keep as `-ENOTSUP` value |
| U-415–U-417 | `MTL_FLAG_BIND_NUMA` (no effect), `mtl_udma_fill`, raw GPU session field | never read / 0 consumers | drop |

Cuts that are **not** LEGACY-ONLY but are implied by the sketch and also need approval, because consumers exist:

- U-362 CUSTOM8 passthrough (FF, GST, OBS, RXTX, RS); U-380…U-385 the plugin ABI (st22_avcodec, JPEG-XS); U-393 the convert library (KT, Python, perf tools).
- U-067 lcore borrowing (14 samples); U-031 DMA device list (6 consumer trees); U-019 log printer.
- Wire-visible default changes: U-155 the legacy default ST20 RTP value (every legacy sender), U-202 incomplete-frame delivery now on by default, U-233 ST22 CBR instead of today's VBR-like behaviour.

## 7. Simplification guard-rails

### 7.1 A lean core header MUST support these directly

1. Instance open/release, shared default instance, port spec (name, IP, prefix, gateway, queue counts) — U-001…U-011, U-015, U-017.
2. Video, compressed video, audio, ANC and fast metadata TX and RX at frame/field granularity with library pools — U-180, U-200, U-230…U-232, U-244, U-260, U-280.
3. Flows with IP, UDP port, PT, SSRC, source filter, user MAC, two legs, and atomic update — U-122…U-130, U-135, U-136.
4. Blocking with timeouts, interrupt, non-blocking poll, a wait object — U-170…U-173.
5. Frame status, packets per leg, arrival and media times on RX; result status on TX — U-174, U-203…U-206.
6. Transport format + app format conversion, stride, interlace, fps — U-187, U-188, U-193, U-195, U-360, U-365.
7. Timing basics: library epoch pacing, TAI user pacing, user RTP, drop-when-late, late reporting — U-150, U-151, U-153, U-157, U-158.
8. App memory import (TX and RX) with a two-phase release — U-112, U-184…U-186, U-208.
9. Stats and errors — U-145, U-400.
10. **RTP passthrough** for every essence (owner requirement) — U-340…U-347; at minimum a reserved unit kind plus a packet-buffer shape that a later revision fills without an ABI break.
11. Every `t` type in §3.2 that a pipeline ops struct embeds, or its unified replacement, so hiding session headers does not break existing pipeline users.

### 7.2 Can move to optional headers, keyed options or helpers without loss

| Destination | Use cases |
|---|---|
| keyed instance/port options (enumerable, like `MTL_OPT_*`) | U-014, U-016, U-023…U-030, U-041…U-044, U-052, U-053, U-057…U-060, U-063…U-065, U-076…U-086, U-088 port default |
| keyed session options (already the pattern) | U-143, U-162, U-248, U-364 |
| optional extension header (`next` blocks or a second header) | U-141 RTCP, U-140 pcap capture, U-215/U-216 timing parser detail, U-212 video auto-detect, U-117 device memory, U-191/U-218 rows |
| helper header (pure functions, no instance) | U-196, U-197, U-246, U-268, U-347, U-348, U-368, U-369, U-370, U-391, U-115 memcpy |
| separate plugin header / library | U-380…U-385, U-393 |
| debug header (`mtl_debug.h`, already) | U-090, U-223, U-267 |
| admin tool, not API | U-072, U-066 |
| logging header | U-018…U-021 |
| platform header (threads, lcores, tasklets, DMA engines) | U-031 DMA device list, U-067…U-069, U-071, U-116 |

### 7.3 Rules any simplification should check against this file

- A row may change from COVERED to "helper" or "keyed option" freely; it may not become NOT COVERED without an approval entry.
- A default that changes wire behaviour (U-155, U-202, U-233) must be listed as CHANGED, with the legacy-compatible setting named.
- Merging two knobs into one (for example `time_source` and `MTL_INSTANCE_PTP_BUILTIN`, or the two RL warm-up values in U-248) must keep every legacy value reachable.
- Before hiding any session-level header, every §3.1 row needs a home and every §3.2 type needs a replacement.
