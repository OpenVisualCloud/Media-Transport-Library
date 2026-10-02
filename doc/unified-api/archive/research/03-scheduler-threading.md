# Research note 03: scheduler, tasklets, threading and execution contexts

| Field | Value |
|---|---|
| Topic | Which thread runs what; which public calls block, lock or allocate; how the app and the tasklets hand work to each other; constraints for a unified session API |
| Revision | 1 |
| Date | 2026-09-29 |
| Baseline | `main` @ `545a266a`, read from a clean `git archive HEAD` export. Line numbers are for HEAD. The working tree at the time had uncommitted edits to `lib/src/mt_sch.c`, `mt_main.[ch]`, `st_tx_video_session.c` and others (for example the `MPOL_LOCAL` mempolicy code from `189e1e91` is removed there). None of those edits change the conclusions below. |
| Scope | `include/*.h`, `lib/src/mt_sch.[ch]`, `lib/src/mt_main.h`, `mt_admin.c`, `mt_cni.c`, `mt_ptp.c`, `mt_stat.c`, `mt_log.[ch]`, `mt_arp.c`, `mt_handle_guard.h`, `datapath/*`, `st2110/*` sessions and transmitters, `st2110/pipeline/*`, `manager/`, `plugins/sample`, `app/sample`, `doc/design.md`, `doc/shm_lcore.md`, KB §2/§4/§6 |
| Hard requirement under test | Nothing called from the public API may run inside the tasklets or block the pinned cores. |

Labels: **[verified]** means I read it in code at the cited `path:line`. **[inferred]** means it follows from verified code plus DPDK, glibc or kernel behaviour I did not re-read this session. **[unknown]** means I could not establish it.

---

## 0. TL;DR

1. **Almost every user callback runs on a pinned tasklet today, and it runs while the session spinlock is held.** The builder and transmitter tasklets hold `mgr->mutex[idx]` (an `rte_spinlock`, taken by `try_get`) around the whole per-session body. That body is where `get_next_frame`, `notify_frame_done`, `notify_event`, `query_ext_frame` and the rest are called.
   **[verified]** `st_tx_video_session.c:2682`, `st_video_transmitter.c:674`, `st_rx_video_session.c:3516`.
2. **A callback that calls a same-session stats API deadlocks the core.** `st20_tx_get_session_stats` and `st20p_tx_get_session_stats` do a blocking `rte_spinlock_lock` on that same spinlock, and `rte_spinlock` is not recursive. Calling either from inside `notify_frame_done` spins forever on the pinned lcore.
   **[verified path]** `st_tx_video_session.c:4760`, `st20_pipeline_tx.c:1311`. **[inferred outcome]**
3. **The pipelines' blocking `get_frame` is woken by the tasklet calling `pthread_mutex_lock` + `pthread_cond_signal`.** This happens on every frame-done or frame-ready. When a waiter exists this is a `FUTEX_WAKE` syscall on the pinned core. When the app thread holds `block_wake_mutex` (it does, while it scans framebuffers), the tasklet can go to sleep in `FUTEX_WAIT`.
   **[verified]** `st20_pipeline_tx.c:29-45`, `:774-789`, `st20_pipeline_rx.c:29-46`. **[inferred cost]** The same pattern exists in every `st*p` pipeline, and the shipped samples do the same thing inside their callbacks (`app/sample/fwd/rx_st20p_tx_st20p_downsample_fwd.c:29-30`).
4. **Control-plane work already runs on pinned tasklets in several places:**
   - TX-hang recovery: `mt_txq_put`/`mt_txq_get` (pthread mutex, RL shaper set-up), mempool free/create, busy flush, INFO logging. `st_video_transmitter.c:681` → `st_tx_video_session.c:4231-4331`. The audio version also takes blocking session spinlocks: `st_tx_audio_session.c:2742`.
   - RX auto-detect: `rv_init_sw` allocates frames, and the pipeline's `notify_detected` calls `mt_rte_zmalloc_socket`. `st_rx_video_session.c:2839`, `st20_pipeline_rx.c:377`.
   - pcap dump: `writev`. `st_rx_video_session.c:2895`.
   - PTP: `rte_eal_alarm_set` (malloc + timerfd). `mt_ptp.c:928,968,1023`.
   - Logging in general: `localtime_r` + stdio or the user's printer. `mt_log.h:24-39`.

   All **[verified]**.
5. **Online update calls hold the session spinlock while doing blocking work.**
   - `st20_tx_update_destination` holds it across `mt_dst_ip_mac` → `arp_get_result`, which sleeps 500 ms per retry for up to `arp_timeout_ms` (default 60 s). `st_tx_video_session.c:3848-3855`, `:926`, `mt_arp.c:171-199`, `mt_main.c:576`.
   - `st20_rx_update_source` holds it across flow and queue teardown/re-create and the multicast join. `st_rx_video_session.c:3932-3990`.

   The tasklets use `try_get`, so they skip the session instead of spinning. The session goes silent for the duration. Blocking `get` users do spin: the admin thread, and the audio fatal-error path on a tasklet. **[verified]**
6. **Callbacks do not have a stable thread identity.**
   - TX `notify_frame_done` can run on the transmitter tasklet, the builder tasklet, the app thread, or a plugin converter thread.
   - Session migration (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`) moves a session to another lcore at runtime.
   - With a shared TX queue or kernel-socket TX, the extbuf free callback can fire on another session's tasklet or on a socket pthread.

   **[verified]** for the first two points (section 2). **[inferred]** for the last.
7. **The scheduler itself is sound for a no-blocking design.** It is a single loop per lcore that sums handler returns, sleeps optionally, and registers/unregisters under a pthread mutex with an ack handshake. The new API can keep it unchanged, provided the per-frame user code and wake-ups move out of it. **[verified]** `mt_sch.c:155-242`, `:873-966`.

Hard constraints the note derives (section 7.1):
- No user code runs on a pinned tasklet by default.
- No syscall and no pthread primitive on the tasklet side of any app↔lib handoff.
- The session spinlock never guards app-visible state.
- Every fast-path public call is lock-free and non-allocating.
- Every wait lives only on the app side, with an armed-waiter protocol.

---

## 1. Execution contexts that exist today

```text
            +-------------------- process --------------------------------------------------+
 app thr ---|-> public API (control: create/free/update; fast: get/put/*_mbuf; wait: BLOCK_GET)|
            |                                                                                |
 pinned     |  EAL lcore k:  mtl_sch_k loop  -> tasklets[]: tx_video_mgr(builder),          |
 lcores     |                                   video_trs(transmitter), tx_audio_mgr, a_trs, |
 (default)  |                                   rx_video_mgr, rx_audio, anc, fmd, cni?, ptp?, |
            |                                   srss?, user tasklets (mtl_sch_api)             |
            |  EAL lcore m:  rv_pkt_lcore_func (ST20_RX_FLAG_USE_MULTI_THREADS only)           |
            |  EAL lcore t:  tap_bkg_thread (TAP only)                                          |
 lib        |  mtl_admin (6 s)  stat thread  cni thread?  srss thread?  socket tx/rx x4?       |
 pthreads   |  tsc calibration (init)   EAL interrupt/alarm thread (rte_eal_alarm_set handlers) |
 plugin thr |  st22 encoder/decoder and converter threads, created by the plugin              |
            +--------------------------------------------------------------------------------+
```

| ID | Context | Created at | Pinned? | Notes |
|---|---|---|---|---|
| T-LC | Scheduler on an EAL lcore (default) | `rte_eal_remote_launch(sch_tasklet_lcore)` `mt_sch.c:285` | yes | lcore comes from the manager or the shm allocator, `mt_sch.c:701-791` **[verified]** |
| T-PT | Scheduler as a plain pthread (`MTL_FLAG_TASKLET_THREAD`) | `pthread_create` `mt_sch.c:287` | no, inherits the caller's affinity **[inferred]** | unregistered non-EAL thread, so no mempool cache (KB §2) |
| X-LC | Extra busy lcores: RX video packet lcore, TAP lcore | `st_rx_video_session.c:2507-2517`, `mt_tap.c:903-911` | yes | the RX packet lcore runs `rv_handle_frame_pkt`, which fires user callbacks `st_rx_video_session.c:2470-2479` **[verified]** |
| ADM | `mtl_admin` pthread, woken every 6 s | `mt_admin.c:379-388` | no | computes CPU-busy and migration under blocking session spinlocks **[verified]** |
| STAT | stat pthread (falls back to the alarm thread) | `mt_stat.c:63-97` | no | runs `stat_dump_cb_fn` **[verified]** |
| CNI-TH | CNI pthread (`MTL_FLAG_CNI_THREAD`, or auto when PTP is off) | `mt_cni.c:391-420`, `:565-580` | no | otherwise the CNI runs as a tasklet on `main_sch` **[verified]** |
| SRSS | shared-RSS pthread | `datapath/mt_shared_rss.c:141-167` | no | also registered as a tasklet `:495` **[verified]** |
| SOCK | kernel-socket TX/RX pthreads | `datapath/mt_dp_socket.c:288,627` | no | frees mbufs after `sendto` `:200` **[verified]** |
| INT | DPDK EAL interrupt/alarm thread | every `rte_eal_alarm_set` | no | PTP delay-req/monitor, ARP, IGMP, DHCP, and the admin/stat/sch-sleep wakers **[verified]** |
| PLG | plugin-owned threads | e.g. `plugins/sample/st22_plugin_sample.c:82` | plugin's choice | call `st22_encoder_get_frame` / `put_frame` into the pipeline |
| APP | application threads | app | app's choice | the only context the public API is documented for |

The pipelines have **no internal threads** of their own. `lib/src/st2110/pipeline/` contains no `pthread_create`. Conversion runs in one of three places: inline in the app thread (`internal_converter`, `st20_pipeline_tx.c:869`, `st20_pipeline_rx.c:878`), in plugin threads, or per packet on the tasklet (`ST20P_RX_FLAG_PKT_CONVERT` via `uframe_pg_callback`, `st20_pipeline_rx.c:535`). **[verified]**

`main_sch` is requested with quota 0 (`dev/mt_dev.c:1954`), and quota 0 fits any scheduler type (`mt_sch.c:433`). So unless `MTL_FLAG_DEDICATED_SYS_LCORE` is set, the CNI and PTP tasklets share lcore 0 of the media schedulers with video sessions. **[verified]**

---

## 2. Callback → execution-context map

"Tasklet" means T-LC or T-PT. "Holds S" means the session spinlock `mgr->mutex[idx]` is held across the call. Declaration lines are in `include/`, call sites in `lib/src/`.

### 2.1 Session (transport) layer

| Callback | Declared | Call site(s) | Context | Holds S |
|---|---|---|---|---|
| st20/st22 tx `get_next_frame` | `st20_api.h:1186`, `:1346` | `st2110/st_tx_video_session.c:1922` (`tv_tasklet_frame`), `:2444` (`tv_tasklet_st22`) | builder tasklet. It is called on every loop iteration until the app returns a frame. | yes (`:2682`) |
| st20 tx `query_frame_lines_ready` | `st20_api.h:1254` | `st_tx_video_session.c:2021` | builder tasklet | yes |
| st20/st22 tx `notify_frame_done` | `st20_api.h:1195`, `:1355` | `tv_notify_frame_done` `:93-114` ← extbuf free cb `tv_frame_free_cb` `:116`: runs where the last chain mbuf is freed (TX completion; builder `:2133`/`:2658`; cleanup `:3052`; recovery `:4298`) | transmitter/builder tasklet; app at free; **[inferred]** other session's tasklet (TSQ) or SOCK thread | yes (tasklet) |
| st20/st22 tx `notify_frame_late` | `st20_api.h:1204`, `:1364` | `st_tx_video_session.c:683` | builder tasklet | yes |
| st20/st22 tx `notify_event` | `st20_api.h:1211`, `:1372` | VSYNC `:310`. RECOVERY/FATAL `:4240-4329`, reached via `st_video_transmitter.c:681` | builder tasklet (vsync); transmitter tasklet (errors) | yes |
| st20 tx `notify_rtp_done` | `st20_api.h:1277`, `:1401` | `st_tx_video_session.c:2261` | builder tasklet | yes |
| st20 rx `notify_frame_ready` | `st20_api.h:1544` | `rv_notify_frame_ready` `st_rx_video_session.c:714-721` ← `rv_frame_notify` `:846` | RX tasklet, or X-LC with `USE_MULTI_THREADS` | yes on the tasklet path |
| st22 rx `notify_frame_ready` | `st20_api.h:1705` | `st_rx_video_session.c:738` ← `:986` | RX tasklet | yes |
| st20 rx `notify_slice_ready` | `st20_api.h:1610` | `:1095` | RX tasklet | yes |
| st20 rx `query_ext_frame` | `st20_api.h:1599` | `:1273` (`rv_slot_by_tmstamp`) | RX tasklet | yes |
| st20 rx `uframe_pg_callback` | `st20_api.h:1586` | `:1788`, `:1795` (per packet) | RX tasklet | yes |
| st20 rx `notify_detected` | `st20_api.h:1592` | `:2803`, followed by `rv_init_sw` (allocations) `:2839` | RX tasklet | yes |
| st20/st22 rx `notify_event` | `st20_api.h:1557`, `:1713` | VSYNC `:3488` | RX tasklet | yes |
| st20/st22 rx `notify_rtp_ready` | `st20_api.h:1619`, `:1724` | `:1971` | RX tasklet | yes |
| st30 tx `get_next_frame` / `notify_frame_done` / `notify_frame_late` / `notify_rtp_done` | `st30_api.h:412,421,430,462` | `st_tx_audio_session.c:765`, `:925`, `:318`, `:1021` | audio tasklet | yes (`try_get` `:1639`) |
| st30 rx `notify_frame_ready` / `notify_rtp_ready` / `notify_timing_parser_result` | `st30_api.h:548,557,563` | `st_rx_audio_session.c:358`, `:628`, `:320` | RX audio tasklet | yes (`:830`) |
| st40 tx `get_next_frame` / `notify_frame_done` / `notify_frame_late` / `notify_rtp_done` | `st40_api.h:391,400,403,420` | `st_tx_ancillary_session.c:937`, `:1142` and `:100` (abort), `:393`, `:1234` | anc tasklet | yes (`:1319`) |
| st40 rx `notify_frame_ready` / `notify_rtp_ready` | `st40_api.h:546,522` | `st_rx_ancillary_session.c:288`, `:690` | RX anc tasklet | yes (`:784`) |
| st41 tx `get_next_frame` / `notify_frame_done` / `notify_rtp_done` | `st41_api.h:198,207,225` | `st_tx_fastmetadata_session.c:718`, `:890`, `:980` | fmd tasklet | yes (`:1065`) |
| st41 rx `notify_rtp_ready` | `st41_api.h:277` | `st_rx_fastmetadata_session.c:205` | RX fmd tasklet | yes (`:265`) |
| experimental st20rc `notify_frame_ready` / `notify_event` | `experimental/st20_combined_api.h:122,129` | `st2110/experimental/st20_redundant_combined_rx.c:40`, `:109` | RX tasklet (via the inner sessions) **[inferred]** | yes |

### 2.2 Pipeline layer (`st*p`)

| Callback | Declared | Call site(s) → context |
|---|---|---|
| st20p tx `notify_frame_available` | `st_pipeline_api.h:922` | Builder tasklet: `st20_pipeline_tx.c:175` (late drop), `:222` (`next_frame`), `:293` (`frame_done`, transmitter or builder). PLG converter thread: `:373` (`convert_put_frame`). App thread: `:1062` (`notify_ext_frame_free`), `:1181` (create). |
| st20p tx `notify_frame_done` | `st_pipeline_api.h:933` | Tasklet: `:162` (late), `:286` (done). PLG thread: `:384` (`convert_put_frame`). App thread: `:999` (`put_ext_frame` with an internal converter). **Four possible contexts for one callback.** |
| st20p tx `notify_frame_late` / `notify_event` | `:942`, `:985` | builder tasklet `:165`; tasklet `:305` |
| st20p rx `notify_frame_available` | `:1029` | RX tasklet `st20_pipeline_rx.c:277`, `:292`; PLG converter thread `:456` |
| st20p rx `query_ext_frame` | `:1042` | RX tasklet `:208` (from `frame_ready`), `:317` (from the transport's `query_ext_frame`) |
| st20p rx `notify_event` / `notify_detected` | `:1050`, `:1056` | RX tasklet `:335`, `:391`. Note that `rx_st20p_notify_detected` `:341-393` also `info()`-logs and allocates the destination buffers `:377` on the tasklet. |
| st22p tx `notify_frame_available` | `:1110` | tasklet `st22_pipeline_tx.c:183`, `:228`, `:281`; PLG encoder thread `:405` |
| st22p tx `notify_frame_done` / `notify_frame_late` / `notify_event` | `:1121`, `:1130`, `:1145` | tasklet `:174`, `:277`, `:178`, `:292` |
| st22p rx `notify_frame_available` / `notify_event` / `query_ext_frame` | `:1195`, `:1202`, `:1208` | tasklet `st22_pipeline_rx.c:192`, `:209`, `:142`; PLG decoder thread `:319` |
| st30p tx `notify_frame_available` / `notify_frame_done` / `notify_frame_late` | `st30_pipeline_api.h:140,146,154` | tasklet `st30_pipeline_tx.c:147`, `:179`, `:242`, `:138`, `:235` |
| st30p rx `notify_frame_available` | `st30_pipeline_api.h:301` | RX tasklet `st30_pipeline_rx.c:125` |
| st40p tx `notify_frame_available` / `notify_frame_done` / `notify_frame_late` | `st40_pipeline_api.h:164,171,180` | tasklet `st40_pipeline_tx.c:150`, `:181`, `:248`, `:141`, `:241` |
| st40p rx `notify_frame_available` | `st40_pipeline_api.h:246` | RX tasklet `st40_pipeline_rx.c:144` |

### 2.3 Plugin callbacks (`st_pipeline_api.h`)

| Callback | Declared | Context |
|---|---|---|
| encoder/decoder/converter `create_session` / `free_session` | `:713/718`, `:769/774`, `:819/824` | app thread, inside `st2*p_*_create` / `_free`, under the plugin manager pthread mutex `st_plugin.c:86-108`, `:184-206`, `:285-307` **[verified]** |
| encoder `notify_frame_available` | `:716` | app thread: `st22p_tx_put_frame` → `st22_pipeline_tx.c:61-72` ← `:843`, `:946` **[verified]** |
| decoder `notify_frame_available` | `:772` | RX tasklet: `rx_st22p_frame_ready` → `st22_pipeline_rx.c:49-62` ← `:200` **[verified]** |
| converter `notify_frame_available` | `:822` | TX side: app thread (`st20_pipeline_tx.c:877`, `:1003`). RX side: RX tasklet (`st20_pipeline_rx.c:288`). Via `st_plugin.c:269`. **[verified]** |
| `st_plugin_get_meta_fn` / `create_fn` / `free_fn` | `:85-93` | app thread at `st_plugin_register` / `mtl_uninit` **[inferred]** |

### 2.4 Instance-level callbacks (`mtl_api.h`)

| Callback | Declared | Context |
|---|---|---|
| `ptp_get_time_fn` | `:667` | Called through `mt_get_ptp_time` → `ptp_from_user` `dev/mt_dev.c:1603-1608`, on **every tasklet pacing and epoch computation**. It is user code on the hot path of pinned cores. **[verified]** |
| `ptp_sync_notify` | `:670` | `mt_ptp.c:759` ← `ptp_try_complete_result`. That is reached from `:1097` (delay-resp parse on the CNI/PTP tasklet on `main_sch`, or the CNI thread) and from `:828` (the EAL alarm thread). **[verified]** |
| `stat_dump_cb_fn` | `:675` | stat thread, or the EAL alarm thread if there is none. `mt_stat.c:58`, `:89-95`. **[verified]** |
| `mtl_log_printer_t` / `mtl_log_prefix_formatter_t` | `:224`, `:227` | **Any** thread that logs, tasklets included. `mt_log.h:24-39`. The default formatter calls `localtime_r` `mt_log.c:9-16`. **[verified]** |
| user tasklet `start` / `stop` / `handler` (`mtl_sch_api.h:46-68`) | | `handler`: T-LC or T-PT. `start`: scheduler thread at `sch_start`, **or the caller's thread** on runtime register `mt_sch.c:954`. `stop`: scheduler thread at `sch_stop` `:235`, **or the caller's thread** on runtime unregister `:906`. **[verified]** |

The header comments say "run from lcore tasklet routine" for most transport callbacks (`st20_api.h:1183-1184` and elsewhere). `notify_frame_late` and the pipeline `notify_frame_late` carry no such note, even though they run there too. **[verified]**

---

## 3. Public API functions: locks, blocking, allocation

### 3.1 Primitives shared between app threads and tasklets

| Primitive | Taken by tasklets | Taken by app-facing APIs | Kind |
|---|---|---|---|
| session spinlock S `mgr->mutex[idx]` | `try_get` in every session tasklet (2.1); blocking `get` in audio recovery `st_tx_audio_session.c:2742` and `tasklet_start` hooks (`:429`, anc `:499`, fmd `:366`) | blocking lock: tx stats `st_tx_video_session.c:4760,4778`; rx stats `st_rx_video_session.c:4622,4640`; update `:3848`/`:3983`; free `:3803` | `rte_spinlock`, not recursive **[verified]** |
| `sch->mutex` (pthread) | never | `mtl_sch_register/unregister_tasklet`, session create/free via `mt_sch_add_quota` / `sch_free_quota` | control plane **[verified]** |
| `sch->*_mgr_mutex` (pthread) | never | create/free/migrate | control plane **[verified]** |
| pipeline `block_wake_mutex` + cond | **yes**: lock/signal/unlock on every frame done/ready when `block_get` is set (`st20_pipeline_tx.c:29-34,41-43`, `st20_pipeline_rx.c:29-35`, the same in st22p/st30p/st40p) | `st2*p_*_get_frame` holds it while re-claiming and sleeps in `cond_timedwait` (`st20_pipeline_tx.c:774-789`); `*_wake_block` | pthread mutex/cond **[verified]** |
| `sch->sleep_wake_mutex` + cond | the scheduler thread itself when idle and sleep is allowed (`mt_sch.c:89-95`) | alarm thread signals `:52-61` | only used when idle |
| TSQ `tx_mutex` | blocking `rte_spinlock_lock` in `mt_tsq_burst` `datapath/mt_shared_queue.c:642` | flush/put paths **[inferred]** | spin across schedulers |
| SRSS list lock | blocking `srss_list_lock` inside the SRSS tasklet `mt_shared_rss.c:22-24,66-71` | entry add/remove at create/free **[inferred]** | spin |
| stat list lock | never | `mt_stat_register/unregister` (blocking spin), stat thread uses `trylock` `mt_stat.c:20-35` | control plane |
| handle guard `lc_refcnt` / `lc_destroying` | never | every public call does a SEQ_CST `fetch_add` + `load`; `*_free` spins in `mt_handle_drain` `mt_handle_guard.h:75-131` | lock-free, a contended cache line **[verified]** |

### 3.2 Classification of today's public functions

| Function (representative) | Context today | Blocks? | Locks shared with tasklet? | Allocates? |
|---|---|---|---|---|
| `st*_create`, `st*p_create` | app | yes: lcore IPC/`flock`, ARP wait, TSC wait | takes S only to publish | yes (hugepage, rings, mempools, flows) |
| `st*_free`, `st*p_free` | app | yes: `mt_handle_drain` spin, tasklet-unregister wait (1 ms sleeps, up to 1 s), queue flush busy-loop | blocking S (`tv_mgr_detach`) | frees |
| `st20_tx_update_destination` | app | **yes, up to `arp_timeout_ms` (60 s) while holding S** `st_tx_video_session.c:926` → `mt_arp.c:191` | blocking S | no |
| `st20_rx_update_source` | app | yes: flow create/destroy, IGMP, while holding S `st_rx_video_session.c:3932-3990` | blocking S | yes (RTCP) |
| `st20_tx/rx_get_session_stats`, `reset_*` | app | a short spin; self-deadlock if called from a callback | blocking S | no |
| `st20_tx_get_framebuffer`, `set_ext_frame` | app (often from inside `get_next_frame`) | no | none (refcnt read) | no |
| `st20_tx_get_mbuf` / `put_mbuf` | app | no | none; `rte_ring_sp_enqueue` (**single producer**, `:4705`), mempool alloc | mbuf from pool |
| `st20_rx_get_mbuf` / `put_mbuf` | app | no | none; `rte_ring_sc_dequeue` (**single consumer**, `st_rx_video_session.c:4761`), `rte_pktmbuf_free` | no |
| `st20_rx_put_framebuff` | app | no | `rte_atomic32_dec(refcnt)` | no |
| `st2*p_tx/rx_get_frame` | app | non-blocking; with `*_FLAG_BLOCK_GET` a `cond_timedwait` (default 1 s, `st20p_tx_set_block_timeout` `st20_pipeline_tx.c:1364`) | block mutex (shared with tasklet) | no |
| `st2*p_tx/rx_put_frame` | app | no; may run an inline SIMD convert (CPU time only) | none; atomic state store | no |
| `st2*p_*_wake_block` | app | no | block mutex | no |
| `mtl_sch_create/start/stop/free` | app | yes: lcore IPC; `sch_stop` sleeps 10 ms in a loop `mt_sch.c:317` | sch mutex | yes |
| `mtl_sch_register_tasklet` | app | no | sch mutex; calls `start` on the caller thread | yes |
| `mtl_sch_unregister_tasklet` | app **only**. From a tasklet on the same sch it sleeps 1 ms × 1000 and then fails. | yes | sch mutex | frees |
| `mtl_sch_enable_sleep`, `mtl_sch_set_sleep_us` | app | no | none (plain stores) | no |
| `mtl_get_lcore` / `mtl_bind_to_lcore` / `mtl_put_lcore` | app | IPC / `flock` | no | no |
| `mtl_ptp_read_time` | any | `mt_wait_tsc_stable` at first use | no | no |

### 3.3 The blocking get/put wake-up, in detail

```text
 app thread (st20p_tx_get_frame)                 tasklet (tx_st20p_frame_done -> notify_frame_available)
 -----------------------------------             ---------------------------------------------------------
 claim FREE->IN_USER  (CAS)  fails               store FREE (release)
 lock(block_wake_mutex)                          [user notify_frame_available()  <- user code, same core]
 loop: claim again (CAS scan)   <-- holds mutex  if block_get:
       cond_timedwait(deadline) -- releases -->     lock(block_wake_mutex)    <-- may FUTEX_WAIT if app holds it
 unlock                                             cond_signal()             <-- FUTEX_WAKE syscall if a waiter
                                                    unlock()                  <-- FUTEX_WAKE if contended
```

- Every frame event on a `BLOCK_GET` session does lock/signal/unlock on the pinned core, whether or not anyone is waiting. `st20_pipeline_tx.c:41-43`, `st20_pipeline_rx.c:42-45`. **[verified]**
- glibc `pthread_cond_signal` makes no syscall when there are no waiters. With a waiter it issues `FUTEX_WAKE`, typically around 1–3 µs, and it sends an IPI to the waiter's CPU. **[inferred]**
- `pthread_mutex_lock` on the default (non-adaptive) mutex goes to `FUTEX_WAIT` on contention. The app holds `block_wake_mutex` across the `tx_st20p_claim_available` scan (`st20_pipeline_tx.c:775-789`). If the app thread is preempted there, **the pinned core sleeps in the kernel until the app thread is rescheduled.** That is priority inversion on a pinned core.
  **[verified code; inferred scheduling]**
- The RX side sets `block_wake_pending` under the mutex (`st20_pipeline_rx.c:32`), so no wakeup is lost. The TX side re-claims on every wake and needs no flag. Both are correct in the lost-wakeup sense. **[verified]**
- Destroy is handled by `wake_on_destroy` → the same signal function (`mt_handle_guard.h:38-46`, `st20_pipeline_tx.c:1126,1198`). **[verified]**

Verdict for question 2: the tasklet does call `pthread_cond_signal`. At frame rate it is cheap in the common case. It still breaks the hard requirement twice over: it enters the kernel when a waiter exists, and it can block on a pthread mutex the app holds.

---

## 4. Scheduler model (current)

### 4.1 Loop and handler semantics

- One loop per scheduler, `sch_tasklet_func` `mt_sch.c:155-242`:
  - It calls every non-NULL `tasklet[i]->ops.handler(priv)` and sums the returns into `pending`, starting from `MTL_TASKLET_ALL_DONE` (0). `HAS_PENDING` is 1 (`mtl_sch_api.h:28-30`).
  - If `allow_sleep` is set and `pending == 0`, it calls `sch_tasklet_sleep`. **[verified]**
  - Negative handler returns are not filtered out, so `-1 + 1 == 0` can make a busy scheduler sleep. **[inferred]** Whether any handler returns a negative value: **[unknown]**.
- **Bug:** `tvs_tasklet_handler` *overwrites* `pending` for each session instead of summing (`st_tx_video_session.c:2694-2698`). The builder therefore reports the status of the last session only. With sleep enabled this can put a scheduler to sleep while earlier sessions still have work. **[verified]**
- The scheduler reads `max_tasklet_idx` (volatile) and `tasklet[i]` (plain pointers) without barriers. Register publishes `sch->tasklet[i]` after filling `ops` (`mt_sch.c:944-951`). That is safe on x86 TSO; on weakly ordered CPUs it would need a release store. **[inferred]**
- Unregister while running (`mt_sch.c:873-923`): set `request_exit` → the scheduler sets `ack_exit` and NULLs the slot (`:195-199`) → the caller polls with `mt_sleep_ms(1)` for up to 1 s, then calls `ops.stop` **on the caller's thread**. `request_exit`/`ack_exit` are plain `bool` (`mt_main.h:476-477`).
  **[verified]** Calling unregister from a tasklet on the same scheduler can never be acked: it times out after 1 s, sleeping on the pinned core. **[inferred]**
- Doc inconsistency: `mtl_sch_api.h:132-133` says unregister is allowed "at runtime before mtl_sch_start". The code supports both. **[verified]**

### 4.2 Sleep

- Sleep is per scheduler: `allow_sleep = MTL_FLAG_TASKLET_SLEEP` at init (`mt_sch.c:997`), togglable with `mtl_sch_enable_sleep` (`mt_main.c:1060`).
- Sleep length is `min(advice_sleep_us)` over the tasklets, defaulting to 1 ms, or the forced `mtl_sch_set_sleep_us` value (`mt_sch.c:64-81`, `mt_main.c:487,1095`).
- Below `sch_zero_sleep_threshold_us` (200) the scheduler calls `mt_sleep_ms(0)`, which is `rte_delay_us_sleep(0)` = `nanosleep` (`mt_main.h:1802-1807`). Above it, `rte_eal_alarm_set` + `cond_timedwait` with a 1 s safety net (`mt_sch.c:86-93`). **[verified]**
- The sleep path therefore makes syscalls and a malloc (`rte_eal_alarm_set` allocates and arms a timerfd **[inferred]**). This happens only when all tasklets are idle, which is acceptable by design.
- Video sessions advise sleeping for `trs × 128` (`st_tx_video_session.c:3481-3482`).
- Busy detection: `sleep_ratio_score > 70 %` marks a scheduler busy, and `!allow_sleep` counts as always busy (`mt_sch.h:40-45`). That feeds migration.

### 4.3 Quota, types, placement

- There are 18 schedulers (`mt_main.h:49`). Each carries `data_quota_mbs_limit`: by default 12 × a 1080p59 4:2:2 10-bit stream (`dev/mt_dev.c:1940-1945`, `st_header.h:23-31`).
- Quota is added and freed under `sch->mutex` (`mt_sch.c:1060-1082`, `:411-429`).
- `mt_sch_get_by_socket` (`:1139-1199`) first scans for an active, non-busy scheduler of the right type **on the requested socket** (`:1151-1157`). If none fits it requests a free slot (`sch_request` `:335`) and starts it if the instance is already started.
- Types (`mt_main.h:483-490`):
  - `DEFAULT`
  - `RX_VIDEO_ONLY` (`MTL_FLAG_RX_SEPARATE_VIDEO_LCORE`)
  - `APP` (from `mtl_sch_create`; never started by `mtl_start`, `mt_sch.c:1209`)
  - `SYSTEM` (`MTL_FLAG_DEDICATED_SYS_LCORE`)
- Quota-0 sessions (anc, fmd, `main_sch` users) fit any type (`:433`). Audio uses `limit / rx_audio_sessions_max_per_sch` (`st_rx_audio_session.c:1646`).
- One scheduler hosts one manager per media type and direction (`mt_main.h:524-566`). A manager registers one tasklet, iterating its sessions; video TX has two (builder + transmitter, both on the same scheduler, so the SP/SC `packet_ring` is safe).
- The scheduler count grows lazily and shrinks when `ref_cnt` reaches 0 (`mt_sch_put` `:1084-1137`).

### 4.4 Migration (`MTL_FLAG_TX_VIDEO_MIGRATE` / `RX_VIDEO_MIGRATE`)

- The admin thread runs every 6 s (`mt_admin.c:379`).
- It takes **blocking session spinlocks** for every video session to compute CPU busy (`mt_admin.c:30`, `:65`).
- It moves the last busy session to another scheduler, holding target mgr mutex → source mgr mutex → source S → target S: `tx_video_migrate_to` `mt_admin.c:101-139`, `from_tx_mgr->sessions[from_idx] = NULL` `:123`. **[verified]**
- Consequence for the API: after a migration, all later callbacks and all tasklet-side producers for that session run on a different thread and CPU, possibly with different L1/L2 cache contents. Anything per-session and single-producer (an SPSC ring, for example) survives only because migration happens under S, when neither old nor new tasklet is running that session. **[inferred]**

### 4.5 Lcore arbitration

- **Manager connected:** `mt_sch_get_lcore` walks the EAL lcores (`rte_get_next_lcore`) and asks MtlManager for each (`mt_instance_get_lcore` → UDS request/response, `mt_instance.c:46-56`) until one is granted (`mt_sch.c:712-731`).
  The manager keeps a `std::bitset<128>` under a mutex and releases an instance's lcores on disconnect (`manager/mtl_lcore.hpp:11-52`, `manager/mtl_instance.hpp:68`). **[verified]**
- **No manager (legacy):** a SysV shm table keyed `ftok("/dev/null",21)` under `flock(/tmp/kahawai_lcore.lock)`, with dead-PID cleanup (`mt_sch.c:505-620`, `:685-699`). `doc/shm_lcore.md` calls this "outdated". Tools: `mtl_lcore_shm_print` / `mtl_lcore_shm_clean` (`mtl_lcore_shm_api.h`).
- The walk is NUMA-filtered unless `MTL_FLAG_NOT_BIND_NUMA`, and falls back across NUMA with `MTL_FLAG_ALLOW_ACROSS_NUMA_CORE` (`mt_sch.c:781-786`).
- Both paths are syscalls and IPC. They run only at scheduler start, which is the control plane. **[verified]**

### 4.6 User-created schedulers and tasklets (`mtl_sch_api.h`)

- `mtl_sch_create` takes a free slot of type `APP` on `MTL_PORT_P`'s socket (`mt_sch.c:1394-1417`). The app must call `mtl_sch_start` and `mtl_sch_stop` itself.
- User tasklets may also be registered onto library schedulers if the app obtains one. It cannot: there is no public accessor, so in practice only `APP` schedulers can host them. **[inferred]**
- The only contract is "non-block method" (`mtl_sch_api.h:42-61`). Nothing enforces it or measures it beyond `MTL_FLAG_TASKLET_TIME_MEASURE` (`mt_sch.c:120-124`, `:203-209`).

---

## 5. Cross-thread handoff mechanisms today

| Handoff | Mechanism | Lock-free? | Hazard |
|---|---|---|---|
| TX frame mode, app → lib | `get_next_frame` pull callback plus the `st_frame_trans.refcnt` atomic | yes | user code polled at loop rate on the tasklet |
| TX frame done, lib → app | extbuf `free_cb` → `notify_frame_done` | yes | fires in whatever thread frees the mbuf (2.1) |
| TX RTP, app → lib | `packet_ring` SP/SC `rte_ring` (`st_tx_video_session.c:3011`, enqueue `:4705`) | yes | single app producer only; a second app thread corrupts the ring **[inferred]** |
| RX RTP, lib → app | `rtps_ring` SP/SC (`st_rx_video_session.c:532`, dequeue `:4761`) + `notify_rtp_ready` | yes | single app consumer only |
| RX frame, lib → app | `notify_frame_ready` + `st20_rx_put_framebuff` → `rte_atomic32_dec(refcnt)` | yes | |
| Pipeline framebuffers | `_Atomic uint32_t stat` per framebuff, CAS claims (`st20_pipeline_tx.c:84-98`, `:210-215`) | yes | `doc/design.md:361-377` restricts it to one app thread per direction |
| Pipeline wake | pthread mutex + cond, signalled from the tasklet | **no** | section 3.3 |
| Audio/anc/fmd TX mgr → transmitter | `RING_F_MP_HTS_ENQ \| RING_F_SC_DEQ` (`st_tx_audio_session.c:1704`) | lock-free, not wait-free | HTS makes a preempted producer stall the others; producers are tasklets only |
| Handle lifetime | `lc_destroying` / `lc_refcnt` Dekker pair, SEQ_CST (`mt_handle_guard.h:48-131`) | yes | one RMW per call on a line next to tasklet-read fields (section 8) |
| Stats | copied under S (video) or read as relaxed atomics (pipeline counters) | no (video) | hazard H2 |
| mbuf alloc/free from app threads | mempool default ops (`mt_util.c:544`, `ops_name` normally NULL → ring MP/MC **[inferred]**). Non-EAL app threads have no per-lcore cache. | lock-free | an MP/MC `rte_ring` update waits for earlier in-flight ops to publish their tail. A preempted app thread mid-dequeue makes the **tasklet** spin in `__rte_ring_update_tail` **[inferred]** |
| Scheduler control | `rte_atomic32` `request_stop` / `stopped`, plain-bool exit handshake | yes | |

---

## 6. Hazards that violate the hard requirement today

| # | Hazard | Evidence | Severity |
|---|---|---|---|
| H1 | User callbacks run on pinned tasklets while S is held. Any blocking or slow user code stalls every session on that lcore. | 2.1 / 2.2 | design |
| H2 | Self-deadlock: a same-session stats/update/free call from inside a callback spins forever on S. | `st_tx_video_session.c:2682` + `:4760`; `st20_pipeline_tx.c:1311` | fatal |
| H3 | The tasklet does pthread mutex + cond_signal for `BLOCK_GET` wake-ups. It can futex-wait on a mutex the app holds. | `st20_pipeline_tx.c:29-45`, `:774-789` | high |
| H4 | TX-hang recovery on the transmitter tasklet does queue re-acquire (pthread mutex + RL shaper), mempool re-create, busy flush and logging. Audio recovery also takes blocking S. | `st_video_transmitter.c:681`, `st_tx_video_session.c:4264-4331`; `st_audio_transmitter.c:58`, `st_tx_audio_session.c:2742-2753` | high (error path) |
| H5 | RX auto-detect allocates on the tasklet (`rv_init_sw`, `mt_rte_zmalloc_socket`) and calls the user `notify_detected`. | `st_rx_video_session.c:2839`, `st20_pipeline_rx.c:377` | medium |
| H6 | `update_destination` holds S across an ARP wait of up to 60 s. `update_source` holds S across flow/IGMP work. The session is silent meanwhile, and blocking-S users (admin thread, audio recovery) spin. | `st_tx_video_session.c:3848-3855`, `:926`; `mt_arp.c:171-199`; `st_rx_video_session.c:3932-3990` | high |
| H7 | User code on the hot path by design: `ptp_get_time_fn`, log printer / `localtime_r`. | `dev/mt_dev.c:1603-1608`, `mt_log.h:24-39`, `mt_log.c:9-16` | medium |
| H8 | pcap dump writes files from the tasklet. USDT-triggered pcap opens files from the tasklet. | `st_rx_video_session.c:2895`, `:2958` | debug only |
| H9 | Blocking spinlocks inside tasklets (TSQ `tx_mutex`, SRSS list lock, `tasklet_start` hooks) can spin behind a control-plane holder. | `mt_shared_queue.c:642`, `mt_shared_rss.c:66-71`, `st_tx_audio_session.c:429` | medium |
| H10 | `USE_MULTI_THREADS`: when the packet ring is full the tasklet processes packets itself (`st_rx_video_session.c:2901-2907`) while the packet lcore does too (`:2479`). Two threads then touch one session's slots and fire callbacks concurrently. | code verified, race **[inferred]** | medium |
| H11 | The builder `pending` overwrite can sleep a scheduler that has work. | `st_tx_video_session.c:2694-2698` | low (sleep mode only) |
| H12 | The shipped samples teach `pthread_mutex_lock` + `pthread_cond_signal` inside tasklet callbacks. | `app/sample/fwd/rx_st20p_tx_st20p_downsample_fwd.c:29-30`, `ext_frame/rx_st20_pipeline_dyn_ext_frame_sample.c:33-34` | design |

---

## 7. Implications and constraints for the new API

### 7.1 Hard constraints

1. **C1 — No user code in tasklet context by default.** Every `get_next_frame` / `notify_*` / `query_*` becomes a queue operation: the tasklet posts, the app polls or waits. The only allowed exception is an explicit opt-in class ("inline callbacks", open question 1), with a documented allow-list of callable functions (none that take S).
2. **C2 — The tasklet side of every app↔lib channel is wait-free and syscall-free.**
   - Allowed: SPSC ring enqueue/dequeue, relaxed/release stores, a single atomic RMW on a line the app rarely writes.
   - Forbidden: pthread primitives, futex, eventfd write, malloc, logging above DEBUG, file I/O.
   - If a wake-up is needed, the tasklet only records that a waiter must be woken; someone else performs the syscall (7.3).
3. **C3 — The app side never holds anything a tasklet needs.** App-visible state must not sit behind S. Stats become seqlock snapshots or per-field relaxed atomics that the tasklet writes and nobody locks. Online updates build the new header or flow off to the side and publish it with a pointer swap plus a generation number that the tasklet acknowledges (RCU-like).
   The tasklet never waits for the app.
4. **C4 — Fast-path public calls are O(1), lock-free and non-allocating, and safe to call from any app thread under a stated threading contract** (7.5). They must also be safe to call from an inline callback if C1's exception exists. A single-producer or single-consumer ring must say so in the API, or be MP/MC-safe.
5. **C5 — Waits exist only on app threads**, with a timeout and a cancel/wake. They are never reachable from tasklets or inline callbacks. A wait must return `-EDEADLK` (or similar) when it detects that it is running on an MTL scheduler thread (cheap check: a thread-local "in tasklet" flag set by `sch_tasklet_func`).
6. **C6 — Control-plane work detected on a tasklet is deferred.** The tasklet posts a request (TX hang, format detected, RTCP config change) to a library control thread. That thread quiesces the session through a flag handshake (the tasklet acks by skipping the session), does the heavy work, and republishes.
7. **C7 — Ordering survives migration.** Per-session event queues must stay single-producer across a migration. Today that holds only because migration runs under S (4.4). Keep an equivalent quiesce handshake, or drop migration (open question 7).
8. **C8 — NUMA placement.** Event rings and wait objects live in hugepage memory on the scheduler's socket (`mt_sch_socket_id`). The doc must say that app threads polling them should run on the same socket. Queue memory must be re-homed on migration if the target is on a different socket; today `mt_sch_get_by_socket` keeps the socket, so this is fine. **[verified]** `mt_sch.c:1151`.

### 7.2 Event / completion queue shape

```text
 per session (or per app-chosen "event context" shared by N sessions)
 +----------------------------------------------------------------------------------+
 | sq (app -> lib):  SPSC ring of frame/buffer descriptors  (replaces get_next_frame,|
 |                   query_ext_frame, put_framebuff, put_mbuf)                       |
 | cq (lib -> app):  SPSC ring of fixed-size event records  (replaces notify_*)       |
 |                   {type, session, frame_idx/opaque, status, rtp_ts, tai, flags}    |
 | overflow:         atomic counter + "coalesced" bit per event class                 |
 | waiter word:      _Atomic u32 {armed, seq}   (app writes, tasklet reads)           |
 | doorbell:         optional fd (eventfd) owned by a lib waker thread, not the tasklet|
 +----------------------------------------------------------------------------------+
```

- **Fixed-size records, no pointers into tasklet stack.** Frame completion carries a slot index plus the metadata copied into the record, or into a per-slot metadata area the app owns while the frame is in its hands. **[inferred]** This matches today's `st_frame` handoff.
- **Overflow policy must be defined per event class** (open question 5):
  - Frame completions cannot be dropped. The ring must be sized at least `framebuff_cnt`, so it can never overflow when every frame is outstanding.
  - VSYNC, "late", stats and similar events can be coalesced into a counter.
- **Submit queue instead of `get_next_frame`.** The builder pops from the SQ, and an empty SQ equals today's `-EBUSY` return. This removes user code from the per-loop poll (`st_tx_video_session.c:1922` runs every iteration while idle). The RX analogue is to pre-post external buffers, like RDMA posted receives. That replaces `query_ext_frame`.
- **One CQ per app thread, not per session**, is useful for apps with many sessions (a libfabric-style shared CQ, see research note 09). It needs either an MPSC CQ (all tasklets producing) or a per-scheduler CQ, where the producer is one lcore. Per-scheduler plus migration is awkward; per session plus an app-side "poll set" is simpler. See open question 4.

### 7.3 Wake-up mechanism options (tasklet side stays syscall-free)

| Option | Tasklet cost | Wake latency | Notes |
|---|---|---|---|
| W0: app busy-polls the CQ | 0 | ~0 | needs a core per app thread; natural for pinned apps (`mtl_get_lcore`) |
| W1: app spins, then sleeps with backoff (no wake) | 0 | up to the backoff | simple and bounded; wastes some latency |
| W2: armed waiter + tasklet `eventfd` write | about 1–2 µs syscall only when armed **[inferred]** | about 5–20 µs | violates C2 when armed; could be an opt-in per session |
| W3: armed waiter + lib **waker thread** (unpinned). The tasklet sets a bit in an MPSC bitmap or ring; the waker writes the eventfd / `futex_wake`. | one atomic OR | +1 thread hop (µs to tens of µs) | keeps C2. The waker itself must not spin; it can poll bits at, for example, 50–100 µs, or be woken by the alarm thread. |
| W4: `FUTEX_WAKE` directly from the tasklet on an app-owned word | syscall when armed | lowest | same objection as W2; Linux-only |

- Armed-waiter protocol: the app does `armed=1` (SEQ_CST) → re-checks the CQ → sleeps. The tasklet does enqueue (release) → `if (load(armed))` → request a wake. This is the Dekker pattern already used by `mt_handle_guard.h`. **[inferred]**
- Today's cost of the unconditional lock/signal disappears when nobody is armed.
- The app-facing wait object should be an fd, or an fd plus a timeout call, so it composes with `epoll` / GStreamer / FFmpeg event loops. **[inferred]**

### 7.4 Progress model

- **Today: automatic progress only** (libfabric's `FI_PROGRESS_AUTO` analogue). Tasklets on library-owned schedulers make all data-path progress. App calls never advance the NIC. **[verified]**
  - The one exception is the internal converter, which the app thread runs inside `get_frame`/`put_frame` (`st20_pipeline_rx.c:878`, `st20_pipeline_tx.c:869`). That is app-thread compute, not progress.
- Keep auto progress as the default and the only mode for pacing-critical TX video, which needs µs-level pacing.
- A **manual-progress** mode (`FI_PROGRESS_MANUAL` analogue) could subsume `mtl_sch_api.h`. The app would own a scheduler and call `mtl_sch_run_once()` from its own pinned thread. That makes the app thread the tasklet thread, and C1's "inline callback" question disappears, because it is the app's own core. Cost: one more mode, and pacing then depends on the app loop. See open question 3.

### 7.5 Proposed classification of API functions

| Class | Rules | Examples (new API) |
|---|---|---|
| **CP — control plane** | Any app thread except MTL scheduler threads and inline callbacks. May block, allocate, do IPC, log. Must not hold any tasklet-visible lock while blocking. Serialised per session internally. | instance init/start/stop, session create/destroy/start/stop, update destination/source (ARP and flow done without S), plugin register, get lcore, stats reset |
| **DP — data-plane-fast** | Any app thread, and allowed from inline callbacks. Lock-free, wait-free on the tasklet side, O(1), no syscalls, no allocation, no logging above DEBUG. States its threading contract (SP/SC per queue, or MP-safe). Returns `-EAGAIN` instead of waiting. | submit frame/buffer, poll/peek CQ, claim/put frame, get/put mbuf, read stats snapshot (seqlock), read PTP time |
| **WT — wait** | App threads only; detect and refuse scheduler threads (C5). Bounded timeout; cancellable; wakeable on destroy. Implemented only on top of DP plus the armed-waiter protocol. | wait CQ / wait-frame with timeout, get event fd, wake/cancel |

Two things are needed to make it enforceable:
- A thread-local `mt_in_sch_thread` flag, set in `sch_tasklet_func`, which CP and WT entry points check.
- Header annotations (for example `MTL_API_CP` / `MTL_API_DP` / `MTL_API_WAIT`) so the contract is in `include/`, not only in docs. **[inferred]**

---

## 8. Risks

- **Priority inversion.**
  - H3: the tasklet waits on a mutex the app holds.
  - Any future design where the tasklet waits for an app-side lock or ring slot. The new rule is that the tasklet never waits for the app: a full CQ means drop or coalesce, an empty SQ means idle.
  - App threads run `SCHED_OTHER` on shared cores, so they can be preempted at any point. **[inferred]**
- **Spinlock holder preemption.**
  - An app or admin thread holding S for a stats copy or an ARP wait makes the session skip TX/RX windows (tasklets `try_get`). A blocking-S user on a tasklet (audio recovery) spins on the pinned core. H6/H9.
  - The same class of problem exists invisibly in DPDK MP/MC rings and mempools used from non-EAL app threads (section 5). The new API should make app-side buffer recycling go through SPSC rings that the tasklet drains into the mempool itself, so app threads never touch MP/MC rings that tasklets also use. **[inferred]**
- **False sharing.**
  - `lc_refcnt` (a SEQ_CST RMW on every public call) shares a cache line with `impl`, `idx` and `type` in `struct st20p_tx_ctx` (`st20_pipeline_tx.h:35-42`), which the tasklet reads per frame.
  - `stat_*` counters that the app writes plainly (`stat_get_frame_try`) sit next to ones the tasklet writes plainly (`stat_drop_frame++` at `st20_pipeline_tx.c:145`, also written by the app at `:920`). That is also a data race.
  - Framebuffer `stat` words are not cache-aligned (`st20_pipeline_tx.h:22-33`).
  - New queues need producer and consumer indices on separate lines (as `rte_ring` does), and a guard design that does not RMW a line tasklets read. Options: per-thread epoch/QSBR (`rte_rcu_qsbr`), or keep the guard only on CP calls. **[inferred]**
- **NUMA.**
  - Schedulers are NIC-socket bound by default (`mt_sch.c:1151`, `:781-786`). App threads are not; nothing tells the app which socket its CQ lives on.
  - The recent `MPOL_LOCAL` change (commit `189e1e91`, `mt_sch.c:126-153`) shows that NUMA balancing already disturbs scheduler memory. App-shared queues would be exposed to the same page-migration stalls if the app's threads run on the other socket. **[inferred]**
- **Callback thread identity.** Apps that use thread-local state or assume "callbacks come from one thread" break under migration or TSQ. The new API should promise nothing about which thread produces events, only per-session ordering.
- **`MTL_FLAG_TASKLET_THREAD`.** Unpinned tasklets are ordinary preemptible threads, so every "never blocks the core" argument turns into "never blocks the tasklet thread". The constraints still hold, and the unregistered-non-EAL caveats apply (KB §2).

---

## Open questions for the maintainer

1. **May any user callback still run in tasklet context?** It matters because slice mode (`query_frame_lines_ready`), RTP mode and `uframe_pg_callback` are latency-critical, and a queue round-trip adds at least one app poll interval. Options:
   - (a) None; queues only.
   - (b) An explicit opt-in "inline callback" class with an allow-list of DP functions, a time budget measured with `stat_max_notify_frame_us`-style counters, and forced unregister on abuse.
   - (c) Inline callbacks only in manual-progress mode (question 3), where the "tasklet" is the app's own thread.
2. **May a pinned tasklet ever issue a syscall to wake an app?** This decides the wait design (7.3), and today's code already does it (H3). Options:
   - (a) Never: a waker thread (W3).
   - (b) Only when a waiter is armed, as a per-session opt-in (W2/W4).
   - (c) Never, and no blocking waits at all; apps poll or back off (W0/W1).
3. **Should the new API offer a manual-progress mode?** A libfabric-`FI_PROGRESS_MANUAL`-style `run_once()` on an app-owned scheduler would replace `mtl_sch_api.h` and the `MT_SCH_TYPE_APP` special case. It also gives integrators like GStreamer and OBS a way to own their threads. The cost is a second progress model, and pacing quality that depends on the app.
   Options: (a) auto only; (b) auto by default plus manual for non-video or for apps with their own pinned loop; (c) manual as a first-class peer.
4. **What is the app-side threading contract for DP calls?** Today it is "one app thread per session and direction" (`doc/design.md:361-377`), with SP/SC rings that silently corrupt if violated. Options:
   - (a) Keep single-thread per queue, documented and debug-asserted.
   - (b) MP/MC-safe queues. The cost is RMW contention and the preemption hazards in section 8.
   - (c) Per-thread queues bound to a shared "poll set".
5. **What is the event-queue overflow policy?** The tasklet may never wait, so every class needs a rule. Options: (a) size the CQ ≥ outstanding frames and coalesce the rest; (b) drop with a counter plus an overflow event; (c) backpressure by making the SQ refuse new frames. VSYNC and "late" events probably want (a), completions must be (a) or (c).
6. **Where does recovery and auto-detect work run?** H4/H5 do control-plane work on tasklets. Moving it to a control thread needs a quiesce handshake, and changes the recovery latency (today it is immediate on the transmitter).
   Options: (a) the admin thread, event-driven instead of every 6 s; (b) a dedicated per-instance control worker; (c) leave it on the tasklet but only the non-blocking parts, with the rest deferred.
7. **Keep runtime session migration (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`)?** It is the main reason tasklet-side producers change thread (4.4). It takes blocking session spinlocks every 6 s, and it complicates single-producer queues and NUMA homing. Options: (a) drop it and rely on better static placement; (b) keep it with an explicit quiesce/ack handshake per session instead of S; (c) keep it as is.
8. **What should the stats API guarantee?** Today it is a consistent copy under S, and a same-session call from a callback deadlocks (H2). Options: (a) a seqlock snapshot (consistent, lock-free, the reader retries); (b) per-field relaxed atomics (no consistency across fields); (c) stats delivered as periodic CQ events.
9. **Should user hot-path hooks survive in the tasklet: `ptp_get_time_fn` and the log printer?** Both are user code on pinned cores today (H7). Options:
   - (a) Keep them, documented as DP-class with the same rules as inline callbacks.
   - (b) Replace `ptp_get_time_fn` with a shared-memory time base (offset + TSC ratio) that the user updates, and route tasklet logging through a lock-free log ring drained by a lib thread.
   - (c) Remove them.
10. **What happens to `MTL_FLAG_TASKLET_THREAD` and `MTL_FLAG_TASKLET_SLEEP`?** Both change what "pinned core" means, and the sleep path uses alarms, `nanosleep` and condvars on the scheduler thread.
    Options: (a) keep both as they are, since only idle paths enter the kernel; (b) keep the thread mode but make it register as EAL, which KB §2 warns breaks multi-process; (c) fold them into the manual-progress mode of question 3.
11. **What is the handle-guard cost model on DP calls?** Every public call does a SEQ_CST RMW on `lc_refcnt` (`mt_handle_guard.h:92`), which is fine for CP and measurable for per-packet DP (RTP mode). Options: (a) keep it everywhere; (b) CP only, with DP calls valid only between start and stop by contract; (c) QSBR/epoch-based protection for DP calls.
12. **Should the legacy shm lcore allocator stay supported?** `doc/shm_lcore.md` calls it outdated. It adds `flock` + SysV shm paths to scheduler start and a separate public header (`mtl_lcore_shm_api.h`). Options: (a) drop it in the new API and require MtlManager or an explicit `lcores` list; (b) keep it as a fallback that is not exposed in the new headers.

