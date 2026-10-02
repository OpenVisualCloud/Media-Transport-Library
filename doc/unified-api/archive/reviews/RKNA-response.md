# Response to reviews RK, RN and RA (addenda K and N)

| | |
|---|---|
| Status | Designer's response, 2026-10-01 |
| Reviews | [RK](RK-kubernetes-review.md) (Kubernetes, 43 findings), [RN](RN-nmos-ipmx-review.md) (NMOS and IPMX, 34), [RA](RA-api-coherence-review.md) (API coherence and leanness, 41) |
| Result | Every finding is fixed except the ones marked **declined** or **partly**, each with its reason. `sketch/check.sh` passes, with a new all-headers probe (RA-33): 135 functions plus 8 under `MTL_LATER`, 33 in `mtl.h` |

Where two reviews disagreed, the choice is stated once in §1 and referred to in the tables.

## 1. Choices between the reviews

| Topic | RK | RA | RN | Chosen |
|---|---|---|---|---|
| fold `mtl_instance_shutdown` into close | fold, with a thread-local report | keep | — | **keep**: the flags (all references, drain) and the report need a call; close stays the one-argument path for programs |
| the unsafe close result | `-MTL_EIO` + `QUEUE_QUARANTINED` (RK-21) | `-MTL_ETIMEDOUT` (E13) | — | **`-MTL_EIO`**: ETIMEDOUT keeps its benign "drain ran out" meaning |
| memory under leases still out | pick one (RK-12) | freed after the last lease (E5) | — | **freed after the last lease**, by the next control-plane call or at exit, since a DP release cannot free; a GStreamer NULL → PLAYING cycle would otherwise leak each time |
| cancel results | — | 0 / 1 / `-MTL_EBUSY` (E3) | 0 when none pending (RN-28) | **RA**: 1 for "none pending" lets the Node tell it from a real cancel |
| `MTL_SENDER_IPMX` | — | renumber sender types with a DEFAULT (E2) | drop it (RN-23) | **drop it**: the profile gives N the TR-10-1 CMAX and VRX, so `MTL_SENDER_N = 0` keeps meaning N under both profiles |
| crypto parameters | — | options (E9) | changeable at an activation (RN-5) | **options, R keys**: both reviews are satisfied, and the extension chain goes |
| per-activation options | — | R keys in the config apply at the boundary (E7) | a new part `MTL_UPDATE_OPTIONS` (RN-3) | **RA**: no new part; R keys in the update's config apply at its boundary |

## 2. RK — Kubernetes

| ID | Disposition |
|---|---|
| RK-1 | fixed: 16 §10.3 set-ups A–C (non-root on a prepared node, root in the container, privileged); open names the missing piece; the pod spec uses A |
| RK-2 | fixed: the startup probe uses the liveness rule (16 §8, §10.4, ex11) |
| RK-3 | fixed: one SR-IOV resource per 2022-7 network (16 §7, §10.4) |
| RK-4 | fixed: MtlManager grants are returned after the schedulers and devices stop (16 §2.2 step 5; `mtl_instance_close`) |
| RK-5 | fixed: handle slots process-wide and never freed, state CLOSED_BY_INSTANCE (R4; 03 §2.2 note; EK20) |
| RK-6 | fixed: step 0 waits for threads inside data calls; SINGLE_READER sessions rely on the application joining first (16 §2.2) |
| RK-7 | fixed: AS state in the never-freed slot with an in-flight counter (16 §2.5; R8 "at any time") |
| RK-8 | fixed: R8 names DPDK's SIGBUS exception; DPDK's descriptors get `FD_CLOEXEC` after EAL init (EK15) |
| RK-9 | fixed: a `pthread_atfork` child handler closes the tracked descriptors (R8, 16 §2.5) |
| RK-10 | fixed: `SESSION_LOST` (liveness) only when a session has no leg left or every port is gone; otherwise `DEGRADED` |
| RK-11 | fixed: a bounded device step counts as worker progress |
| RK-12 | fixed: §1 choice; mtl.h, the report (`bytes_kept`) and 16 §2.3–2.4 now say the same |
| RK-13 | fixed: 1 covers held memory and stuck threads; `threads_unjoined` reported |
| RK-14 | fixed: `-MTL_EDEADLK` from library threads, mt not consumed (mtl.h, mtl_queue.h, the log sink) |
| RK-15 | fixed: step 3 delivers pending results to dispatch threads; `results_discarded` (EK21) |
| RK-16 | fixed: re-open rules in `mtl_instance_open`; 03 §7.1 note |
| RK-17 | fixed: a reset starts only if the budget remains; `caps.reset_budget_ns` |
| RK-18 | fixed: the budget counts from SIGTERM; ex11 records the time in the handler |
| RK-19 | fixed: DRAIN stops at the deadline minus the later steps |
| RK-20 | fixed: shutdown and FLUSH treat rows STALL as TRUNCATE |
| RK-21 | fixed: `-MTL_EIO`, §1 |
| RK-22 | fixed: one reference handle per shared open; others get `-MTL_ESHUTDOWN` after ALL_REFERENCES |
| RK-23 | fixed: health returns `-MTL_ESHUTDOWN` after close; ex11 maps it (liveness 200, readiness 503) |
| RK-24 | fixed: `SCHED_STALLED` masked during close; `instance.stall_ns` 1 s, events at 1/8, 1/4, 1/2 |
| RK-25 | fixed: flagged only when quota ÷ period is below the CPU count |
| RK-26 | fixed: `instance.housekeeping_cpus` removed; `instance.main_lcore` is the housekeeping CPU, first of the mask, never a scheduler |
| RK-27 | fixed: 04 §5.2 and §7.1 notes |
| RK-28 | fixed: `MTL_INSTANCE_MANAGER_OPTIONAL` removed; in a pod auto arbitration means none |
| RK-29 | fixed: EK19 |
| RK-30 | fixed: detect is a start condition; the application's `locked` is what reports grandmaster loss |
| RK-31 | fixed: the network-status helper moved under `MTL_LATER` with `-MTL_EAGAIN`; `env:VAR#n=ip/prefix` is the v1 form |
| RK-32 | fixed: `MTL_INSTANCE_PREFLIGHT` removed (16 §7) |
| RK-33 | fixed: ex11 blocks the signals before open |
| RK-34 | fixed: rows for timelines, codec plugins, queues; residual `tx_maxrate` (16 §3) |
| RK-35 | fixed: `strategy: Recreate` guidance (16 §10.4) |
| RK-36 | fixed: `NET_RAW` caveat and `SYS_NICE` for NUMA policy under RuntimeDefault (16 §10.3) |
| RK-37 | fixed: a consistent snapshot, detail "" if it changed (mtl_observe.h) |
| RK-38 | fixed: key ranges corrected; `instance.profile` is 2040 |
| RK-39 | fixed: `mac` ignored on input |
| RK-40 | fixed: timeout 0 = abort semantics; FOREVER bounded per step |
| RK-41 | fixed: `mtl_legacy.h` close semantics; LEARN.md updated |
| RK-42 | fixed: `port.igmp_report_ms` 10 s until a query is seen |
| RK-43 | fixed: memlock wording and `dma_entry_limit` (16 §9) |

## 3. RN — NMOS and IPMX

| ID | Disposition |
|---|---|
| RN-1 | fixed: the switch at the slot boundary by the clock, unit or not; RX by media time or local arrival (mtl.h update; 17 §2.2) |
| RN-2 | fixed: neighbours resolved after commit, not awaited |
| RN-3 | fixed: R options in the update's config apply at its boundary (§1) |
| RN-4 | fixed: ex13 is one update per PATCH; `applied_at` reports terminal states by seq |
| RN-5 | fixed: `crypto.*` options, R keys (§1) |
| RN-6 | fixed: the counter restarts only for unused key bytes |
| RN-7 | fixed: RX keys honour `when`; `MTL_EVENT_KEY_NEEDED`, `rx.crypto_unknown_key` |
| RN-8 | fixed: `MTL_SUBMIT_SENDER_TIME` |
| RN-9 | fixed: DRY_RUN leaves the status untouched and may later meet ENOSPC; ex13 reads `update_seq` after the call. **Partly**: the call does not return the seq, since a Node serialises the updates of one session |
| RN-10 | fixed: the Node keeps `master_enable`; MUTED is a transport state |
| RN-11 | fixed: 17 §2.2 (423; REPLACED is for non-NMOS callers) |
| RN-12 | fixed: RX REAPPLY re-sends reports without a leave |
| RN-13 | fixed: notes in 09 §7.1, §7.2, §9.3 and 07 §3.2 |
| RN-14 | fixed: colorimetry always rendered (mtl_format.h, mtl_sdp.h) |
| RN-15 | fixed: colorimetry, TCS and range change while running under MEDIA when no conversion uses them |
| RN-16 | fixed: `MTL_SDP_ONE_MLINE` removed; render always two m-lines |
| RN-17 | fixed: render covers privacy, extmap, IPMX keyword and TP, and bumps `o=` on grandmaster and Info Block changes. **Partly**: profile, level and PAR stay in `fmtp_extra` text |
| RN-18 | fixed: parse outputs the IPMX flag, ts-refclk, RTCP port, and the crypto options |
| RN-19 | fixed: parse reserves the legs it did not find |
| RN-20 | fixed: the per-essence schedule in mtl_rtcp.h, ANC included |
| RN-21 | fixed: the library builds 0x0001, 0x0002, 0x0004 and 0x0011; the rest are application bytes |
| RN-22 | fixed: per-unit blocks append and never bump the version |
| RN-23 | fixed: `MTL_SENDER_IPMX` dropped (§1); `video.vtotal` default as RN proposed |
| RN-24 | fixed: the k rule and DISCONTINUITY re-anchor in `MTL_MEDIA_SENDER` |
| RN-25 | fixed: RX classifies by local arrival time for SENDER or unlocked clocks |
| RN-26 | fixed: `tx.precede` is C, shares the queue, takes the video's RTP and media time |
| RN-27 | fixed: `time.freerun_slew_ppm` |
| RN-28 | fixed: render returns `-MTL_EBUSY`; cancel follows RA (§1) |
| RN-29 | fixed: `ts_refclk[64]`, `mediaclk[16]` with the 12-character wire limit |
| RN-30 | fixed: profile table completed; `MTL_FLOWF_DSCP_LITERAL` |
| RN-31 | fixed: `MTL_WAIT_RTCP` in mtl.h; key 2040; counts corrected. The time *state* FREERUN keeps its name: it now means exactly "running free", which is what the source of that name does |
| RN-32 | fixed: `tx.units_muted`; no reports while muted. Render works while muted, since an IS-05 transport file is still needed |
| RN-33 | fixed: planned = applied = now in CREATED and STOPPED; pending updates fail on a time step; the Node constrains `rtcp_*` |
| RN-34 | fixed: `mtl_crypto_clear_keys` merged into `set_key(NULL)`; the RFC 3550 mode and `rtcp.sr_interval_ms` dropped; `mtl_rtcp_mib_build` removed. **Declined**: `mtl_rtcp_mib_next` stays in `mtl_rtcp.h`, next to the format it parses |

## 4. RA — API coherence and leanness

| ID | Disposition |
|---|---|
| RA-1 | fixed: `enum mtl_rtcp_sr` removed (`rtcp.sr` is a bool); `struct mtl_rtcp_report`, `MTL_RTCP_RPT_*`; the all-headers probe in check.sh |
| RA-2 | fixed: `dscp` 0 = the profile's, `MTL_FLOWF_DSCP_LITERAL`; sender types unchanged (§1) |
| RA-3 | fixed: cancel as `parts` 0 with 0 / 1 / `-MTL_EBUSY` (`UPDATE_COMMITTING`) |
| RA-4 | fixed: one reference handle per shared open |
| RA-5 | fixed: §1 |
| RA-6 | fixed: the legs rules in mtl.h; 09 notes |
| RA-7 | fixed: §1 |
| RA-8 | fixed: `mtl_sdp_meta` strings are fixed arrays, copied and terminated |
| RA-9 | fixed: §1; `mtl_ext_hdr` and `mtl_ext_kind` removed, `next` reserved again |
| RA-10 | fixed: FREERUN and HOLDOVER are ready; `DEGRADED` is information only |
| RA-11 | fixed: ex13 rewritten as proposed |
| RA-12 | fixed: FLUSH and abort statuses; reason `ABORTED`; 03 §3.4 note |
| RA-13 | fixed: the close comment covers stuck threads, library-thread callers, re-open and queues and timelines |
| RA-14 | fixed: RN-14 |
| RA-15 | fixed: `enum mtl_shutdown_outcome` removed; `references_left` |
| RA-16 | fixed: `MTL_UPDATE_CANCEL` removed; the state enum keeps its `MTL_UPDATE_STATE_` prefix |
| RA-17 | fixed: render and `mtl_index_at` return `-MTL_EBUSY` |
| RA-18 | fixed: `mtl_port_get_spec` takes a size; `mac` ignored on input |
| RA-19 | fixed: RTCP, retransmission, encryption and profile keys in 1000–1039; `instance.profile` 2040 |
| RA-20 | fixed: `MTL_WAIT_RTCP` in mtl.h |
| RA-21 | fixed: RN-22 |
| RA-22 | fixed: one record per (kind, tag) |
| RA-23 | fixed: SENDER mode rules in mtl.h |
| RA-24 | fixed: `MTL_EVENT_GRANDMASTER`; the time-state rule in mtl.h |
| RA-25 | fixed: ex11 (initialised report, budget floor, one signal path, ALL_REFERENCES) |
| RA-26 | fixed: `mtl_port_specs_from_network_status` under `MTL_LATER` |
| RA-27 | fixed: `mtl_audio_remap` under `MTL_LATER` |
| RA-28 | fixed: RN-21 |
| RA-29 | fixed: `video.vtotal`, `video.htotal`, `cvideo.max_bitrate_bps` are options |
| RA-30 | fixed: `update_planned_tai_ns` removed (104 B); D-80 updated |
| RA-31 | fixed: planned in CREATED/STOPPED, DRY_RUN and seq rules in the update comment |
| RA-32 | fixed: `env:` ports and `MTL_PORTS` are `-MTL_EINVAL` in a set-uid process |
| RA-33 | fixed: the all-headers probe (C99 both orders, C++17) |
| RA-34 | fixed: the close comment says instance close flushes; close sessions first to drain |
| RA-35 | fixed: R4 extended |
| RA-36 | fixed: health snapshot statement |
| RA-37 | fixed |
| RA-38 | fixed |
| RA-39 | fixed: `MTL_CRYPTO_` prefix; the `uint8_t` fields explained in their comment |
| RA-40 | fixed: 16 §7 says "the first group past the budget" |
| RA-41 | fixed: `mtl_flow_parse` and `mtl_raster_parse` are AS |

## 5. What the reviews did not change

- The direction: one bounded network-first shutdown, crash-only rules, health for probes,
  CPUs and time from the pod, one atomic IS-05 update, optional headers for SDP, RTCP and
  encryption. All three reviews kept it.
- The core header: 33 functions. The reviews' simplifications removed two functions
  (`mtl_rtcp_mib_build`, `mtl_crypto_clear_keys`) and moved two under `MTL_LATER`
  (`mtl_port_specs_from_network_status`, `mtl_audio_remap`): 139 → 135.
