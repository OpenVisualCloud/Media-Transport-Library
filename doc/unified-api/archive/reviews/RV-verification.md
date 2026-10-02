# RV — Verification of the RKNA response against the headers, examples and documents

| | |
|---|---|
| Status | Verification, 2026-10-01. Read-only: no file other than this one was changed |
| Checked | every ID of [RK](RK-kubernetes-review.md) (43), [RN](RN-nmos-ipmx-review.md) (34) and [RA](RA-api-coherence-review.md) (41) against the dispositions of [RKNA](RKNA-response.md) |
| Against | `sketch/include/mtl/experimental/*.h`, `sketch/examples/ex01`, `ex11`, `ex13`, 16, 17, 03, 04, 07, 08, 09, 14, 15, DECISIONS, OPEN-QUESTIONS, LEARN, README, 00-summary, REVISION-4, sketch/README |
| Verdicts | **VERIFIED** fix present and resolves the finding; **INCOMPLETE** part of it missing; **MISSING** claimed, absent; **REGRESSION** the fix left or created an inconsistency |
| Line numbers | per file (`mtl.h:NNN` etc.) as of this run |

## 0. Result

| | RK | RN | RA | Total |
|---|---|---|---|---|
| VERIFIED | 35 | 23 | 38 | 96 |
| INCOMPLETE | 1 | 10 | 2 | 13 |
| MISSING | 0 | 0 | 0 | 0 |
| REGRESSION | 7 | 1 | 1 | 9 |

`sketch/check.sh`: **OK (201 compile runs, compilers: gcc clang g++ clang++)**.

- Per header: mtl_convert 3, mtl_crypto 1, mtl_debug 3, mtl_format 10, mtl.h 33, mtl_legacy 2, mtl_mem 12, mtl_observe 16, mtl_options 6, mtl_packet 0, mtl_plugin 3, mtl_queue 12.
- Per header, continued: mtl_reasons 0, mtl_rtcp 3, mtl_sdp 2, mtl_sync 15, mtl_util 14.
- Total 135, and 8 under `MTL_LATER` (mem 3, util 3, queue 1, convert 1). 17 headers.
- By call class: CP 58, DP 19, DPC 7, WT 14, AS 37.

Most fixes are really in the headers, and no claimed fix is missing. The regressions are mostly stale text. A removed name is still used, an error-code comment no longer matches the new close contract, or two headers give different thresholds.

The INCOMPLETE ones are mostly RN items whose IPMX detail is only partly covered: the privacy triple, the audio SR mid-unit NTP, InfoFrames with SENDER video, and render while muted.

## 1. RK — Kubernetes

| ID | Verdict | Evidence | What is missing |
|---|---|---|---|
| RK-1 | VERIFIED | 16 §10.3 set-ups A–C (16:473-487); pod spec `runAsUser/runAsGroup` 16:512-515; reasons 16:327, 330 | — |
| RK-2 | VERIFIED | `startupProbe` on `/live` 16:533; 16:398-400; ex11 `probe_status` 66-72 (startup uses the liveness mask) | — |
| RK-3 | VERIFIED | 16:354-358, pod `e810_red`/`e810_blue` 16:509, 521-527 | — |
| RK-4 | VERIFIED | mtl.h:471-473 (schedulers and devices stop, then grants); 16 §2.2 steps 4–5 (16:107-108, 121-122); 16:452-453 | — |
| RK-5 | VERIFIED | R4 mtl.h:53-56; 03 §2.2 addendum 03:100-103; EK20 16:579; 16 §2.1 16:76-81 | — |
| RK-6 | VERIFIED | mtl.h:468-469 ("those inside one are waited for"); 16 step 0 16:117 incl. SINGLE_READER rule | — |
| RK-7 | VERIFIED | R8 mtl.h:73-74 ("at any time, also during and after close"); 16 §2.5 16:189-195; abort mtl.h:489-490 | — |
| RK-8 | REGRESSION | R8 mtl.h:72-75; 16:185-188, 202-204; EK15 16:574 | R8 and 16 §1 rule 5 / §2.5 call the heap-growth SIGBUS handler "the one exception", but `instance.hotplug` (mtl_options.h:186-187, 16:200-201) installs DPDK's process-wide SIGBUS handler: a second exception. Also FD_CLOEXEC "after EAL init" misses memfds DPDK creates on later heap growth at create |
| RK-9 | VERIFIED | R8 mtl.h:75-79; 16:205-213; EK15 16:574; reason FORKED mtl_reasons.h:28 | — |
| RK-10 | REGRESSION | mtl_observe.h:198-199, 210-212; 16 §4 16:272; 16 §8 16:380, 387 | 16 §3 row "Instance threads" (16:227) still says "health `PORT_REMOVED`"; PORT_REMOVED is now only an event, the health bit is `DEGRADED` |
| RK-11 | VERIFIED | mtl_observe.h:196-197; 16:271, 379; EK11 16:570 | — |
| RK-12 | VERIFIED | mtl.h:475-477; mtl_observe.h:264 `bytes_kept`; 16:154, 178-181; D-89 DECISIONS:105; Q-K8S-2 16:617 | — |
| RK-13 | VERIFIED | mtl.h:473-475 (1 includes a thread stuck in application code); mtl_observe.h:262; 16:154 | — |
| RK-14 | REGRESSION | mtl.h:480-481; mtl_observe.h:268-269, 292-293; mtl_queue.h:136-138 | `MTL_EDEADLK` is still defined as "not allowed from a library busy-loop thread" (mtl.h:159), R6 limits it to busy-loop threads (mtl.h:68-70), and the only reason is `BUSY_LOOP_THREAD` (mtl_reasons.h:71); no reason is named for the dispatch/log/codec case |
| RK-15 | VERIFIED | mtl.h:470-471; 16 step 3 16:120; `results_discarded` mtl_observe.h:257; EK21 16:580 | — |
| RK-16 | VERIFIED | mtl.h:460-463; 03 §7.1 addendum 03:713-717; 16:168-170 | — |
| RK-17 | VERIFIED | 16 step 4 16:121; `caps.reset_budget_ns` 08:470; EK1 16:560 | — |
| RK-18 | VERIFIED | ex11 17-26 (`clock_gettime` in the handler), 76-82; 16:138-143 | Nit: if `shutdown_all` runs without a signal, `g_term` is zero and the budget collapses to the 100 ms floor |
| RK-19 | VERIFIED | mtl_observe.h:246-247; 16:118 | — |
| RK-20 | VERIFIED | mtl.h:304-306 (STALL as TRUNCATE); 16:118; 03:307-310 | — |
| RK-21 | REGRESSION | mtl.h:477-478 `-MTL_EIO (QUEUE_QUARANTINED)`; 16:155-158 | R2 (mtl.h:40-41) and `MTL_ETIMEDOUT` (mtl.h:162) still say "(stop, close)", though no close returns it now (also LEARN:308); `MTL_EIO` (mtl.h:149) is defined as "the session failed (ERROR)" under "one meaning each", yet instance close now returns it for a quarantined port |
| RK-22 | VERIFIED | mtl.h:453-455; mtl_observe.h:244-245, 250, 267-268; 16:70-75 | — |
| RK-23 | VERIFIED | mtl_observe.h:239-240; ex11 68 (ESHUTDOWN: liveness 200, readiness 503); 16:373-374, 402 | Nit: R4 (mtl.h:52-53) says a stale handle is `-MTL_EBADF`; the consumed instance handle answering `-MTL_ESHUTDOWN` is not stated as an exception there |
| RK-24 | REGRESSION | mtl_observe.h:239 (SCHED_STALLED masked); `instance.stall_ns` 1 s, events at 1/8, 1/4, 1/2 (mtl_options.h:175-176; 16:378) | `MTL_EVENT_SCHED_STALLED` still says "at 1x, 2x, 4x the limit" (mtl_queue.h:51), contradicting mtl_options.h and 16 §8 |
| RK-25 | VERIFIED | mtl_options.h:180-181; 16:287; EK17 16:576 | — |
| RK-26 | INCOMPLETE | `housekeeping_cpus` gone; `instance.main_lcore` mtl_options.h:162-164; 16:286 (placement in `sched.lcore`, 08:426) | `instance.sys_lcore` (mtl_options.h:161, `MTL_SYS_SHARED/DEDICATED`) still overlaps `main_lcore`; RK asked for one knob, and its relation to `main_lcore` is unstated |
| RK-27 | VERIFIED | 04 §5.2 addendum 04:466-470; 04 §7.1 addendum 04:603-604 | — |
| RK-28 | REGRESSION | flag gone from mtl.h; 16:285; D-92 DECISIONS:108 | The removed `MTL_INSTANCE_MANAGER_OPTIONAL` is still used as live API at 15:158-160, 15:196, 07:708, 03:779 and 14:244 (R-6). Auto still picks MtlManager whenever its socket exists (mtl_options.h:177-179), so a pod that mounts it for AF_XDP gets manager CPU arbitration, against K-REQ-8 |
| RK-29 | VERIFIED | EK19 16:578; 16:461-462 | Note: 14 tracks only "EK1–EK18" (14:167, 14:335) although 16 §11 says the EK items are tracked in 14 |
| RK-30 | VERIFIED | mtl_options.h:231-233; mtl_sync.h:124-125; 16:301-305 | — |
| RK-31 | VERIFIED | mtl_util.h:63-74 (under `MTL_LATER`, `-MTL_EAGAIN` PORT_ENV_UNSET); `env:VAR#n=ip/prefix` mtl.h:447-448; 16:358-361 | — |
| RK-32 | REGRESSION | flag gone from mtl.h; 16:338-340; D-91 DECISIONS:107 | mtl_reasons.h:120 still says "(open and PREFLIGHT, 16 §7)"; 14:325 lists "preflight" as a deliverable |
| RK-33 | VERIFIED | ex11 28-46 (block before open, install, unblock); 16:196-199 | — |
| RK-34 | VERIFIED | 16 §3 rows Queues, Timelines, Codec plugins 16:234-236; `tx_maxrate` 16:241, residual 3 16:251-253 | — |
| RK-35 | VERIFIED | 16:546-549 | — |
| RK-36 | VERIFIED | 16:492 (`NET_RAW` caveat), 16:496-499 (`SYS_NICE`) | — |
| RK-37 | VERIFIED | mtl_observe.h:233-234, 237-239; 16:370-372 | — |
| RK-38 | VERIFIED | `instance.profile` = 2040 mtl_options.h:190-191; 2020-2039 block mtl_options.h:174; 16 header row 16:9 | — |
| RK-39 | VERIFIED | mtl.h:408 "ignored on input" | — |
| RK-40 | VERIFIED | mtl.h:479-480; 16:85-86 | — |
| RK-41 | VERIFIED | mtl_legacy.h:22-25; LEARN:59, 225 | — |
| RK-42 | VERIFIED | mtl_options.h:220-221 (10000 until a query is seen) | — |
| RK-43 | VERIFIED | 16 §9 16:420-424 (user namespace wording, `dma_entry_limit`) | — |

## 2. RN — NMOS and IPMX

| ID | Verdict | Evidence | What is missing |
|---|---|---|---|
| RN-1 | VERIFIED | mtl.h:876-879; 17:74-78; 09 §7.1 addendum 09:406-412 | — |
| RN-2 | VERIFIED | mtl.h:869-870; 17:78-81; 09:408-409 | — |
| RN-3 | VERIFIED | mtl.h:872-875; mtl_options.h:15-17; 17:97 | — |
| RN-4 | VERIFIED | ex13 13-36 (one update, FLOWS\|LEGS\|REAPPLY, active config), 40-51 (terminal states by seq) | — |
| RN-5 | VERIFIED | `crypto.*` R keys mtl_options.h:85-96; mtl_crypto.h:6-10; 17:351-355 | — |
| RN-6 | VERIFIED | mtl_crypto.h:46-48; 17:368-370 | — |
| RN-7 | VERIFIED | mtl_crypto.h:48-51; `MTL_EVENT_KEY_NEEDED` mtl_queue.h:59; `rx.crypto_unknown_key{version}` 08:476 | — |
| RN-8 | VERIFIED | `MTL_SUBMIT_SENDER_TIME` mtl.h:969-971; 17:302 | — |
| RN-9 | INCOMPLETE | DRY_RUN mtl.h:866-868; seq rule mtl.h:886, 946; ex13 33-34 | Acknowledged **partly**: the call still returns no seq, so matching relies on the Node serialising; cancel's effect on `update_state` (CANCELLED) is implied by the enum, not stated in the update comment |
| RN-10 | VERIFIED | mtl.h:921-923; 17:99-101; ex13 header comment | — |
| RN-11 | VERIFIED | mtl.h:879-880; 17:88 | — |
| RN-12 | VERIFIED | mtl.h:863-865; 17:87 | Staggered legs and an opt-in leave+join were not taken (not required) |
| RN-13 | VERIFIED | 09:406-412, 09:484-487, 09:794-795, 07:361-364; 17:452-454 | — |
| RN-14 | VERIFIED | mtl_format.h:51-54; mtl.h:578; mtl_sdp.h:10; 17:55 | — |
| RN-15 | INCOMPLETE | mtl.h:575-577, 859-860; 17:96 | 17 says colorimetry/TCS/range "change with the activation", but mtl.h:875 applies `when` only to FLOWS, LEGS and R options; whether a MEDIA part in the same update lands at the same boundary is unstated. PAR is not covered (it stays in `fmtp_extra`) |
| RN-16 | VERIFIED | `MTL_SDP_ONE_MLINE` gone; mtl_sdp.h:8-9, 53-54; 17:128-129 | — |
| RN-17 | INCOMPLETE | mtl_sdp.h:10-13, 41-42; 17:127-135 | Acknowledged **partly**. Also: "render covers privacy" is overstated. The `crypto.*` options (mtl_options.h:86-96) have no key_generator, key_id or key_version, so `a=privacy` cannot be complete. `a=infoframe` and measured values are free text only |
| RN-18 | INCOMPLETE | `MTL_SDP_IPMX` mtl_sdp.h:27; `ts_refclk`, `rtcp_port` mtl_sdp.h:40, 43; crypto into `spec_options` mtl_sdp.h:53 | The privacy triple (key_generator, key_version, key_id) parsed from `a=privacy` has no destination. Profile, level, PAR and DID_SDID come back only as `fmtp_extra` text |
| RN-19 | VERIFIED | mtl_sdp.h:55-56; 17:93, 137-138 | — |
| RN-20 | INCOMPLETE | mtl_rtcp.h:9-12; 17:212-219 | The per-essence schedule is there, ANC included. The SENDER-audio half is not: a report sent every int(10 ms/ptime) packets falls mid-unit, but "NTP field = the unit's media time" (mtl_rtcp.h:13) gives no NTP for the packet it precedes. No `unit_samples` alignment rule either |
| RN-21 | INCOMPLETE | mtl_rtcp.h:14-17, 34-50; 17:227-238 | The library builds 0x0011 "from the crypto.* options", but no option carries privacy_version (or the key identity), which RN said 0x0011 needs |
| RN-22 | VERIFIED | mtl.h:1027-1028; mtl_rtcp.h:51-54; 17:240-245 | — |
| RN-23 | REGRESSION | `MTL_SENDER_IPMX` gone (mtl.h:557-562); `video.vtotal` default mtl_options.h:114-115; 17:256-259 | 14:332 still lists `MTL_SENDER_IPMX` as a phase-3 deliverable |
| RN-24 | INCOMPLETE | mtl.h:272-279; 17:313-319 | The k rule and the DISCONTINUITY re-anchor are in. Units in flight across an AUTO time step (RN's third point) are not addressed; 17:307-308 only covers the timeline step policy and pending updates |
| RN-25 | VERIFIED | mtl.h:877-878; 17:76-77 | — |
| RN-26 | INCOMPLETE | `tx.precede` C, shared queue, video's RTP and M (mtl_options.h:46-49); 17:271-275 | "Update with its video" became two updates with the same `when` (17:274-275): not atomic, so one can fail alone. `a=infoframe` is not rendered. A SENDER-mode video cannot be a precede target (mtl.h:278-279; 17:320-321), so an async HDMI source cannot carry InfoFrames (§4, R-1) |
| RN-27 | VERIFIED | `time.freerun_slew_ppm` mtl_options.h:239-240; 17:299 | — |
| RN-28 | VERIFIED | render `-MTL_EBUSY` mtl_sdp.h:48-50; cancel 0/1/EBUSY mtl.h:880-882 | — |
| RN-29 | VERIFIED | mtl_rtcp.h:40-41, 55 | Nit: a 64-character ts-refclk leaves no room for the NUL in `char[64]`; say whether the field is NUL-padded or limit it to 63 |
| RN-30 | INCOMPLETE | 17 §3.1 DSCP row and checks 17:197, 200; `MTL_FLOWF_DSCP_LITERAL` mtl.h:496 | PTP's EF and DHCP DNS/domain for in-band discovery (RN-30) are not in the profile table |
| RN-31 | VERIFIED | `MTL_WAIT_RTCP` mtl.h:1120; key 2040; 17:45-46 (7 + 2, 135, 8) | The FREERUN rename was declined, with a reason |
| RN-32 | INCOMPLETE | `tx.units_muted`, no reports while muted: mtl.h:712-716, mtl_rtcp.h:12, 08:475 | The response says render works while muted, but mtl_sdp.h:8-9 (and 17:128) render "every enabled leg"; muted means no leg is enabled, so render writes no m-line. Not stated anywhere |
| RN-33 | VERIFIED | mtl.h:882-883, 885; reason TIME_STEP mtl_reasons.h:97; 17:114-117 | — |
| RN-34 | VERIFIED | `set_key(NULL)` mtl_crypto.h:44-45; `rtcp.sr` bool mtl_options.h:81; no `sr_interval_ms`, no `mtl_rtcp_mib_build`; 17:206-208 | The `mib_next` move was declined, in line with RA |

## 3. RA — API coherence

| ID | Verdict | Evidence | What is missing |
|---|---|---|---|
| RA-1 | VERIFIED | `struct mtl_rtcp_report`, `MTL_RTCP_RPT_*` mtl_rtcp.h:60-74; `rtcp.sr` bool; all-headers probe check.sh | — |
| RA-2 | VERIFIED | `dscp` mtl.h:507; `MTL_FLOWF_DSCP_LITERAL` mtl.h:496; sender types mtl.h:557-562 (N = 0 under both profiles) | — |
| RA-3 | VERIFIED | mtl.h:880-882; `MTL_REASON_UPDATE_COMMITTING` mtl_reasons.h:29 | — |
| RA-4 | VERIFIED | mtl.h:453-455; 16:70-71 | — |
| RA-5 | VERIFIED | R4 mtl.h:53-56; mtl.h:475-477; `bytes_kept` mtl_observe.h:264; 16:154, 178-181 | — |
| RA-6 | VERIFIED | mtl.h:498-502, 709, 712-716, 858; 09:484-487, 794-795 | — |
| RA-7 | VERIFIED | mtl.h:872-875 | — |
| RA-8 | VERIFIED | mtl_sdp.h:28-46 (fixed arrays, copied, NUL-terminated, 416 B) | — |
| RA-9 | VERIFIED | no `mtl_ext_*` in any header; `next` reserved mtl.h:727; crypto options mtl_options.h:85-96 | — |
| RA-10 | VERIFIED | mtl_observe.h:205-212 (`DEGRADED` outside every mask); 16:384, 387; 17:178-181 | — |
| RA-11 | VERIFIED | ex13 13-51 matches RA §4.3 | — |
| RA-12 | VERIFIED | mtl.h:304-306, 487-490; `MTL_REASON_ABORTED` mtl_reasons.h:119; 03:307-310 | — |
| RA-13 | VERIFIED | mtl.h:467-482 (stuck threads, library-thread callers, objects closed, re-open at mtl.h:460-463) | — |
| RA-14 | VERIFIED | mtl_format.h:51-54 (rendered, FULLPROTECT with BT2100 `-MTL_EINVAL`) | — |
| RA-15 | VERIFIED | no `mtl_shutdown_outcome`; `references_left` mtl_observe.h:250; 200 B mtl_observe.h:315, 16:160 | — |
| RA-16 | REGRESSION | `MTL_UPDATE_CANCEL` gone from mtl.h; `MTL_UPDATE_STATE_*` kept | 14:329 still lists `CANCEL` beside `REAPPLY` and `DRY_RUN` as a deliverable |
| RA-17 | VERIFIED | mtl_sdp.h:48-50; `mtl_index_at` mtl_sync.h:72-74 | — |
| RA-18 | VERIFIED | `mtl_port_get_spec(..., size_t size)` mtl_observe.h:283-284; mtl.h:408 | — |
| RA-19 | VERIFIED | mtl_options.h:73-99 (1000–1039), 190-191 (2040); 16:9 | — |
| RA-20 | VERIFIED | mtl.h:1120; mtl_rtcp.h:20 | — |
| RA-21 | VERIFIED | mtl.h:1027-1028; mtl_rtcp.h:51-54 | — |
| RA-22 | VERIFIED | mtl.h:1019 "one record per (kind, tag)" | — |
| RA-23 | INCOMPLETE | mtl.h:272-279 (k, AT_INDEX, RX_BY_INDEX, `tx.precede`, overlap) | `mtl_index_at` on a SENDER session (mtl_sync.h:72-74 says nothing) and the `media_index` that results carry are still unstated, though E23 named both |
| RA-24 | VERIFIED | `MTL_EVENT_GRANDMASTER` mtl_queue.h:55; time-state rule mtl.h:336-338 | — |
| RA-25 | VERIFIED | ex11 83 (`memset`), 81-82 (floor), 17-26 and 31-46 (one signal path), 85 (ALL_REFERENCES) | — |
| RA-26 | VERIFIED | mtl_util.h:63-74 | — |
| RA-27 | VERIFIED | mtl_convert.h:56-64 | — |
| RA-28 | VERIFIED | no `mtl_rtcp_media_desc`/`mib_build`; `par_*`, `measured_rate_milli`, `channel_order`, `MTL_RTCP_MIB_APP_ONLY` mtl_rtcp.h:36-49 | — |
| RA-29 | VERIFIED | options 505, 506, 515 mtl_options.h:114-116, 122-123; config fields reserved mtl.h:583, 607 (96 B) | — |
| RA-30 | VERIFIED | status 104 B mtl.h:932-948, 1160; D-80 DECISIONS:96 | — |
| RA-31 | INCOMPLETE | mtl.h:866-868, 885-886 | ARMED (E3's "a `when` before T0 means T0") is still unaddressed |
| RA-32 | VERIFIED | mtl.h:452 | — |
| RA-33 | VERIFIED | check.sh "all headers" and "reversed" C99, C++17; passes | — |
| RA-34 | VERIFIED | mtl.h:469-470; 16:87-88 | — |
| RA-35 | VERIFIED | R4 mtl.h:53-56 | — |
| RA-36 | VERIFIED | mtl_observe.h:237-239; type-stable slots R4 | — |
| RA-37 | VERIFIED | mtl.h:68 now has its asterisk | Nit: it is indented 3 spaces where the R6 continuation lines use 5 |
| RA-38 | VERIFIED | ex01 24-25 (`k++` only after an acquire), 30 (label "mtl") | — |
| RA-39 | VERIFIED | `MTL_CRYPTO_AES*` mtl_crypto.h:35-42; `uint8_t` note mtl.h:575-576 | — |
| RA-40 | VERIFIED | 16:345-347 | — |
| RA-41 | VERIFIED | `mtl_flow_parse`, `mtl_raster_parse` AS mtl_util.h:38-43 | — |

## 4. Regressions and new inconsistencies

| # | Where | Inconsistency | Fix |
|---|---|---|---|
| R-1 | mtl.h:278-279, 17:320-321 against mtl_options.h:46-49, 17:271-275 | A SENDER session cannot be a `tx.precede` target, yet precede now copies the video's RTP and M, which would work for it. So IPMX InfoFrames (TR-10-10) for an async HDMI source (I1 §5.3) cannot be built | Allow a SENDER target, or state in 17 §3.3 that the case is unsupported |
| R-2 | mtl_queue.h:51 against mtl_options.h:175-176 and 16:378 | SCHED_STALLED events at "1x, 2x, 4x the limit" against "1/8, 1/4, 1/2" (RK-24) | Fix mtl_queue.h:51 |
| R-3 | mtl.h:40-41, 149, 159, 162; mtl_reasons.h:71; LEARN:308 | `MTL_ETIMEDOUT` "(stop, close)" though no close returns it; `MTL_EIO` "session failed" now also instance quarantine; `MTL_EDEADLK` "busy-loop thread" now also dispatch/log/codec threads with no reason code (RK-14, RK-21) | Reword the three error comments and R2/R6; name a reason for the library-thread case |
| R-4 | R8 mtl.h:72-73, 16:46, 16:185-188 against mtl_options.h:186-187, 16:200-201 | "The one exception" to "no signal handler" against `instance.hotplug`, which installs DPDK's SIGBUS handler (RK-8) | R8: "and, with `instance.hotplug`, DPDK's hotplug handler" |
| R-5 | mtl_reasons.h:120; 14:325 | `PREFLIGHT` still named after its removal (RK-32) | Drop it |
| R-6 | 15:158-160, 15:196, 07:708, 03:779, 14:244 | `MTL_INSTANCE_MANAGER_OPTIONAL` still used as live API (RK-28); 15's addendum (15:187-190) does not cover the name | Mark or rewrite those rows |
| R-7 | 14:332; 14:329 | `MTL_SENDER_IPMX` and `CANCEL` still listed as deliverables (RN-23, RA-16) | Update the roadmap rows |
| R-8 | 16:227 | "health `PORT_REMOVED`" is not a health bit any more (RK-10) | "health `DEGRADED` or `SESSION_LOST`" |
| R-9 | mtl_sdp.h:8-9, 17:128 against RKNA RN-32 | "every enabled leg" leaves a muted session with no m-line; the response says render works while muted | State which legs a muted render writes |
| R-10 | 10-api-sketch.md:14-31 | Stale header table: mtl.h 32 (33), mtl_sync 14 (15), mtl_observe 14 (16), util "+2 later" (+3), no rows for mtl_rtcp.h, mtl_crypto.h, mtl_sdp.h, "126 ... 6 more" (135, 8). 10:39 "the library never calls the application" contradicts R6 | Regenerate the table (the header-wins rule) |
| R-11 | REVISION-4.md:291, 297 | "118 issues, three of them critical": the reviews have four Critical (RK-1, RK-5, RK-7, RN-1) and one Blocker (RA-1) | "four critical and one blocker" |
| R-12 | 14:167, 14:335 against 16:553-555 | 16 §11 says EK items are tracked in 14, but 14 names only EK1–EK18; EK19–EK21 are missing | Extend 14 |

## 5. Name sweep

The task's removed names were searched in every header and example, sketch/README, 16, 17, OPEN-QUESTIONS, DECISIONS and 08:

- `MTL_INSTANCE_PREFLIGHT`, `MTL_INSTANCE_MANAGER_OPTIONAL`, `MTL_SENDER_IPMX`, `MTL_UPDATE_CANCEL`, `mtl_ext_hdr`, `MTL_EXT_CRYPTO`, `MTL_EVENT_TIME_SOURCE`, `mtl_rtcp_mib_build`;
- `mtl_crypto_clear_keys`, `struct mtl_rtcp_sr`, `housekeeping_cpus`, `update_planned_tai_ns`, `MTL_SHUTDOWN_RETIRED`, `rtcp.sr_interval_ms`, `MTL_RTCP_SR_IPMX`;
- also `mtl_ext_kind`, `mtl_ext_crypto`, `mtl_rtcp_media_desc`, `mtl_shutdown_outcome`, `MTL_SDP_ONE_MLINE`, `bytes_kept_until_exit`, `MTL_HEALTH_SESSION_ERROR`/`FLOW_PENDING`.

What the search found:

- **Live reference:** one, `PREFLIGHT` at mtl_reasons.h:120 (R-5).
- **Historical "removed" mentions, which are fine:** DECISIONS:79 and :108, 16:618, OPEN-QUESTIONS:1673.
- **Outside that set:** REVISION-4:310 names the old tag clash, historically, which is fine. The stale uses are those in R-5, R-6 and R-7.

## 6. Numbers

| Claim | Where | check.sh | Verdict |
|---|---|---|---|
| `mtl.h` 33 functions | README:28, 00-summary:38, REVISION-4 §8 :302, 17:46, RKNA header | 33 | match |
| 135 total | README:29, 00-summary:39, REVISION-4:303, 17:46, RKNA | 135 | match |
| 8 under `MTL_LATER` | REVISION-4:303, 17:46, RKNA | 8 | match |
| 17 headers (16 optional) | README:28-29, :81, 00-summary:38, REVISION-4:301, sketch/README:17 | 17 | match |
| 7 NMOS/IPMX + 2 Kubernetes functions | 17:45 | rtcp 3 + crypto 1 + sdp 2 + sync 1; observe 2 | match |
| 126 before the addenda, 139 → 135 | 00-summary:39, RKNA §5 | 126 + 9 = 135 | match |
| 13 optional headers | README:16 (the revision-4 paragraph) | — | historical, but reads as current next to "16" two paragraphs later |
| 10-api-sketch §1 table | 10:14-31 | — | **stale** (R-10) |
| struct sizes: status 104, report 200, health 152, sdp_meta 416, rtcp_info 144, rtcp_report 48 | mtl.h:1160, mtl_observe.h:314-315, mtl_sdp.h:61, mtl_rtcp.h:84-85; 16:160; D-80 | compile-checked | match |
