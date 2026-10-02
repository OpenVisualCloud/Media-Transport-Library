# Side findings — defects and documentation drift found during the research

| | |
|---|---|
| Status | Working notes for follow-up — revision 3 (after reviews C1–C5); SF-56…SF-68 added by the revision-4 addenda (2026-10-01). **Nothing here is fixed on `main` yet**, but open PR #1770 fixes or partly fixes about forty rows; each row now has a Status column |
| Date | 2026-09-30 |
| Baseline | `main` @ `545a266a`. Status checked against PR #1770 (`fix/side-findings`, open, head `74b9991d`, 38 commits, +3208/−371 in 132 files) by reading its diff and commit messages. Part of the revision-1 research read the working tree, which then had unrelated uncommitted edits; revision 2 re-pinned every `path:line` to HEAD (review C4 §5) |

These were found while researching the new API. They exist independently of it and are
worth fixing (or confirming) on their own, most of them before Phase 1
([14 §1.2](14-implementation-roadmap.md)). Labels as in the research notes: **[verified]**
read in code; **[inferred]** follows from code but was not executed — each inferred item
needs a reproducing test first. The source note is given as `Rnn`.

Revision 2 said "nothing here has been fixed". Review C5 pointed out that PR #1770 already
addresses many rows `[C5 §1.10]`. The **Status** column now reads:

- **fix in open PR #1770** — the PR's diff changes the cited code as the row asks (not
  merged, so not "fixed");
- **partly in open PR #1770** — the PR covers part of the row; the rest is still open;
- **open** — nothing in flight.

PR #1770 also fixes defects not listed here (for example `tv_update_dst` overwriting the
destination UDP port with the source port, and a misleading `st20_tx_set_ext_frame`
warning).

## 1. Library correctness — verified

| ID | Finding | Evidence | Note | Suggested test | Status |
|---|---|---|---|---|---|
| SF-01 | `st30p_tx_create` tests `ST30P_RX_FLAG_FORCE_NUMA` (bit 2) on TX ops, so TX FORCE_NUMA does not apply to the pipeline context and an unused TX bit triggers it | `lib/src/st2110/pipeline/st30_pipeline_tx.c:671` | R02 §4.4 #18 | unit | fix in open PR #1770 |
| SF-02 | Pipeline `notify_frame_late` receives the internal pipeline context as `priv` on the transport-late path (st20p, st30p, st40p); ST22p never forwards transport lateness | `st20_pipeline_tx.c:421`, `:462` → `st_tx_video_session.c:683`; `st30_pipeline_tx.c:256`, `:283`; `st40_pipeline_tx.c:281`, `:316` | R06 F1 | unit (UB) | fix in open PR #1770 (incl. ST22 forwarding) |
| SF-03 | ST41 USER_TIMESTAMP checks `frame->ta_meta.tfmt` (the audio union member) instead of `tf_meta.tfmt`, so the flag practically never applies | `st_tx_fastmetadata_session.c:769-772`; `st_header.h:154-161` | R05 F14 | unit | fix in open PR #1770 |
| SF-04 | ST41 USER_PACING with MEDIA_CLK converts zero-based (a 1970 time) instead of unwrapping | `st_tx_fastmetadata_session.c:252` | R05 F13 | unit | fix in open PR #1770 (rejects MEDIA_CLK and falls back to epoch pacing, as ST30/ST40) |
| SF-05 | TX extbuf free callback calls `notify_frame_done` *before* decrementing `refcnt` and clearing `addr/iova`; st20p already set the slot FREE, so an app re-arming the slot from the callback can get `-EIO` or have its new address overwritten with NULL | `st_tx_video_session.c:116-141`; `st20_pipeline_tx.c:264-276` | R04 §6 #2 (race **[inferred]**) | UB stress | fix in open PR #1770 |
| SF-06 | `mt_map_add` overlap check misses a new range that strictly encloses an existing one; comment says "1M", value is 64 KiB | `lib/src/mt_dma.c:32-41`, `:21` | R04 §4.3 | unit | fix in open PR #1770 |
| SF-07 | `mtl_dma_map` maps only `MTL_PORT_P`; port R and the DMA engine work only because of the shared VFIO default container | `lib/src/mt_main.c:864-865` | R04 §4.2 | integration | open (the region design, 05, replaces it) |
| SF-08 | TX builder overwrites `pending` per session instead of summing, so with `MTL_FLAG_TASKLET_SLEEP` a scheduler can sleep while earlier sessions have work | `st_tx_video_session.c:2694-2698` | R03 §4.1 | unit | fix in open PR #1770 |
| SF-09 | `mt_pacing_train_bps_result_search(impl, i, …)` passes the session port index where the physical port is expected | `st_tx_video_session.c:2758` (compare `:4270`) | R05 F23 | unit | fix in open PR #1770 |
| SF-10 | ST30 `sync_pacing` computes `rtp_time_stamp` (incl. delta) and it is immediately overwritten (dead code) | `st_tx_audio_session.c:334-354` vs `:395` | R05 F10 | review | open |
| SF-11 | ST22 invalid codestream size fires `notify_frame_done` as if the frame were sent | `st_tx_video_session.c:2468-2474` | R02 §4.3 #13 | unit | open |
| SF-12 | TX recovery reports in-flight frames as `COMPLETE` and counts them in `stat_frames_sent`; recovery runs in the tasklet (pthread mutex, mempool re-create, INFO logs) | `st_tx_video_session.c:4231-4332`, `:4290-4300` → `st20_pipeline_tx.c:276-290` | R06 E1, F2; R03 H4 | UB | open (engines track R1) |
| SF-13 | Same-session stats getter called from inside a callback spins forever on the non-recursive session spinlock | `st_tx_video_session.c:2682` + `:4760`; `st20_pipeline_tx.c:1311` | R03 H2 (outcome **[inferred]**) | UB | partly in open PR #1770 (the getter docs now warn; the deadlock remains) |
| SF-14 | `st20_tx_update_destination` holds the session spinlock across the ARP wait (500 ms retries, up to 60 s); `st20_rx_update_source` across flow/IGMP work | `st_tx_video_session.c:3848-3855`, `:926`; `mt_arp.c:171-199`; `st_rx_video_session.c:3932-3990` | R03 H6 | integration | open |
| SF-15 | Pipeline BLOCK_GET: the tasklet does `pthread_mutex_lock` + `pthread_cond_signal`; the app holds the mutex while scanning, so the pinned core can sleep in `FUTEX_WAIT` | `st20_pipeline_tx.c:29-45`, `:774-789`; same in every `st*p` | R03 H3 | review / perf | open (Phase 0.5, salvaging PR #1610's `mt_session_event.c`) |
| SF-16 | `*_wake_block()` does not make a blocked `get_frame` return early (the predicate loop sleeps again until the deadline) | `st20_pipeline_tx.c:781-788` | R13 §1.4 | unit | fix in open PR #1770 |
| SF-17 | RX dynamic ext frame: `query_ext_frame` result's `addr`/`iova` are not checked (with DMA offload, iova 0 means a DMA write to IOVA 0); dedicated `ext_frames[]` `buf_len` is not checked | `st_rx_video_session.c:1279`, `:439-450` | R04 §6 #6 | unit | fix in open PR #1770 |
| SF-18 | RX ext frames the transport drops are recycled silently; the app never gets its buffer back (incomplete frame, ignored `notify_frame_ready` return) | `st_rx_video_session.c:977-981`, `:978` | R04 §6 #10 | UB | partly in open PR #1770 (a negative return now returns the incomplete frame to the pool); the silent recycle with no notice to the app remains |
| SF-19 | `instance_in_reset` is never set to 1, so the "DEV IN RESET" path is dead | `mt_main.c:534`, `mt_stat.c:49` | R06 §1.4, R13 §1.1 | review | open |
| SF-20 | PTP `locked` and `connected` are never cleared after sync loss; `mt_ptp_is_locked()` is unused | `mt_ptp.c:540-552`, `:1317`; `mt_ptp.h:131-136` | R06 Q2 | unit | open (engines track E9) |
| SF-21 | Built-in PTP switches the time function from `CLOCK_REALTIME` (UTC) to the PHC when the master is first seen: a ~37 s step for running sessions | `mt_ptp.c:1062-1067`; `dev/mt_dev.c:1597-1601` | R05 F2 (step size **[inferred]**) | integration | open |
| SF-22 | Port stats: copied after the lock is released; reset without the lock; HW counters reset on every read; iavf `tx_err` always 0 | `dev/mt_dev.c:2635-2669`, `:210-212`, `:168` | R06 N4 | unit | partly in open PR #1770 (copy and reset under the lock); reset-on-read and iavf `tx_err` remain |
| SF-23 | Pipeline stats getters/resets return 0 (success) on a stale or destroying handle without filling the output | `MT_HANDLE_GUARD(ctx, …, 0)` at `st20_pipeline_tx.c:1309,1333` etc. | R06 §1.6 | unit | fix in open PR #1770 (`-EIO`) |
| SF-24 | `mtl_get_log_level()` returns `-EIO` cast to `enum mtl_log_level` | `mt_log.c:119-125` | R06 §1.6 | unit | open |
| SF-25 | `stat_epoch_troffset_mismatch` (ST20 TX) and `st30/st40_rx_user_stats.stat_pkts_dropped` are never written; video never writes `stat_epoch_mismatch` | grep | R05 F5, R06 F3 | review | open (the docs that claimed them are corrected in PR #1770, DD-03/DD-11) |
| SF-26 | `ST_PLUGIN_MAGIC` shifts `c` by 16 instead of 8 (harmless because both sides use the macro) | `include/st_pipeline_api.h:71` | R02 §4.8 | review | open |
| SF-27 | `st_frame_fmt_is_codestream` treats the `_END` sentinel as a codestream (`<=`) | `include/st_pipeline_api.h:2309` | R02 §4.8 | unit | fix in open PR #1770 |
| SF-28 | `st_frame_is_late()` always returns false on RX frames (RX `timestamp` is media clock) | `include/st_pipeline_api.h:2470-2474`; `st_rx_video_session.c:867` | R02 §4.5 #24 | unit | open |
| SF-29 | `mtl_ptp_read_time` can `pthread_join` the TSC calibration thread and updates its two-field cache without synchronisation | `mt_main.c:1100-1127` | R02 §4.2 #12 (race **[inferred]**) | review | open |
| SF-30 | `mtl_init()` mutates the caller's params (`port_params[i].flags \|= …`) | `mt_main.c:404` | R13 §1.1 | unit | open (the Phase 0 versioned params never mutate the input) |
| SF-31 | Several instance calls dereference without a NULL check (`mtl_get_lcore`, `mtl_put_lcore`, `mtl_bind_to_lcore`, `mtl_abort`) | `mt_main.c:700-758` | R13 §1.1 | unit | open |
| SF-32 | `mtl_sch_unregister_tasklet` called from a tasklet on the same scheduler sleeps 1 ms × 1000 and fails; `request_exit/ack_exit` are plain `bool` | `mt_sch.c:873-923`, `mt_main.h:476-477` | R03 §4.1 | unit | open (the misleading header comment, DD-13e, is corrected in PR #1770) |
| SF-33 | Header split cannot be built on the pinned DPDK 26.07 (no `patches/dpdk/26.07/hdr_split/`) | `versions.env:1`, `patches/dpdk/` | R04 §5, R07 §3.2 | build | open |
| SF-34 | `ST21_PACING_WIDE` and the RX timing parser use the gapped read schedule; ST 2110-21:2022 §7.1.4 requires linear for type W | `tv_init_pacing`; `st_rx_timing_parser.c` | R12 §3.4–3.5 | measurement | open (engines track E5, opt-in on legacy) |
| SF-35 | `ST31_PTIME_80US` sets a packet time of 1/12500 s but 4 samples at 48 kHz (8 at 96 kHz), so the RTP advances at 50 kHz (+4.17 %) and drifts ≈ 41.7 ms per second against PTP | `st_fmt.c:1111-1113`, `:1172-1174`, `:1197-1199`; `st_tx_audio_session.c:207-211`, `:238`, `:274-282` | C2 #21 (read, not executed) | unit | fix in open PR #1770 (83.33 µs; a 10 ms frame is now 120 packets) |
| SF-36 | `update_destination` on the kernel-socket backend does not redirect GSO packets: they go to `t->send_addr`, fixed at queue creation | `datapath/mt_dp_socket.c:236`, `:148` | C1 #10 (effect **[inferred]**) | integration | fix in open PR #1770 |
| SF-37 | `update_destination` does not update the RTCP TX header copied from `s_hdr` at init, so retransmits keep the old destination | `st_tx_video_session.c:1023-1025` | C1 #10 (effect **[inferred]**) | unit | open |
| SF-38 | The builder can claim a pipeline frame (`CONVERTED` → `IN_TRANSMITTING`) and return without building it (`refcnt != 0`, oversize user meta), leaving the slot stuck with no completion | `st20_pipeline_tx.c:210-213`; `st_tx_video_session.c:1936-1953` | C1 #12 (stuck state **[inferred]**) | UB | open (the slot interface's rejected-at-pick-up callback, engines track R2) |
| SF-39 | Double completion window: `tv_frame_free_cb` reads `refcnt`, notifies, then decrements, while TX recovery reads, notifies and resets; both can report DONE for one frame when the free runs elsewhere (#1147's class) | `st_tx_video_session.c:127-135` vs `:4295-4300` | C1 #11 **[inferred]** | UB stress | partly in open PR #1770 (free-callback half, SF-05) |

## 2. Library — probable, needs a reproducing test first

| ID | Finding | Evidence | Note | Status |
|---|---|---|---|---|
| SP-01 | `mtl_uninit` with live sessions self-deadlocks: `tv_mgr_uinit` holds the session spinlock and `tv_mgr_detach` takes it again (TX video/audio, anc, fmd; RX video does not re-lock) | `st_tx_video_session.c:3803`, `:3907-3914`; `st_tx_audio_session.c:2538` | R13 §1.4 | open |
| SP-02 | `DATA_PATH_ONLY` on non-kernel-socket backends dereferences a NULL flow | `st_rx_video_session.c:3053-3056`; `datapath/mt_queue.c:56`, `:76` | R07 §7 #10 | fix in open PR #1770 (fails at create on backends that need a flow) |
| SP-03 | `USE_MULTI_THREADS` RX: when the packet ring is full the tasklet processes packets while the packet lcore does too — two threads on one session | `st_rx_video_session.c:2901-2907`, `:2479` | R03 H10 | open |
| SP-04 | `st20_tx_free` reads `s_impl->sch` without a lock while admin migration may rewrite it | `st_tx_video_session.c:4802`; `mt_admin.c:101-141` | R13 §1.5 | open |
| SP-05 | DMA offload combined with GPU frames is not rejected; GPU frames have `iova = 0` | `st_rx_video_session.c:2572-2575` | R04 §5 | fix in open PR #1770 (DMA offload skipped for GPU framebuffers) |
| SP-06 | DMA device teardown does not drain in-flight copies when another session keeps the device alive | `mt_dma.c:438-460` | R04 §6 #12 | open |
| SP-07a | st40p TX leaks UDW buffers and double-frees on the create-failure path (secondary sweep, not re-verified) | `st40_pipeline_tx.c` | R04 §6 #15 | fix in open PR #1770 |
| SP-07b | st40p RX `meta_num` not clamped (secondary sweep, not re-verified) | `st40_pipeline_rx.c` | R04 §6 #15 | open (the PR does not touch `st40_pipeline_rx.c`) |
| SP-07c | st20p RX `PKT_CONVERT` + `EXT_FRAME` writes through a NULL destination; `st20p_rx_get_fb_addr` returns the plane array instead of `addr[0]` (secondary sweep, not re-verified) | `st20_pipeline_rx.c` | R04 §6 #15 | partly in open PR #1770 (`st20p_rx_get_fb_addr` fixed; the NULL destination remains) |
| SP-07d | st22p create leaks `ctx` on failure; st22p/st30p/st40p `frame_done` flag ordering can suppress the next callback (secondary sweep, not re-verified) | `st22/st30/st40_pipeline_tx.c` | R04 §6 #15 | fix in open PR #1770 (both) |
| SP-08 | MtlManager death: no reconnect, and `send()` without `MSG_NOSIGNAL` / SIGPIPE handling can kill the process | `mt_instance.c:15-32`; the two sends at `:17` and `:69` `[C5 §8.6]` | R13 §2 | partly in open PR #1770 (`MSG_NOSIGNAL` on both sends, with a unit test); reconnect, re-registration and lcore re-announce remain (Phase 2, G-98); the manager side is SF-48 |
| SP-10 | USER_PACING (non-exact) has no onward/duplicate check: two frames can take the same epoch | `st_tx_video_session.c:644-656` | R05 F6 | open |

## 3. Documentation and knowledge-base drift

`CLAUDE.md` says: *"If the knowledge base disagrees with the code, fix the knowledge base
in the same change."* These are the disagreements found.

| ID | Drift | Evidence | Status |
|---|---|---|---|
| DD-01 | `CLAUDE.md` describes `ld_preload/` as an "LD_PRELOAD UDP shim"; the mudp/mufd stack was removed in `2b182cd87` (#1647), only a no-op `meson.build` remains | R08 §1.1 | fix in open PR #1770 |
| DD-02 | KB §7 claims secondary-process stats access (`--proc-type=secondary`); the library always passes `--in-memory` | `dev/mt_dev.c:344`; R07 §5.2 | fix in open PR #1770 |
| DD-03 | KB §5 names `stat_frame_late` (does not exist) and `stat_epoch_mismatch` for video (never written) | R05 F29, R06 F3 | fix in open PR #1770 |
| DD-04 | KB line 41 lists pipeline prefixes without `st40p_` | R02 §7 | fix in open PR #1770 |
| DD-05 | `doc/design.md:390` references `ST40P_TX_FLAG_EXT_FRAME`, which does not exist | R02 §7, R07 §8 | fix in open PR #1770 |
| DD-06 | `doc/design.md` §6.11 repeats the USER_TIMESTAMP paragraph and never describes USER_PACING | R02 §7 | fix in open PR #1770 |
| DD-07 | `doc/design.md` §6.13 update-function list misses st41, st30p, st40p | R02 §7 | fix in open PR #1770 |
| DD-08 | `doc/design.md:618` says ST40 RX is RTP-only; frame-level RX exists | R07 §8 | fix in open PR #1770 |
| DD-09 | `doc/design.md` §8.2 says video RTP "reflects the actual wire time" — contradicts ST 2110-10 §7.5 and muddles the RL compensation text | R05 §4, R12 §10.7 | fix in open PR #1770 (the section now says the RTP is the scheduled first-packet time) |
| DD-10 | `doc/experimental/af_xdp.md:54` uses prefix `af_xdp:`; code only recognises `dpdk_af_xdp:` | `mt_util.c:962`; R07 §5.1 | fix in open PR #1770 |
| DD-11 | `doc/stats_guide.md` calls the TX late-drop check "post-send" (it runs in `next_frame`), claims all counters are thread-safe via the session spinlock (false for pipeline overlays, pkt_lcore mode, port stats), and says audio overflow bumps `stat_pkts_dropped` (never written) | R06 F4 | fix in open PR #1770 |
| DD-12 | `.github/instructions/mtl-c-coding.instructions.md` and `CLAUDE.md` say C99 only in `lib/`; 13 files use C11 `_Atomic`/`<stdatomic.h>`, and meson sets no `c_std` | R13 §1.3 | fix in open PR #1770 (the text; Q-ABI-7 still decides the policy) |
| DD-13a | `MTL_FLAG_TX_VIDEO_MIGRATE` doc describes RX | `mtl_api.h:345-350` | fix in open PR #1770 |
| DD-13b | `rl_offset_ns` documented as µs | `st30_api.h:465-466` | fix in open PR #1770 |
| DD-13c | stats getters document a nonexistent `@param port` | `st20_api.h:1985-1986`, `st30_api.h:611-612`, … | fix in open PR #1770 |
| DD-13d | `ST40/41_RX_FLAG_DATA_PATH_ONLY` say "st30_rx_ops"; `ST30P_RX_FLAG_DATA_PATH_ONLY` points to `st30_rx_get_queue_meta`, a session-layer function, because no `st30p_rx_get_queue_meta` exists | `st40_api.h:111`, `st41_api.h:64`, `st30_pipeline_api.h:243` | fix in open PR #1770 |
| DD-13e | `mtl_sch_unregister_tasklet` comment says unregister is allowed only before `mtl_sch_start` | `mtl_sch_api.h:131-133` | fix in open PR #1770 |
| DD-13f | `st20_combined_api.h:22` says ST 2110-22; `timestamp_last_pkt` doc says "first pkt" | `st20_combined_api.h:22`, `st20_api.h:576-577` | fix in open PR #1770 |
| DD-14 | st30p/st40p headers say `notify_frame_done` is called only when `notify_frame_late` is not; the code calls both | `st30_pipeline_api.h:143-145`, `st40_pipeline_api.h:166-168` vs `st30_pipeline_tx.c:138-142`, `st40_pipeline_tx.c:141-145` | fix in open PR #1770 |
| DD-15 | `*_DROP_WHEN_LATE` docs say "when mtl reports late frames"; it silently requires USER_PACING + TAI and uses a one-frame grace window | `st_pipeline_api.h:461-466` vs `st20_pipeline_tx.c:124-139` | fix in open PR #1770 |
| DD-16 | `ST40P_*_FLAG_FORCE_NUMA` documented "NOT SUPPORTED YET" but TX uses it for context memory; `st40p_rx_ops.rtp_ring_size` documented mandatory but unused | R07 §8 | partly in open PR #1770 (`rtp_ring_size` now documented unused; the FORCE_NUMA text remains) |
| DD-17 | Public leftovers of removed features: `MTL_TRANSPORT_UDP`, `MTL_FLAG_UDP_LCORE` | `mtl_api.h:295-302`, `:381` | fix in open PR #1770 (marked unused) |
| DD-18 | ST30/40/41 `ENABLE_RTCP` flags are defined but never read in `lib/` | R07 §3.1 | fix in open PR #1770 (documented as ignored) |

## 4. Consumers (samples, plugins, bindings)

| ID | Finding | Evidence | Status |
|---|---|---|---|
| SC-01 | Rust: `ops.priv_` taken from a by-value `self` that is then moved (dangling pointer used by C callbacks) | `rust/src/imtl/video.rs:482-484`, `:535-538` (UB **[inferred]**) | open |
| SC-02 | Rust: `Mtl` derives `Clone` and implements `Drop` → double `mtl_uninit` on clone | R08 §1.2 | open |
| SC-03 | FFmpeg: `ops_rx.gpu_context` points at a stack local | `ecosystem/ffmpeg_plugin/mtl_st20p_rx.c:207,217` | fix in open PR #1770 |
| SC-04 | FFmpeg sets `ST20_RX_FLAG_DMA_OFFLOAD` on `st20p_rx_ops`; sample sets `ST30P_TX_FLAG_BLOCK_GET` on RX ops (both work only because bit values coincide) | `mtl_st20p_rx.c:176`; `app/sample/rx_st30_pipeline_sample.c:163` | open |
| SC-05 | GStreamer st20p RX `zero_copy` condition is always true for the two accepted formats, so the memcpy path is dead | `gst_mtl_st20p_rx.c:274` (HEAD) | open |
| SC-06 | GStreamer never calls `wake_block` and has no `unlock` vfunc: flush waits out the 1 s timeout | R08 §1.2 | open |
| SC-07 | Samples: `pthread_create` checked with `< 0`; `fwd/rx_st20p_tx_st20p_split_fwd.c` never increments `fb_fwd` (always exits `-EIO`); ext-frame sample frees DMA memory before `st20p_tx_free`; return values of put calls ignored | R08 §1.2 | partly in open PR #1770 (the first three, a double frame put and a dropped-frame return fixed; other ignored put returns remain) |
| SC-08 | OBS: `pthread_mutex_unlock` on a mutex that is not held | `ecosystem/obs_mtl/linux-mtl/mtl-input.c:125` | fix in open PR #1770 |

## 5. Found by review C5 and during revision 3

Each C5 claim was re-read at `545a266a` with `git show` before it was added. Where C5's
line numbers or wording did not match the code, the details below give the correct
citation and say so.

| ID | Finding | Evidence | Note | Status |
|---|---|---|---|---|
| SF-40 | `mtl_is_manager_alive()` logs at `err` when no manager runs (the normal single-instance case) and opens a new connection per call | `lib/src/mt_instance.c:255-271` (`err` at `:266`); `:201` | C5 §8.6 **[verified]** | open |
| SF-41 | TX recovery zeroes the frame's `sh_info` refcount while chain mbufs of the frame may still be held in descriptors | `st_tx_video_session.c:4290-4301` | C5 §4.11 **[inferred]** | open (engines track R1) |
| SF-42 | `tx_st22p_frame_done` leaves IN_TRANSMITTING by load-then-store, not CAS | `st22_pipeline_tx.c:263-265` | C5 §4.11 (latent) | open (the slot interface's reclaim needs a CAS) |
| SF-43 | The OBS output labels an OBS monotonic timestamp as MEDIA_CLK | `ecosystem/obs_mtl/linux-mtl/mtl-output.c:224-225` | C5 §7.8 (latent) | open |
| SF-44 | `tx_st20p_newest_available` returns the **oldest** frame; sequence numbers are taken at `get_frame` | `st20_pipeline_tx.c:62-77`, `:806-808` | C5 §4.11 **[verified]** | open (rename; `seq` at submit, G-08) |
| SF-45 | RX complete-frame path: a refused `notify_frame_ready` drops a frame already counted as received | `st_rx_video_session.c:938-943`; ST22 `:1043-1048` | C5 §4.11 **[verified]** | open |
| SF-46 | RX with DMA offload drops a new frame's packets while the previous frame's copies are in flight, visible only in the stat log | `st_rx_video_session.c:1202-1208`, `:3577-3583` | C5 §4.11 **[verified]** | open (RX reject reason `dma_busy`, 08 §2.2) |
| SF-47 | The MtlManager socket is world-accessible and trusts client-reported identity | `manager/mtl_manager.cpp:95-96`; `manager/mtl_instance.hpp:192-193`, `:244-269`; `lib/src/mt_instance.c:213-214` | revision 3 **[verified]** | open ([15 §7](15-security-and-deployment.md)) |
| SF-48 | The manager can die of SIGPIPE | `manager/mtl_instance.hpp:58`, `:269`; `manager/mtl_manager.cpp:57-61` | revision 3 (effect **[inferred]**) | open |
| SF-49 | With `MTL_FLAG_TASKLET_THREAD` the scheduler threads are plain pthreads never registered with EAL, so they have no lcore ID and no mempool cache | `lib/src/mt_sch.c:252-257`, `:285-287`; `rte_thread_register` appears nowhere in `lib/src` | revision 3 **[verified]** | open (04 §7.1 registers them) |
| SF-50 | There is no TX queue stop or reset path: a fatal TX queue is only flagged, and the destroy path's pad flush gives up after 1 ms per pad, so a stalled queue keeps its descriptors, and the memory they reference, in use | `dev/mt_dev.c:1840-1869`, `:1782-1799`, `:1760-1780` | revision 3 **[verified]** | open (04 §4.5; spike S8) |
| SF-51 | With `udp_port = 0` the default destination port is index-derived and differs between TX create, TX update and RX | see details | revision 3 **[verified]** | open (09 §9.3 requires `udp_port`) |
| SF-52 | Default TX SSRCs are deterministic, and ANC and fastmeta share one base, so ANC session *i* and fastmeta session *i* get the same SSRC; two MTL processes emit identical SSRCs | `st_tx_video_session.c:966`, `st_tx_audio_session.c:188`, `st_tx_ancillary_session.c:246`, `st_tx_fastmetadata_session.c:189` | revision 3 **[verified]** | open |
| SF-53 | TX audio sessions get the default name `"RX_AUDIO_M%dS%d"` | `st_tx_audio_session.c:2117` | revision 3 **[verified]** | open |
| SF-54 | The split-forward sample sets `ops_rx.interlaced` inside the TX-session loop, so its TX sessions never get `interlaced` | `app/sample/fwd/rx_st20_tx_st20_split_fwd.c:266` | revision 3 **[verified]** | open (PR #1770 changes only the st20p split-forward sample) |
| SF-55 | GStreamer st20p sink, copy path: it rejects a buffer smaller than the frame but copies the whole buffer, so a larger GstMemory overflows the MTL frame; with `interlace-mode=interleaved` every buffer overflows by one field | `ecosystem/gstreamer_plugin/gst_mtl_st20p_tx.c:704-722`, `:358-363`; details | revision 3 (**[inferred]**, not run) | open |
| SF-56 | A tasklet unregister that times out is followed by the session free, so a scheduler still running the tasklet uses freed memory | `mt_sch.c:885-901` and its callers | addendum K (K2 H-K-14) **[verified]** | open ([16 §11](16-kubernetes-and-crash-safety.md) EK2) |
| SF-57 | Without MtlManager the lcore table is SysV shm checked with `kill(pid, 0)`: across PID namespaces a live sibling's lcores are stolen, and a restarted container (often PID 1 again) never frees stale entries; the clean tool reports but does not clear | `mt_sch.c:685-699`, `:692`, `:1308-1333` | addendum K (K2 H-K-10) **[verified]** | open (EK6) |
| SF-58 | The built-in phc2sys steers the node's `CLOCK_REALTIME` and leaves frequency and tick set when the process dies; the PTP client leaves a PF PHC's frequency offset | `mt_ptp.c:163-240`, `:358-390` | addendum K (K2 H-K-2, H-K-3) **[verified]** | open (EK10) |
| SF-59 | MtlManager resolves a client's ifindex in its own network namespace, so a pod's ifindex names another interface, which then gets XDP and has its rules wiped | `mt_instance.c:228`; `mt_socket.c:397-421`; `dev/mt_af_xdp.c:390`, `:742` | addendum K (K2 H-K-4) **[verified]** | open (EK5) |
| SF-60 | MtlManager deletes every flow rule on an interface's first use, and a restarted manager loses every grant | `manager/mtl_interface.hpp:75`, `:97` | addendum K (K2 H-K-5) **[verified]** | open (EK5) |
| SF-61 | `lcores` without `main_lcore` injects CPU 0 into EAL's `-l`; a CPU outside the cpuset aborts inside EAL | `dev/mt_dev.c:430-441` | addendum K (K2 H-K-11) **[verified]** | open (EK7) |
| SF-62 | `numa_bind` rewrites the calling thread's affinity and memory policy | `mt_main.c:448-461` | addendum K (K2 H-K-12) **[verified]** | open (EK13) |
| SF-63 | Multicast groups are not left at uninit before the ports close, and no gratuitous ARP is sent at start | `mt_mcast.c:527-559` | addendum K (K2 H-K-17) **[verified]** | open (EK9) |
| SF-64 | Kernel-socket ARP spins forever when `sendto` fails: the abort and timeout checks come after the `continue` | `mt_socket.c:323-328` | addendum K (K2 H-K-22) **[verified]** | open (EK3) |
| SF-65 | A `kahawai.json` in the working directory is read implicitly and can `dlopen` plugins | `mt_config.c:52-61` | addendum K (K2 H-K-23) **[verified]** | open (EK12) |
| SF-66 | A second MtlManager unlinks the live one's socket (split brain); the SKB-mode XDP program is never detached because the attached mode is overwritten | `manager/mtl_manager.cpp:85`; `manager/mtl_interface.hpp:420-430` | addendum K (K2 H-K-24, H-K-25) **[verified]** | open (EK5) |
| SF-67 | vfio no-IOMMU mode is not detected: after a SIGKILL the NIC may DMA into pages the kernel has reused | no check in `dev/mt_dev.c` | addendum K (K2 H-K-1) **[verified: absence]** | open (EK8) |
| SF-68 | The RX parsers read the payload at a fixed offset and ignore the RTP X and CC bits, so a stream with a header extension (HDCP, IPMX PEP) or CSRCs is mis-parsed | RX session parsers; TX writes X = CC = 0 at `st_tx_video_session.c:959-960` | addendum N (I1 GI-9) **[verified]** | open ([17 §3.3](17-nmos-and-ipmx.md)) |

Details:

- **SF-40.** `mt_instance_init` also reports the missing manager, at `warn` (`:201`), not
  `err` as C5 says. The manager state belongs in `mtl_instance_get_status` (08 §6).
- **SF-41.** `rte_mbuf_ext_refcnt_set(&frame->sh_info, 0)` runs while mbufs may still be in
  descriptors: on the hung queue itself, where `mt_txq_done_cleanup` frees only completed
  descriptors (`:4261`), and on the other 2022-7 legs, which are only pushed with
  `2 × nb_tx_burst` pad packets (`:4284-4288` → `mt_dpdk_flush_tx_queue`,
  `dev/mt_dev.c:1782-1798`), not drained. The next PMD free of such an mbuf decrements a
  zeroed count, so the free callback misfires or never fires. C5 cites `:4222-4229`, which
  at `545a266a` is the end of `tv_st22_ops_check`, just before `st20_tx_queue_fatal_error`
  at `:4231`.
- **SF-42.** C5 calls the field non-atomic; it is `_Atomic uint32_t`
  (`st22_pipeline_tx.h:23`), and st20p uses the same check-then-store
  (`st20_pipeline_tx.c:267-272`). Harmless while one context moves a frame out of
  IN_TRANSMITTING; a race once flush reclaim or recovery on a worker can too.
- **SF-43.** The output sets `tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK` with
  `timestamp = obs_frame->timestamp`. It has no effect today: the output sets neither
  USER_TIMESTAMP nor USER_PACING, and st20p copies the frame's time only with one of them
  (`st20_pipeline_tx.c:231-233`). It becomes a wrong RTP the day either flag is enabled.
  C5 cites `:208-230`. The same output names its TX session `"mtl-input"` (`:156`) and
  copies each plane as if contiguous, ignoring `obs_frame->linesize` (`:217-222`), so a
  frame whose OBS row pitch exceeds the packed row size is copied skewed (**[inferred]**).
- **SF-44.** The function keeps the frame whose sequence number is not greater, i.e. the
  oldest, as the converter's comment at `:322` expects: the behaviour is the intended FIFO
  and the name is wrong. Because numbers are assigned at `get_frame`, transmit order is
  acquire order, not `put_frame` order.
- **SF-45.** The frame goes back to the pool after `stat_frames_received` was incremented
  (`:923`), and the only trace is an `err()` on the tasklet (`:941`). PR #1770 fixes the
  *incomplete*-frame path only (SF-18).
- **SF-46.** The drop is counted in `dma_previous_busy_cnt`, which the stat dump prints and
  resets; it appears in no user stat or frame status.
- **SF-47.** The manager makes the socket world-accessible ("Allow all users to connect
  (which might be insecure)") and uses the pid/uid the client writes into its register
  message, without `SO_PEERCRED`. Any local user can take lcores, add or remove flows and
  UDP filters, and receive an interface's XSK map fd over `SCM_RIGHTS`.
- **SF-48.** The manager's `send`/`sendmsg` pass no `MSG_NOSIGNAL`, and only SIGINT is
  handled, so a client that closes its socket before a reply arrives can kill the root
  manager. PR #1770 fixes the library side only (SP-08).
- **SF-50.** `mt_dev_tx_queue_fatal_error` sets `fatal_error` and nothing else;
  `mt_dpdk_flush_tx_queue` pushes `2 × nb_tx_burst` pads through `mt_dpdk_tx_burst_busy`
  with a 1 ms timeout each. `lib/` calls `rte_eth_dev_rx_queue_stop` (`dev/mt_dev.c:1923`)
  but never `rte_eth_dev_tx_queue_stop`, so nothing releases descriptors a hung queue holds.
  Related to SF-41.
- **SF-51.** Audio: TX create 10100 + 2i (`st_tx_audio_session.c:2127`), TX update and RX
  20000 + 2i (`:2262`; `st_rx_audio_session.c:997`). ANC: TX create 10200 + 2i
  (`st_tx_ancillary_session.c:1697`), TX update and RX 30000 + 2i (`:1872`;
  `st_rx_ancillary_session.c:1007`). Fastmeta: the same as ANC
  (`st_tx_fastmetadata_session.c:1442`, `:1618`; `st_rx_fastmetadata_session.c:435`). A TX and an RX session left at
  defaults therefore never meet, and an `update_destination` with port 0 moves a TX
  session to another port than its create chose.
- **SF-52.** Bases: video `idx + 0x123450`, audio `+ 0x223450`, ANC and fastmeta both
  `+ 0x323450`.
- **SF-55.** `sink->frame_size` is `st20p_tx_frame_size()` (`:420`), which returns the
  pipeline's `src_size` (`st20_pipeline_tx.c:1260-1268`), halved for interlaced by `st_frame_size` (`st_fmt.c:611`); the
  frames are allocated at that size (`st20_pipeline_tx.c:575`; `src_size` from `:1092`).
  The copy at `:722` uses `buffer_size`, the mapped GstMemory size. The PTS-pacing path
  also adds its offset to the upstream buffer's PTS in place (`:718`, `+=`).
