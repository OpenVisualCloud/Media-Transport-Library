# Research note 06 — Observability: stats, events, errors, diagnostics (current state)

| | |
|---|---|
| Topic | Observability of the public MTL API today: counters, callbacks/events, per-frame results, timing parser, logs, stats dump, USDT, error reporting, port/PTP/scheduler health |
| Revision | 1 |
| Date | 2026-09-29 |
| Baseline | `main` HEAD `545a266a`. Source was read from `git archive HEAD`, not from the working tree. The working tree has uncommitted edits in `lib/` (`mt_rtcp.c`, `mt_sch.c`, `st_tx_video_session.c`, …), and those were ignored. `include/` is identical in both. |
| Status | Research only. Nothing here is implemented. |

Legend: **[verified]** = checked in code at `path:line` (HEAD). **[inferred]** = follows from verified code but not exercised. **[unknown]** = not determined.
Paths without a directory are under `lib/src/` or `lib/src/st2110/`.

---

## 1. Inventory

### 1.1 Pull getters (app calls the library)

| API | Returns | Scope | Reset | Read consistency (notes below) |
|---|---|---|---|---|
| `st20/st30/st40/st41_{tx,rx}_get_session_stats` | Media-specific `*_user_stats` embedding `st_tx_user_stats` / `st_rx_user_stats` (`include/st_api.h:344`, `:397`) with `port[P/R]` arrays (`st_api.h:251`, `:266`) | session + session port | yes | N1, N2 |
| `st20p/st30p/st40p_{tx,rx}_get_session_stats` | Same transport struct, with the pipeline `stat_frames_sent/dropped/received/corrupted` overlaid | session | yes | N3 |
| ST22 / ST22p | **No public getter exists** (none found in `include/`) **[verified]** | — | — | — |
| `mtl_get_port_stats` / `mtl_reset_port_stats` | `mtl_port_status` (`include/mtl_api.h:792`): rx/tx pkts and bytes, `rx_err`, `rx_hw_dropped` (imissed), `rx_nombuf`, `tx_err` | physical port | yes | N4 |
| `mtl_get_var_info`, `st_get_var_info`, `mtl_get_fix_info` | sch/lcore/DMA counts, `dev_started`, session counts (`mtl_api.h:766-789`, `st_api.h:225`) | global | n/a | trivial |
| `mtl_ptp_read_time` / `_raw` | current PTP time only; there is no lock/offset/state getter **[verified]** | global | n/a | — |
| `st20p_tx_get_pacing_params`, `st20p_*_get_sch_idx`, `st20p_rx_get_queue_meta` | tr_offset/trs/vrx, scheduler index, queue IDs (static config) | session | n/a | — |
| `st20(p)_rx_timing_parser_critical` | 2110-21 pass thresholds (`st20_rx_tp_pass`, `include/st20_api.h:515`), not results | session | n/a | — |
| `st20(p)_rx_pcapng_dump` | packet capture to file | session | n/a | — |

- **N1.** The getter does a `memcpy` under the per-session `rte_spinlock` (`st_tx_video_session.c:4760-4762`). The same shape is at `st_rx_video_session.c:4622`, `st_tx_audio_session.c:3067`, `st_rx_audio_session.c:1858`, `st_tx_ancillary_session.c:2478`, `st_rx_ancillary_session.c:1816`, `st_tx_fastmetadata_session.c:2222`, `st_rx_fastmetadata_session.c:1159`.
  Reset does a `memset` under the same lock (`st_tx_video_session.c:4778-4781`).
  The tasklets `trylock` that same lock (`st_tx_video_session.h:29-35`, `st_video_transmitter.c:674`), so each stats read makes the tasklet skip that session for one pass **[verified]**.
- **N2.** Exception: ST20 RX `pkt_lcore` mode runs `rv_handle_frame_pkt()` on a separate lcore without the session lock (`st_rx_video_session.c:2469-2480`). Its counters are written unsynchronized while the app reads them **[verified]**.
- **N3.** The transport copy happens under the spinlock, then the pipeline counters are read with relaxed `atomic_load` (`pipeline/st20_pipeline_tx.c:1300-1322`). The result is not one atomic snapshot across the two layers **[verified]**.
- **N4.** The getter pulls `rte_eth_stats_get` into accumulators under `inf->stats_lock`, then does the `memcpy` after the unlock (`dev/mt_dev.c:2635-2651`). Reset `memset`s without the lock (`mt_dev.c:2654-2669`). Both race the stat and admin threads, which update the same struct (`mt_dev.c:205-207`) **[verified]**.
  The library resets the HW counters after every read (`mt_dev.c:210-212`), so external ethdev readers see resets **[verified]**. On iavf, `tx_err_packets` is never accumulated (`mt_dev.c:168`, "iavf wrong report the tx error"), so VF users always read 0 **[verified]**.

### 1.2 Push callbacks (the library calls the app)

| Callback | Context | Payload | Media types | Notes |
|---|---|---|---|---|
| `notify_event(ST_EVENT_VSYNC)` | session tasklet, each epoch, when `*_FLAG_ENABLE_VSYNC` is set (`st_tx_video_session.c:303-315`, `st_rx_video_session.c:3479`) | `st10_vsync_meta` | ST20/ST22 + pipelines | `stat_vsync_mismatch++` if the tasklet is more than 1 ms late (`st_tx_video_session.c:313-315`) **[verified]** |
| `notify_event(ST_EVENT_RECOVERY_ERROR / FATAL_ERROR)` | TX video transmitter tasklet, under the session spinlock (`st_video_transmitter.c:674-686` → `st_tx_video_session.c:4231-4332`) | `NULL` | **ST20/ST22 TX only** | audio/anc/fmd recovery only bumps counters (`st_tx_audio_session.c:2720-2780`). RX has no error events **[verified]** |
| `notify_frame_late(priv, epoch_skipped)` | TX builder tasklet, inside the pacing decision, under the lock (`st_tx_video_session.c:682-684`) | skip count | ST20/22, ST30, ST40 TX + pipelines (not ST41) | units differ per type (§4 F6). Wrong `priv` on one pipeline path (§4 F1) |
| `notify_frame_done` | mbuf ext-buffer free callback, i.e. after NIC TX completion, not at wire time (`st_tx_video_session.c:116-134`) **[verified]** | pipeline: `st_frame.status/timestamp/epoch/rtp_timestamp` | all TX | the only per-frame TX result. No send time, no lateness |
| `notify_frame_ready` / `notify_frame_available` | RX tasklet | frame meta (§1.3) | all RX | — |
| `notify_detected` | RX tasklet, once on detection (`st_rx_video_session.c:2800-2850`) | `st20_detect_meta` | ST20 with `AUTO_DETECT` | no callback on failure (§3) |
| `notify_timing_parser_result` | ST30 RX tasklet, ~every 200 ms (`include/st30_api.h:325-332`, `:563`) | dpvr/ipt/tsdf + compliance | ST30 RX only | — |
| `ptp_sync_notify` | built-in PTP, per accepted DELAY_RESP, **port P only** (`mt_ptp.c:753-760`). Runs in the CNI tasklet or thread (`mt_cni.c:567-579`) | `master_utc_offset`, `delta` | global | no lock state, path delay, GM ID, or loss notice **[verified]** |
| `stat_dump_cb_fn(priv)` | `mtl_stat` thread every `dump_period_s` (default 10 s), after the internal dump (`mt_stat.c:46-61`) | none (a tick) | global | shares `priv` with `ptp_get_time_fn` (`include/mtl_api.h:622`) |
| `mtl_set_log_printer` | any thread that logs, including tasklets, synchronously (`mt_log.h:24-40`) | level + printf varargs | process-global | no instance/session/module ID, no user priv |

### 1.3 Per-unit (per-frame) results

| Field(s) | Side | Content | Notes |
|---|---|---|---|
| `status` in `st_frame` (`include/st_pipeline_api.h:298`), `st20_rx_frame_meta` (`st20_api.h:561`), `st30_rx_frame_meta`, `st30_frame` | RX; TX done | `COMPLETE / RECONSTRUCTED / CORRUPTED / DROPPED` (`st_api.h:78-92`) | Without `RECEIVE_INCOMPLETE_FRAME`, CORRUPTED frames are never delivered and appear only in counters |
| `pkts_total`, `pkts_recv[P/R]` | RX ST20, ST20p, ST30p, ST40 | received packets (redundant copies excluded); per-port valid packets | shows which path delivered, per frame. ST30 transport meta has no per-port data (`st30_api.h:304-323`) **[verified]** |
| `frame_recv_size` vs `frame_total_size` | RX ST20/ST30 | bytes | integrity |
| `timestamp_first_pkt`, `timestamp_last_pkt`, `fpt`, `receive_timestamp` | RX | arrival TAI (HW time with `MTL_FLAG_ENABLE_HW_TIMESTAMP`, else SW time read at processing) | the `timestamp_last_pkt` doc says "first pkt" (`st20_api.h:576-577`) **[verified]** |
| `seq_lost`, `seq_discont`, `port_seq_lost[]`, `port_seq_discont[]` | RX ST40/ST40p | sequence integrity | ST40 only |
| `tp[P/R]` → `st20_rx_tp_meta` (`st20_api.h:475-510`) | RX ST20/ST20p with `*_TIMING_PARSER_META` | per frame: cinst/vrx/ipt min/max/avg, fpt, latency, rtp_offset, rtp_ts_delta, `compliant`, `failed_cause` | N5 |
| `timestamp/epoch/rtp_timestamp` on TX done | TX | scheduled media-time TAI and epoch index (`st_tx_video_session.c:1981-1988`) | this is not the actual send time **[verified]** |

- **N5.** The pipeline copies `tp` into the framebuffer (`pipeline/st20_pipeline_rx.c:250-256`). The ST20 transport points into slot memory (`st_rx_video_session.c:852-859`), which is valid only during the callback **[inferred]**.

### 1.4 Periodic dump (logs)

- The `mtl_stat` pthread is woken by an EAL alarm every `dump_period_s` (`mt_stat.c:61-99`, `:143-170`). It walks the registered callbacks under the `stat_mgr` spinlock (`mt_stat.c:30-44`). Registered: device, PTP, scheduler, each session manager, pipelines, RTCP **[verified]**.
- Session dumps collect under the session lock with a 10 µs timeout (`st_header.h:79`, `st_tx_video_session.c:3939-3953`) and log outside the lock. Deltas are computed against a private `stat_snapshot`, which the user reset also clears (`st_tx_video_session.c:4780`) **[verified]**.
- **Log-only information** (not in any public struct) **[verified]**:
  - TX session: fps, throughput, `cpu_busy_score`, inflight counts, internal build/transmit return codes (`st_err.h`), frames in transmit, tasklet time, get-next-frame and notify durations (`st_tx_video_session.c:3606-3745`).
  - Pipeline: framebuffer state histogram (dbg only, `pipeline/st20_pipeline_tx.c:672-679`); get/put/drop/convert-fail/busy counts (`:393-411`, `:680-686`).
  - PTP: delta, path delay, correct delta, sync timeouts, lock state (`mt_ptp.c:1490-1545`).
  - Scheduler: avg loop ns, sleep ratio, tasklet timing (`mt_sch.c:452-500`).
  - RTCP: NACK/retransmit counters (`mt_rtcp.c:420-445`).
  - RX timing parser aggregate at INFO (`st_rx_timing_parser.c:225-263`); `stat_untrusted_pkts`; `stat_pkts_pool_empty` (`st_header.h:682`).
- `mt_in_reset()` prints "DEV IN RESET", but `instance_in_reset` is only ever set to 0 (`mt_main.c:534`), so that branch is dead **[verified]**.

### 1.5 USDT (`mt_usdt_provider.d`, `doc/usdt.md`)

- Providers: `sys` (log_msg, tasklet/sessions time-measure enable, CNI pcap), `ptp` (ptp_msg t1..t4, ptp_result), and per media/pipeline `tx_frame_next/done/drop/get/put`, `rx_frame_available/put`, `rx_no_framebuffer`, `rx_frame_incomplete`, plus attach-to-enable dumps **[verified]**.
- Probes carry indices and RTP timestamps only. There is **no probe for lateness, recovery, fatal error, PTP state change, link, migration, or back-pressure** **[verified]**. USDT is Linux-only and a build option (knowledge base §5 "USDT Tracepoints").

### 1.6 Error codes from API calls

- Create functions return `NULL`, and the library never sets `errno`/`rte_errno` (0 assignments in `lib/src`) **[verified]**. The only reason given is an `err()` log line.
- `get_frame` returns `NULL` for "no frame", "timeout", "not ready", and "session dead" alike **[inferred]** from `pipeline/st20_pipeline_tx.c:760-790`.
- Pipeline stats getters and resets on a stale or destroying handle return **0 (success) without filling the output**: `MT_HANDLE_GUARD(ctx, ..., 0)` at `pipeline/st20_pipeline_tx.c:1309,1333`, `st20_pipeline_rx.c:1234,1260`, `st30_pipeline_tx.c:852`, `st30_pipeline_rx.c:606`, `st40_pipeline_tx.c:852`.
  ST40p RX uses `-EIO` (`st40_pipeline_rx.c:630,656`), and transport getters use `-EINVAL` **[verified]**.
- `mtl_get_log_level()` returns `-EIO` cast to `enum mtl_log_level` (`mt_log.c:119-125`) **[verified]**.

---

## 2. Live-stream questions: can MTL answer them today?

| Question | Current mechanism | Gap |
|---|---|---|
| Was my frame sent on time? | pipeline `notify_frame_done(status)`; `stat_frames_sent/dropped` | `done` fires at TX completion (mbuf free), and carries no actual-vs-target time **[verified]**. COMPLETE only means "not dropped". |
| How early/late was I? | `notify_frame_late(epoch_skipped)`; `stat_epoch_drop`, `stat_epoch_onward` (epoch units) | No ns margin anywhere. Pipeline drop paths hard-code `epoch_skipped = 0` (`pipeline/st20_pipeline_tx.c:165`, `st30_…:142`, `st40_…:145`, `st22_…:178`) **[verified]**. Early-within-window is invisible. |
| How many frames are queued? | dbg-level log histogram (`pipeline/st20_pipeline_tx.c:672-679`); "frames in trans" log (`st_tx_video_session.c:3585-3591`) | **No gauge API** for framebuffer states, ring depth, or inflight **[verified]**. |
| Did the NIC underrun / pacing slip? | `stat_trans_troffset_mismatch` (`st_video_transmitter.c:122-125`), `stat_exceed_frame_time` (`st_tx_video_session.c:2135-2140`), `stat_pkts_dummy` | No wire-level underrun signal. `stat_epoch_troffset_mismatch` is never incremented **[verified]**. |
| Did a frame miss its epoch and get skipped/dropped? | TX `stat_epoch_drop` (counts epochs, not frames); pipeline `stat_frames_dropped` + `DROPPED` status | Q1 |
| Is PTP locked, what offset? | `ptp_sync_notify` (port P); log dump | No getter. `locked` is sticky (Q2). |
| Did RX packets arrive late / out of order / duplicated? | `port[i].reordered_packets`, `duplicates_same_port`, `stat_pkts_redundant`, `lost_packets`; timing parser | Video reorder is intra-frame only. Same-port duplicates are always 0 for video (`st_api.h:290-320`). No per-packet lateness without the TP. |
| Was the RX stream 2110-21 compliant? | `*_TIMING_PARSER_META` → per-frame `compliant`; `_STAT` → INFO log; ST30 `notify_timing_parser_result` | Q3 |
| End-to-end latency? | RX `timestamp_first_pkt` vs `rtp_timestamp`; TP `latency`/`fpt` | The app must compute it. TX has no actual-send time. There is no "time in pipeline". |
| Which redundant path delivered? | per frame `pkts_recv[P/R]`; `port[i].frames`, `frames_partial[i]`, `stat_pkts_unrecovered`, `stat_pkts_redundant` | Good for video. ST30 transport has no per-frame per-port data. Video per-port `lost_packets` is charged one frame late (`doc/stats_guide.md:99-104`). |
| Is the link up? | probed only in `mtl_start` (`dev/mt_dev.c:815-850`, `:2008-2021`) | Q4 |
| Is my session CPU-starved? | `stat_exceed_frame_time`, `stat_user_busy`, `stat_vsync_mismatch`, RX `stat_burst_pkts_max`, `rx_hw_dropped_packets`; `MTL_FLAG_TASKLET_TIME_MEASURE` | `cpu_busy_score`, loop ns, tasklet times, sleep ratio: log-only **[verified]**. No migration event **[inferred]**. |
| Did a conversion/copy path get used? | RX ST20 `stat_pkts_dma`, `stat_pkts_copy_hdr_split`, `stat_pkts_enqueue_fallback`, `stat_pkts_multi_segments_received` | Q5 |
| Did recovery happen, did I lose frames? | `stat_recoverable_error`, `stat_unrecoverable_error`; ST20 TX events | In-flight frames at recovery are reported as done. In pipelines they become **`COMPLETE` and count in `stat_frames_sent`** (`st_tx_video_session.c:4290-4300` → `pipeline/st20_pipeline_tx.c:276-290`) **[verified]**. |

- **Q1.** Without `DROP_WHEN_LATE` + `USER_PACING`, a late frame is sent late (slipped) and the app only gets `epoch_skipped`. Audio reports lateness as `stat_epoch_mismatch` and silently clamps "late within window" into `stat_epoch_late` (`st_tx_audio_session.c:293-320`) **[verified]**.
- **Q2.** `locked` is set after 100 good samples and never cleared on sync loss (`mt_ptp.c:540-552`; cleared only at init, `:1317`). `connected` is also never cleared. `mt_ptp_is_locked()` and `mt_ptp_is_connected()` exist but are unused (`mt_ptp.h:131-136`) **[verified]**. With a user `ptp_get_time_fn`, MTL knows nothing about sync state.
- **Q3.** The result is trustworthy only with HW RX timestamps. With SW time, packets inside a burst are discarded as untrusted (`st_rx_video_session.c:1546-1557`), and that count is not exposed. Enabling the TP forces the detector init path (`:3427-3435`). There is no session-level compliance counter, and ST40/41 have no TP **[verified]**.
- **Q4.** No runtime link monitoring and no LSC/RESET ethdev callback: `rte_eth_dev_callback_register` does not appear in `lib/`. `MT_IF_STAT_PORT_DOWN` is set only at init (`mt_dev.c:2021`). No getter **[verified]**.
- **Q5.** The pipeline converter choice (internal, plugin, or derive) is only logged at create (`pipeline/st20_pipeline_tx.c:630-648`). The internal converter runs in the app thread inside `put_frame` (`:868-869`). There is no conversion counter or timing. TX chain vs copy is not reported **[inferred]**.

---

## 3. Asynchronous errors and exceptions

| Condition | Detection | Surfaced as | Recoverable? State after |
|---|---|---|---|
| TX queue hang (`tx_burst` returns 0 for longer than `tx_hang_detect_ms`, default 1 s) | transmitter tasklet (`st_video_transmitter.c:33-52`; audio `st_audio_transmitter.c:50-62`) | ST20/22: `RECOVERY_ERROR`/`FATAL_ERROR` + counters. Audio/anc/fmd: counters and logs only | E1 |
| Link down at runtime | none **[verified]** | indirect: RX counters stop. TX may loop hang→recovery **[unknown]**, NIC-dependent | — |
| VF/PF reset (`RTE_ETH_EVENT_INTR_RESET`) | not registered at HEAD **[verified]** | nothing | **[unknown]**; `instance_in_reset` is dead |
| PTP loss / GM change | `ptp_sync_timeout_handler` increments `stat_sync_timeout_err` (`mt_ptp.c:612-619`) | WARN in the dump only | `locked`/`connected` stay true. Pacing continues on a free-running PHC **[inferred]** |
| RX no signal / timeout | none in the library | pipeline `get_frame` blocks until the block timeout, then returns NULL | no signal lost/restored event |
| RX format mismatch (no auto-detect) | per-packet validation | `stat_pkts_wrong_{len,pt,ssrc,interlace}_dropped`, `err_packets` | stays mismatched, no event |
| Auto-detect failure | tasklet | one `err_once` log, then all packets silently dropped (`st_rx_video_session.c:2710-2716`, `:3206-3208`) | E2 |
| Format change after detect SUCCESS | not handled **[inferred]** | wrong-length drops | — |
| RX back-pressure | tasklet | `stat_frames_dropped` (pipeline), `stat_slot_get_frame_fail`, `stat_pkts_enqueue_fail`, "back-pressure … (sustained Kx)" log | E3 |
| NIC RX drop / nombuf | port stats | `rx_hw_dropped_packets`, `rx_nombuf_packets` + ERR log with xstats | per port, not per session |
| Scheduler overload | admin thread `cpu_busy_score` (`mt_admin.c:16-50`) | optional migration (`MTL_FLAG_TX/RX_VIDEO_MIGRATE`), logs | the session moves and the app is not told **[inferred]** |

- **E1.** Recovery drains the rings, gets a new queue, rebuilds the mempool with `recovery_idx++`, and returns to WAIT_FRAME (`st_tx_video_session.c:4231-4332`). In-flight frames are reported as "done" (misreported, §2). If recovery fails, `s->active=false` and the app must free the session.
  All of this runs **in the tasklet**: it takes a pthread mutex (`dev/mt_dev.c:1851`) and re-creates mempools, which breaks the two-world rule **[verified]**.
- **E2.** Recovery needs `update_source` or a recreate **[inferred]**. On success, `rv_init_sw()` is called from the tasklet (`st_rx_video_session.c:2839`), so it allocates in the data plane **[verified]**.
- **E3.** Self-healing. The troubleshooting table points at `stat_pkts_pool_empty` (`doc/stats_guide.md:405`), but that field is internal (`st_header.h:682`) **[verified]**.

Summary: the only structured async error channel is `notify_event`, with two opaque codes and no args (`include/st_api.h:208-222`). Only ST20/ST22 TX and their pipelines fire it **[verified]**. Every other failure is either a counter the app must poll and diff, or a log line.

---

## 4. Flaws (evidence-backed)

1. **F1 — Pipeline `notify_frame_late` gets the wrong `priv` on the non-drop path.** `st20p/st30p/st40p` forward the user's function pointer to the transport (`pipeline/st20_pipeline_tx.c:462`, `st30_pipeline_tx.c:283`, `st40_pipeline_tx.c:316`), but set `ops_tx.priv = ctx` (`st20_pipeline_tx.c:421`, `st30_…:256`, `st40_…:281`).
  The transport calls `s->ops.notify_frame_late(s->ops.priv, …)` (`st_tx_video_session.c:683`), so the app receives the internal pipeline context as its `priv`. The drop path passes the right `ctx->ops.priv` (`st20_pipeline_tx.c:165`). ST22p does not forward transport lateness at all **[verified]**.
2. **F2 — Recovery misreports frames.** In-flight frames during TX recovery complete as `COMPLETE` and count in `stat_frames_sent` (§2, last row) **[verified]**.
3. **F3 — Dead or misleading counters** **[verified]**:
   - `stat_epoch_troffset_mismatch` (ST20 TX) is never written.
   - `st30_rx_user_stats.stat_pkts_dropped` and `st40_rx_user_stats.stat_pkts_dropped` are never written, although `doc/stats_guide.md:83` says audio overflow bumps it.
   - `stat_epoch_mismatch` is always 0 for video.
   - `stat_epoch_drop` and `stat_epoch_mismatch` share the doc string "Total number of epoch mismatch events" (`st_api.h:350`, `:358`).
   - `stat_epoch_onward` counts epochs, not events.
   - `st41_rx_user_stats.stat_last_time` and `stat_max_notify_rtp_us` are gauges, refreshed only at dump time (`st_rx_fastmetadata_session.c:488-490`).
   - `stat_pkts_simulate_loss` is a test hook in the public ABI.
   - The knowledge base names a `stat_frame_late` counter that does not exist (`.github/copilot-docs/mtl-knowledge-base.md` §5 "Epoch Timing").
4. **F4 — Doc drift in `doc/stats_guide.md`.** It calls the TX late-drop check "post-send" (`:301-304`), but `tx_st20p_if_frame_late()` runs in `next_frame`, before transmit (`pipeline/st20_pipeline_tx.c:194`). It says all counters are "thread-safe (per-session spinlock)" (`:14`), which is false for pipeline overlays, pkt_lcore mode, and port stats **[verified]**.
5. **F5 — Coverage holes** **[verified]**:
   - ST22/ST22p have no stats getter.
   - Only ST20/ST22 TX have error events.
   - ST41 TX has no `notify_frame_late`.
   - ST30 transport RX meta has no per-port packet counts.
   - RTCP TX/NACK counters are log-only, 32-bit, and reset at every dump (`mt_rtcp.c:420-445`).
6. **F6 — Lateness semantics differ per media type** **[verified]**:
   - Video fires `notify_frame_late` when epochs are skipped, with the argument = frames skipped (`st_tx_video_session.c:678-684`).
   - Audio fires it when the target time is already in the past, with the argument in packet-time units (`st_tx_audio_session.c:314-319`), and silently clamps "late within window" into `stat_epoch_late` (`:293-299`).
   - Ancillary passes an epoch delta (`st_tx_ancillary_session.c:393`).
   - Pipelines pass `0`.
7. **F7 — Stats read races** **[verified]**:
   - Port stats (N4).
   - Scheduler and tasklet `mt_stat_u64` are updated by the scheduler thread (`mt_sch.c:206-226`) and read then reset by the stat thread (`:464-483`), with no synchronization (`mt_util.h:337-342`).
   - Pipeline `stat_get_frame_try/succ/put/drop` are plain ints written by the app thread or tasklet (`pipeline/st20_pipeline_tx.c:145,767,920`) and reset by the stat thread (`:683-686`).
   - Reading session stats takes the tasklet's lock. That is cheap, but the tasklet skips a pass (N1).
8. **F8 — Getters with side effects and cost.** `mtl_get_port_stats` calls `rte_eth_stats_get` from the app thread while holding `inf->stats_lock`. On a VF this is a PF mailbox round-trip; `mt_stat.c:152` notes it fails in alarm context **[inferred cost]**.
9. **F9 — Logs as the only signal.** See §1.4: PTP health, CPU starvation, RTCP, TP aggregate, and converter choice are log-only.
  The printer is process-global with no instance/session ID (`mt_log.c:38-45`). `MT_LOG` builds a wall-clock prefix (`time` + `localtime_r` + `strftime`) on every call, from any context including tasklets (`mt_log.h:24-40`, `mt_log.c:6-12`) **[verified]**.
10. **F10 — Non-dbg logs in tasklet paths** **[verified]**:
    - `info("max drop batch …")` in the TX pipeline `next_frame` callback (`pipeline/st20_pipeline_tx.c:197`).
    - `err`/`info` in the TX recovery path (`st_tx_video_session.c:4238-4325`).
    - `err_once` on detect failure.
    - `info` "st20 detected" in the RX tasklet (`st_rx_video_session.c:2847-2852`).

  Most are rare or one-shot, but `CLAUDE.md` forbids INFO in the data plane.
11. **F11 — Error-return inconsistency.** Pipeline getters return 0 on an invalid handle. Creates give no reason code. `get_frame` NULL is ambiguous. `mtl_get_log_level` returns a negative enum. `-EIO` is overloaded for invalid arguments (`mt_dev.c:2638-2645`) **[verified]**.
12. **F12 — Mixed kinds in one struct.** Counters (monotonic), gauges (`stat_burst_pkts_max`, `stat_last_time`), and estimates share one flat `uint64_t` list, and reset clears all of them. Video `stat_pkts_unrecovered` is an estimate, `(frame_size − recv)/avg_pkt` (`doc/stats_guide.md:324`) **[verified]**.
13. **F13 — PTP state is not trustworthy.** `locked` and `connected` are sticky (Q2), `ptp_sync_notify` covers port P only, and there is no PTP status struct **[verified]**.
14. **F14 — `stat_dump_cb_fn` is a bare tick.** It passes no data, shares `priv` with `ptp_get_time_fn`, and runs on the stat thread interleaved with the library's own dump logs **[verified]**.

---

## 5. Proposed taxonomy for the unified API

Principle: each fact has **one** channel and a declared kind, and the channel fits what the fact is used for.

| Kind | Examples | Channel | Writer (tasklet) | Reader (app) |
|---|---|---|---|---|
| **Counter** (monotonic u64) | pkts/bytes per port; frames sent/dropped/late/corrupted/reconstructed; epochs skipped; recoveries; NACKs; reject reasons | `mtl_session_stats_get(h, &s, sizeof s)`, versioned struct | single writer, relaxed store of an aligned u64, no lock; multi-writer → shards or atomics | lock-free; app computes deltas |
| **Gauge** | framebuffers per state; ring fill; inflight; PTP offset/path delay; `link_up`; sch busy %; current sch idx | same getter, separate sub-struct | owner, relaxed | "sampled"; no reset |
| **Summary / histogram** | TX lateness margin (target vs actual first packet, ns); callback durations; tasklet time; RX first-packet time; TP latency | log2 buckets + min/max/sum/count per window | owner-only, lock-free; window roll via seqlock or double buffer | seqlock retry snapshot |
| **Per-unit result** | TX: status, target TAI, actual first-packet TAI, lateness ns, epochs skipped. RX: status, per-port pkts, first/last arrival, TP meta, seq lost | carried with the frame on done/get, one struct for all media | filled in the tasklet | valid for the frame's lifetime |
| **Event, reliable** | state changes: link, PTP lock/GM, session recovering/recovered/failed, RX signal lost/restored, format detected/changed/failed, sched migrated, back-pressure begin/end | bounded MPSC ring; `mtl_event_poll()` or eventfd; seq number exposes overflow; optional control-thread callback | lock-free enqueue of a small POD; `events_lost++` on overflow | may block |
| **Event, lossy** | per-frame late/drop notices, vsync | same ring, lossy class (or rely on the per-unit result) | same | same |
| **Trace hook** | packet/frame lifecycle, pacing decisions, PTP messages | USDT (existing), optional static tracepoints | near-zero cost when detached | external tools |
| **Log** | human diagnostics only, never the only carrier of a fact | printer with `(instance, session, module, level)` | rate-limited; nothing at INFO or below in tasklets | — |

TX per-unit status values could be: SENT, SENT_LATE, DROPPED_LATE, ABORTED, FLUSHED_BY_RECOVERY.

Tasklet-safety requirements:

- Counters, gauges, histograms, per-unit results, and event enqueue **must be lock-free and wait-free in the tasklet**: no spinlock shared with app threads, no malloc, no log.
- Stats **reads must not take the tasklet's session lock**. Use relaxed per-field atomics for counters, and a seqlock for groups that must be coherent (histogram windows, PTP offset/path-delay pairs).
- App callbacks, if kept, run on a library control thread that drains the event ring. Today `notify_frame_late` and `notify_event` run inside the tasklet under the session spinlock (`st_tx_video_session.c:682`, `:4329`).
- Recovery work (queue re-acquire, mempool rebuild) moves off the tasklet. The tasklet only sets state and emits `SESSION_RECOVERING`.

Suggested uniform layout (same field names for all media types; size/version checked by `_Static_assert`):

- `session.tx`: frames_{submitted, sent, sent_late, dropped_late, aborted, flushed_by_recovery}, epochs_skipped, lateness_hist, early_margin_hist, build_overrun, user_not_ready, pkts/bytes[port], recoveries_{OK, failed}.
- `session.rx`: frames_{delivered, complete, reconstructed, corrupted_delivered, corrupted_discarded, dropped_backpressure}; pkts_{received[port], redundant, lost[port], unrecovered + exact/estimated flag, reordered[port], dup_same_port[port], rejected{pt, ssrc, len, interlace, …}}; arrival_hist; tp_{narrow, wide, failed}.
- `session.queue` gauges: per-state framebuffer counts, ring fill.
- `port`: link_up, speed, NIC counters. A library thread reads and caches these; the getter never touches HW. `tx_err` carries a "not supported on this driver" flag instead of a silent 0.
- `ptp` (per port): state (DISABLED / USER_SOURCE / ACQUIRING / LOCKED / HOLDOVER / LOST), offset_ns, path_delay_ns, last_sync_age_ns, gm_id.
- `sched` (per scheduler): busy %, avg loop ns, max tasklet time, sessions assigned.

---

## 6. Evidence index

- Public stats structs: `include/st_api.h:251-460`, `include/st20_api.h:1732-1828`, `include/st30_api.h:583-603`, `include/st40_api.h:552-567`, `include/st41_api.h:283-303`, `include/mtl_api.h:766-863`.
- Events: `include/st_api.h:208-222`. Firing sites: `st_tx_video_session.c:310,4240,4278,4311,4320,4329`, `st_rx_video_session.c:3488`.
- Lateness: `st_tx_video_session.c:620-686`, `st_tx_audio_session.c:270-320`, `st_tx_ancillary_session.c:380-435`, `pipeline/st20_pipeline_tx.c:100-172`.
- Stats dump: `mt_stat.c`, `st_tx_video_session.c:3556-3746,3939-3953`.
- Port stats: `dev/mt_dev.c:155-300,2603-2669`.
- PTP: `mt_ptp.c:505-560,598-619,740-760,1102-1121,1490-1545`; `mt_ptp.h:127-136`.
- Timing parser: `st_rx_timing_parser.c`, `st_rx_video_session.c:1539-1565,3409-3435`.
- Scheduler timing: `mt_sch.c:120-226,452-500`. Admin CPU busy and migration: `mt_admin.c:16-353`.
- USDT: `mt_usdt_provider.d`.
- Ecosystem consumers: nothing in `ecosystem/`, `plugins/`, `python/`, `rust/`, or `app/` calls `*_get_session_stats`, `mtl_get_port_stats`, `notify_event`, or `notify_frame_late` (`git grep` at HEAD). Only `tests/tools/RxTxApp` does **[verified]**.

---

## Open questions for the maintainer

1. **Should per-frame results replace most callbacks?** Today lateness, drops, and done status are split across `notify_frame_late`, `notify_frame_done`, counters, and USDT, with inconsistent units (F6) and a `priv` bug (F1).
  Options: (a) put one per-frame result on the frame from done/get, and keep counters only as aggregates; (b) keep callbacks but normalise them; (c) both. This decides whether apps can react frame by frame without code running in tasklet context.
2. **Where do app callbacks run?** Today they run in the tasklet under the session spinlock, so a slow app stalls pacing.
  Options: (a) keep tasklet callbacks with a strict non-blocking contract; (b) the library drains an event ring on a control thread and calls back from there; (c) no callbacks, only poll/eventfd. This trades error-reaction latency against pacing risk.
3. **What exactly is "on time" on TX?** Options: when the frame is handed over (put vs deadline); when the first packet hits the wire (needs a NIC TX timestamp or a TSC estimate at burst); or when TX completes (today's `done`). This decides which timestamp the per-frame result carries, and whether NIC TX timestamping is required.
4. **Reset semantics.** Today counters are resettable, and reset also rewrites the dump baselines.
  Options: (a) monotonic only, app-side baselines; (b) reset implemented as a library-side baseline subtraction, so concurrent readers never see torn zeros. This matters when several consumers read the same counters (app, dump, exporter).
5. **Event delivery guarantee and state resync.** Should state-change events (link, PTP, recovery, signal loss) be guaranteed, with overflow counted, or best-effort? Should every event have a "current state" getter so a late subscriber can resync? Without one, a missed edge leaves the app with wrong state (compare today's sticky `ptp->locked`).
6. **Runtime link and VF-reset scope.** HEAD registers no ethdev LSC/RESET callbacks. Should the API promise LINK_DOWN/UP and RESET events? And what happens to sessions: pause and resume, recreate, or report only? This decides whether session state must survive a port restart.
7. **PTP status contract.** Options: a per-port state machine (ACQUIRING / LOCKED / HOLDOVER / LOST) with offset, path delay, and GM identity, plus change events; or raw numbers only, with policy left to the app. What should be reported when the user supplies `ptp_get_time_fn`?
8. **Uniform schema across media types.** Is it acceptable to replace the per-type `st20/30/40/41_*_user_stats` layouts with one common schema plus a tagged media-specific tail? ST22/ST22p have nothing today. This is ABI churn against a consistent user experience.
9. **Queue-depth gauges and back-pressure events.** Should the API expose framebuffer state counts (free / app / queued / converting / in-flight) and ring fill, and emit BACKPRESSURE_BEGIN/END? This answers "how much buffering and latency am I carrying" directly, but the pipeline state machine would have to keep lock-free per-state counts.
10. **Estimated vs exact counters.** Video `stat_pkts_unrecovered` is an estimate, and video per-port loss appears one frame late. Should such fields be flagged (or given an `_est` name), or should the accounting be made exact? This matters for alarms and SLA reports.
11. **Timing parser as a first-class feature?** Today it is flag-gated, per-frame only, trustworthy only with HW timestamps, and forces the detector path.
  Options: an always-on cheap summary (narrow/wide/failed counters, latency histogram) with the per-frame detail opt-in; or keep it fully opt-in. Is a TX self-check (our own cinst/vrx) wanted?
12. **Logging contract.** Should the new printer carry `(instance, session, module, level)` and a user `priv`, be rate-limited, and be banned from tasklets at INFO and below (with a debug-build assert)? Should every log-only fact in §1.4 first get a structured home?
13. **Error returns.** Should one convention apply everywhere? That would mean negative errno throughout; create reporting a reason through an out-param or `mtl_last_error()`; and `get_frame` distinguishing timeout, not-ready, and session-failed. This decides whether apps can "manage all exceptions" without scraping logs.
