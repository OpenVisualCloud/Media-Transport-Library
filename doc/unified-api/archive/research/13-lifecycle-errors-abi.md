# Research 13 — Lifecycle, recovery, error model, ABI/versioning, build/packaging

| Field | Value |
|---|---|
| Topic | Instance and session lifecycle, recovery, error model, ABI/versioning, build/packaging constraints for a unified session API |
| Revision | 1 |
| Date | 2026-09-29 |
| Baseline | `main` HEAD `545a266a` (working tree has uncommitted edits; cited lines are from the working tree) |
| Status | Knowledge gathering only — nothing is implemented |

Labels: **[verified]** = read in the cited source; **[inferred]** = follows from verified code but not exercised; **[unknown]** = not established.

---

## 1. Lifecycle as implemented

### 1.1 Instance (`mtl_handle`)

```text
          mtl_init()                 mtl_start()              mtl_stop()
(none) ─────────────► INITIALIZED ───────────────► STARTED ───────────────► INITIALIZED
            │  NIC ports started,       schedulers (lcores/threads) run       schedulers stopped,
            │  admin/PTP/CNI/ARP up     tasklets                              ports stay up
            │
            └── MTL_FLAG_DEV_AUTO_START_STOP: init calls start, mtl_stop() is a no-op
mtl_uninit(): stop → free subsystems → rte_eal_cleanup()  →  (process can never mtl_init again)
mtl_abort(): sets a flag only
```

- **[verified]** `mtl_init()` returns `NULL` on every failure; the reason is only logged (`lib/src/mt_main.c:389-633`).
- **[verified]** `mtl_start()`/`mtl_stop()` only start/stop schedulers: `mt_dev_start()` → `mt_sch_start_all()`, `mt_dev_stop()` → `mt_sch_stop_all()` (`lib/src/dev/mt_dev.c:2097-2114`). NIC ports are started during `mtl_init()` (`dev_start_port()` from `mt_dev_if_init`, `lib/src/dev/mt_dev.c:1985`). So "stopped" means "no tasklet runs", not "NIC quiesced".
- **[verified]** Start/stop are idempotent (`_mt_start`/`_mt_stop` check `mt_started()`, `lib/src/mt_main.c:354-387`). Stop→start again is allowed; schedulers created later are started if the instance is started (`lib/src/mt_sch.c:1150-1159`). **[inferred]** restart works because `sch_start` re-creates the lcore/thread.
- **[verified]** One EAL per process: `static bool eal_initted` rejects a second init with `-EIO` (`lib/src/dev/mt_dev.c:324,495-498`); `mtl_uninit` calls `rte_eal_cleanup()` (`lib/src/dev/mt_dev.c:2153-2156`). So at most one `mtl_init` per process lifetime — the reason `NoCtxTest.*` needs a process each.
- **[verified]** `mtl_abort()` only sets `instance_aborted` (`lib/src/mt_main.c:752-763`); readers are ARP wait, socket wait and PTP wait loops (`lib/src/mt_arp.c:182`, `lib/src/mt_socket.c:330`, `lib/src/mt_ptp.c:1649`). It does **not** wake pipeline `get_frame` sleepers nor stop tasklets.
- **[verified]** `instance_in_reset` is initialised to 0 and read by `mt_stat.c:49` but never set to 1 anywhere — dead state (`lib/src/mt_main.c:542`, `lib/src/mt_main.h:1193,1752`).
- **[verified]** `mtl_init()` mutates the caller's params (`p->port_params[i].flags |= ...`, `lib/src/mt_main.c:416`) and then copies `sizeof(*p)` of it (`lib/src/mt_main.c:494`).
- **[verified]** NULL/type checks are inconsistent: `mtl_start/stop/uninit` check NULL (`-EINVAL`) and type (`-EIO`); `mtl_get_lcore`, `mtl_put_lcore`, `mtl_bind_to_lcore`, `mtl_abort` dereference without a NULL check (`lib/src/mt_main.c:707-763`).

### 1.2 Sessions (all families: `st20/22/30/40/41_{tx,rx}`, `st20p/22p/30p/40p_{tx,rx}`)

```text
            *_create(mt, ops)                                   *_free(h)
(none) ──────────────────────► ACTIVE (attached to a scheduler) ──────────► (freed; handle dangling)
               │  no separate start/stop per session                 │
               │  tasklet runs iff instance started                   └─ fatal error: s->active=false,
               │                                                          ST_EVENT_FATAL_ERROR (TX video only)
               └─ create allowed before or after mtl_start()
```

- **[verified]** There is no per-session start/stop in any public header — only `create`, `free`, `update_{source,destination}`, `wake_block`, `set_block_timeout`, `put_frame_abort` (grep of `include/*.h`). Pause = `mtl_stop()` for the whole instance, or free/recreate.
- **[verified]** Sessions may be created between `mtl_init` and `mtl_start` or after start; the scheduler is started on demand (`lib/src/mt_sch.c:1150-1159`). KB §6 "Global Init Ordering" agrees.
- **[verified]** Create is serialised by the per-scheduler manager mutex (`st_tx_video_session.c:4393-4416`).
  The tasklet reaches sessions with `rte_spinlock_trylock` (`st_tx_video_session.h:29-35`); free nulls
  `mgr->sessions[idx]` under the same spinlock (`st_tx_video_session.c:3729-3747`).
  **[inferred]** Hence no tasklet-originated callback can start after `*_free` returns.
- **[inferred]** But callbacks *can* fire during free, in the freeing thread: `tv_uinit_hw()` flushes the TX queue, freeing mbufs whose extbuf callback `tv_frame_free_cb` calls `tv_notify_frame_done` (`st_tx_video_session.c:116-141`, `3271-3279`). The app must tolerate `notify_frame_done` re-entrantly from its own `free` call.
- **[inferred]** Calling `*_free` from inside a session callback deadlocks: callbacks run with the session spinlock held and `tv_mgr_detach()` takes it again (`st_tx_video_session.c:3734`). Not documented in the headers.

### 1.3 Handle guard (use-after-free defence, landed `b8b6cf51e`)

- **[verified]** `lib/src/mt_handle_guard.h` adds `lc_destroying` + `lc_refcnt` per public handle. Every public entry
  does `MT_HANDLE_GUARD` (acquire or return); `*_free` does CAS 0→1 (loser gets `-EBUSY`), calls `wake_on_destroy`,
  spins in `mt_handle_drain()` until refcnt is 0, then tears down. Used by all 8 pipeline types and all session
  families (e.g. `st20_tx_free`, `st_tx_video_session.c:4715-4755`).
- **[verified]** It protects *concurrent* callers during free, not calls *after* free: the guard reads `impl->type` and `lc_destroying` through the raw pointer (`mt_handle_guard.h:73-95`), which after `mt_rte_free(ctx)` is freed memory. A NULL handle segfaults (`*type` deref). Some entry points also read `ctx->idx` before the guard (`st20_pipeline_tx.c:759`).
- **[verified]** Type mismatch or "dying" returns `-EIO` (the historic convention), not `-EINVAL`/`-ESHUTDOWN` (`mt_handle_guard.h:66-95`).
- **[verified]** Blocking sleepers hold their ref across `pthread_cond_timedwait` and are woken by `wake_on_destroy` (`st20_pipeline_tx.c:775-799`, `1126`, `1198-1199`).
- **[verified]** It is the first C11 in `lib/`: `<stdatomic.h>`/`_Atomic` appear in 13 lib files, while `mtl-c-coding.instructions.md` and `CLAUDE.md` say "C99 only". No `c_std` is set in meson, so the compiler default (gnu17) applies.

### 1.4 Blocking calls at free / stop time

| Situation | Behaviour | Label |
|---|---|---|
| `st20p_tx_free` with frames `IN_TRANSMITTING` | `tx_st20p_framebuffs_flush()` sleeps 50 ms per transmitting frame and polls other states up to 100×10 ms per frame — free can block ~1 s per frame; skipped if instance not started | [verified] `st20_pipeline_tx.c:722-753,1203` |
| `*_free` while app thread blocked in `get_frame` | woken via `wake_on_destroy`, returns NULL; any frame claimed during the wake is returned to FREE | [verified] `st20_pipeline_tx.c:781-799` |
| `*_wake_block()` without free | predicate loop re-checks and **goes back to sleep** until the fixed deadline unless a frame appeared — it does not force an early return | [verified] `st20_pipeline_tx.c:781-788`, commits `74541c90`, `ed63ea96` |
| Samples' shutdown | set `stop`, call `*_wake_block`, join → shutdown latency up to `block_timeout_ns` (default 1 s) | [inferred] `app/sample/tx_st20_pipeline_sample.c:248`, `st20_pipeline_tx.c:1134` |
| `mtl_stop()` while pipelines active | tasklets stop; blocked `get_frame` times out and returns NULL; nothing tells the app why | [inferred] |
| `st20p_rx_free` with frames held by app (`IN_USER`) | buffers freed; app's `st_frame*` dangles; no check | [inferred] `st20_pipeline_rx.c:1116-1157` |
| `mtl_uninit()` with leaked sessions | `tv_mgr_uinit` holds the session spinlock, then `tv_mgr_detach` takes it again → self-deadlock; same in TX/RX audio, anc, FMD; RX video does not re-lock. Leaked `s_impl` never freed. | [inferred] `st_tx_video_session.c:3734,3838-3845`; `st_tx_audio_session.c:2536`; `st_rx_video_session.c:4023-4047` |

### 1.5 Concurrency hazards not closed by the guard

- **[inferred]** `st20_tx_free` reads `s_impl->sch` without a lock (`st_tx_video_session.c:4731`) while `mt_admin` migration (`MTL_FLAG_TX_VIDEO_MIGRATE`) rewrites it under both manager mutexes (`lib/src/mt_admin.c:101-141`). A free racing a migration can lock the wrong manager and detach the wrong index. Opt-in flag only.
- **[inferred]** Migration moves a session to another lcore, so application callbacks silently change thread; not documented as part of the callback contract.

---

## 2. Recovery

| Trigger | Detection | Action | What the app sees | Label |
|---|---|---|---|---|
| TX video queue hang (no good burst for `tx_hang_detect_ms`, default 1 s) | `video_trs_burst_fail` | `st20_tx_queue_fatal_error`: drain rings, swap queue, complete held frames, re-init mempools (`recovery_idx++`) | `ST_EVENT_RECOVERY_ERROR`, or `FATAL_ERROR` + `s->active=false`; stats | [verified] `st_video_transmitter.c:31-44,679-682`; `st_tx_video_session.c:4160-4261` |
| TX audio queue hang (1 s) | `st_audio_trs_burst_fail` | Per-manager: new queue, re-init every session's mempool | **No event** — stats only | [verified] `st_audio_transmitter.c:45-58`; `st_tx_audio_session.c:2718-2776` |
| TX ancillary / fast-metadata hang | none found | — | nothing | [verified] by absence of `fatal_error` in those files |
| Non-DPDK PMD (AF_XDP, kernel socket) TX video hang | — | no recovery | `ST_EVENT_FATAL_ERROR` but session stays "active" | [verified] `st_tx_video_session.c:4167-4171` |
| Link down at init | `dev_detect_link` | fail init, or mark port down if `MTL_FLAG_ALLOW_DOWN_PORTS`; sessions prune down ports | create-time only | [verified] `mt_dev.c:1968-2005`, `st_tx_video_session.c` `tv_ops_prune_down_ports` |
| Link down at runtime | **not detected** — no `rte_eth_dev_callback_register`, no LSC polling | TX: indistinguishable from hang → recovery loop each hang window; RX: nothing | TX video: repeated `RECOVERY_ERROR`; RX: frames simply stop | [verified] grep shows no `RTE_ETH_EVENT_*`; loop behaviour [inferred] |
| VF reset (PF reset / iavf `RESET` event) | not handled by MTL | relies on DPDK PMD internals | nothing | [verified] no reset handling; `inf->resetting` only brackets `rte_tm_hierarchy_commit` (`mt_dev.c:755-759`) |
| PTP loss | sync-timeout alarm | extrapolates (`ptp_sync_expect_result`), counts `stat_sync_timeout_err`; `connected`/`locked` are never cleared after first lock | nothing but logs/stats; `ptp_sync_notify` just stops firing | [verified] `mt_ptp.c:598-625`; only `mt_ptp.c:1315` sets `locked=false` (init path) |
| MtlManager dies | no heartbeat, no reconnect | existing lcores/queues keep working; next `mt_instance_*` `send()` fails (and, without `MSG_NOSIGNAL`/`SIGPIPE` handling, can raise SIGPIPE) | log errors; possible process kill | [verified] `mt_instance.c:15-32`; no `SIGPIPE`/`MSG_NOSIGNAL` in `lib/`; SIGPIPE outcome [inferred] |
| App process dies | manager sees EOF | `~mtl_instance()` returns lcores | n/a | [verified] `manager/mtl_manager.cpp:192-199`, `manager/mtl_instance.hpp:66-68` |
| Manager restarts while clients live | manager state was in memory | **[inferred]** new manager does not know previously granted lcores → double allocation possible | nothing | [inferred] |

In-flight data during TX video recovery: packets in rings and NIC are discarded; frames holding references are
completed via `notify_frame_done` as if sent (`st_tx_video_session.c:4219-4231`). The only loss signal is one
`RECOVERY_ERROR` event with `args=NULL` — no frame IDs, no count **[verified]**. RX has no recovery path; loss shows as
incomplete frames/stats **[verified]** (RX emits only `ST_EVENT_VSYNC`, `st_rx_video_session.c:3485`).

`enum st_event` has only `VSYNC`, `RECOVERY_ERROR`, `FATAL_ERROR` (`include/st_api.h:208-220`) **[verified]**. Emitters: `FATAL_ERROR` 4 sites and `RECOVERY_ERROR` 1 site, all in `st_tx_video_session.c` **[verified]**.

---

## 3. Error model

| Aspect | Current state | Label |
|---|---|---|
| Integer returns | `0` success, negative errno. Distribution of `return -E*` in `lib/src/*.c`: `-EIO` 368, `-EINVAL` 312, `-ENOMEM` 136, `-EBUSY` 43, `-ENOTSUP` 27, `-EAGAIN` 3, `-ETIMEDOUT` 1, `-ETIME` 1, `-ENOSPC` 1 | [verified] grep |
| `-EIO` meaning | catch-all: wrong handle type, dying handle, queue failure, "not dpdk", "no queue" | [verified] `mt_handle_guard.h:66`, `mt_main.c:647,686` |
| Handle-returning calls | `*_create`, `mtl_init`, `get_frame`, `get_mbuf`, `get_framebuffer` return `NULL` on any failure; no errno, no out-param | [verified] `include/st20_api.h:1839,1894,1929,…` docs; only 4 `rte_errno` reads, zero `errno =` in `lib/` |
| Pipeline `get_frame` NULL | same NULL for: not ready, timeout, woken, destroying, stopped | [verified] `st20_pipeline_tx.c:757-822` |
| Void entry points | `*_rx_put_mbuf` return nothing even on bad handle | [verified] `mt_handle_guard.h:146-151` |
| Error strings | no `mtl_strerror`; the lib uses libc `strerror` / `rte_strerror` internally for logs only | [verified] grep |
| Logging-only errors | almost every failure: create validation, queue exhaustion, PTP timeouts, manager IPC | [verified] |
| Callback returns | `get_next_frame` `<0` = no frame; `notify_frame_ready` `<0` = app rejects; `notify_event` return ignored — semantics differ per callback and are not typed | [verified] `st_tx_video_session.c:1922-1928`; rest [inferred] |
| Async errors | only TX video via `notify_event`; audio/anc/fmd/RX none | [verified] §2 |

---

## 4. ABI / versioning status

| Item | Finding | Label |
|---|---|---|
| Version source | `VERSION` = `26.01.0.DEV`; meson project version; `MTL_VERSION_MAJOR/MINOR/LAST/EXTRA` written to generated `mtl_build_config.h`, installed to `include/mtl` | [verified] `meson.build:5,42-53` |
| Compile-time version | `MTL_VERSION_NUM(a,b,c) ((a)<<16\|(b)<<8\|(c))`, `MTL_VERSION` | [verified] `include/mtl_api.h:44-49` |
| Runtime version | only `const char* mtl_version(void)` (string incl. `__TIMESTAMP__`, git describe, compiler); no numeric runtime getter, no header-vs-library check | [verified] `mt_main.c:998-1008` |
| soname | `shared_library('mtl', …)` without `version:`/`soversion:` → `SONAME libmtl.so`, never bumped in 31 commits touching `lib/meson.build` | [verified] `lib/meson.build:151-158`; `readelf -d build/lib/libmtl.so` |
| Symbol visibility | no `-fvisibility=hidden`, no export macro, no linker version script (`.map`); Windows generates a `.def` from all exports (`-Wl,--output-def,libmtl.def`) | [verified] `lib/meson.build:115-121` |
| Exported surface | 757 defined dynamic function symbols in `build/lib/libmtl.so`, 235 of them internal (`mt_*`, `tv_*`, `rv_*`) | [verified] `nm -D` on local build |
| Release cadence | tags `v22.04` … `v25.02`, `v25.12-rc1`, `v26.01`; version bumps never paired with ABI notes | [verified] `git tag`, `git log -- VERSION` |
| ABI breaks in practice | Since `v26.01`: `struct mtl_port_init_params` gained `rl_burst_size` — it is embedded as `port_params[MTL_PORT_MAX]` inside `mtl_init_params`, so every later field moved; session stats fields removed/renamed (`incomplete_frames_cnt`→`stat_frames_incomplete`, …) | [verified] `git diff v26.01 HEAD -- include` |
| Struct growth | by-value `ops` structs copied with `sizeof` of the library's view (`s->ops = *ops`, `ctx->ops = *ops`, `mt_memcpy(&impl->user_para, p, sizeof(*p))`); no `struct_size`/version member; no reserved fields | [verified] `st_tx_video_session.c:3369`, `st20_pipeline_tx.c:1145`, `mt_main.c:494` |
| Enum growth | every enum exports a `*_MAX` sentinel (≈30) which changes value on growth | [verified] `include/mtl_api.h:217-334`, `st20_api.h:315-399`, … |
| 64-bit enum | `enum mtl_init_flag` holds `MTL_BIT64(32..63)` values: outside `int`, a GNU extension before C23; MSVC keeps enums `int` | [verified] `include/mtl_api.h:338-503`; MSVC truncation [inferred] |
| Flag word usage | `MTL_FLAG_*` 39 of 64 (bits 22–31, 37, 49–62 free); `ST20P_TX` 15/32 (max bit 15); `ST20P_RX` 17/32 (max 24); `ST22P_TX` 12; `ST22P_RX` 9; `ST30P_TX` 7; `ST40P_TX` 11; `ST20_RX` 13 (max 23); `ST20_TX` 12; `ST30_RX` 7 (max 17); all session/pipeline `flags` are `uint32_t` | [verified] grep of `include/*.h` |
| Deprecation | `__mtl_deprecated_msg()` on fields (`tx_sessions_cnt_max`, `sip_addr` unions, `sample_size`), suppressed under `__MTL_LIB_BUILD__` and `__MTL_PYTHON_BUILD__`; no removal schedule | [verified] `include/mtl_api.h:141-147,742-760`, `st30_api.h:474` |
| Experimental | `include/experimental/st20_combined_api.h` installed to `mtl/experimental/`; no opt-in macro, no symbol tagging (unlike DPDK `__rte_experimental`) | [verified] `include/meson.build:10-16` |
| Plugin ABI | only versioned interface: `st_plugin_meta{version, magic}`, V1 only; `ST_PLUGIN_MAGIC` shifts `c` by 16 instead of 8 (typo, harmless) | [verified] `include/st_pipeline_api.h:61-82`, `st_plugin.c:903-917` |

Conclusion **[inferred]**: there is no ABI promise in practice. Apps must be rebuilt for every release and often for every `main` commit; the unversioned soname hides this from the dynamic loader.

---

## 5. Build / packaging constraints for a new API

- **Language**: `lib/` is documented as C99 (`.github/instructions/mtl-c-coding.instructions.md` "Language"), but it
  already uses C11 `_Atomic`/`<stdatomic.h>` in 13 files **[verified]**. Public headers include `<pthread.h>` and
  expose `pthread_t` (`mtl_bind_to_lcore`) **[verified]** `include/mtl_api.h:12,1157`. Public headers must stay
  C++-includable (`extern "C"` present) and free of C11 atomics.
- **Header layout**: installed under `<prefix>/include/mtl/` plus `mtl/experimental/`; pkg-config lists both `.` and
  `mtl` subdirs, so `<mtl_api.h>` and `<mtl/mtl_api.h>` both resolve **[verified]** `meson.build:77-85`,
  `include/meson.build`. `st_convert_internal.h` is installed and SWIG-wrapped despite its name **[verified]**.
  The include guard in `mtl_api.h` sits after the system includes **[verified]** `include/mtl_api.h:12-21`.
- **Windows**: MSYS2/MinGW64 CI build (`.github/workflows/msys2_build.yml`), `lib/windows/win_posix.[ch]` POSIX shims, static DPDK, `.def` export for MSVC consumers; manager IPC is `-ENOTSUP` on Windows (`mt_instance.c:277-345`) **[verified]**. Any new primitive (eventfd, `poll`, UNIX sockets) needs a Windows equivalent or a portable wait object **[inferred]**.
- **Python (SWIG)**: `python/swig/pymtl.i` `%include`s six public headers wholesale **[verified]**. Existing
  workarounds: `__MTL_PYTHON_BUILD__` hides packed RTP header structs/bitfields (`st_api.h:94`, `st20_api.h:723`) and
  deprecation attributes (`mtl_api.h:141`); 2-D `char`/`uint8_t` arrays need C setters (`mtl_para_port_set`,
  `mtl_para_sip_set`, `st_txp_para_port_set` at `include/st_pipeline_api.h:2412`). Python examples use only blocking
  pipeline calls — no C callbacks into Python **[verified]** `python/example/st20p_tx.py:82`.
- **Rust (bindgen)**: `rust/imtl-sys/build.rs` binds `wrapper.h` (6 headers) with defaults **[verified]**. Anonymous
  unions surface as `__bindgen_anon_1` (`rust/src/imtl/video.rs:743`); enum constants as
  `mtl_init_flag_MTL_FLAG_BIND_NUMA` (`rust/src/imtl/mtl.rs:100`); callbacks are hand-written `unsafe extern "C" fn`
  trampolines (`video.rs:389-703`) **[verified]**. Bindgen emits no `static inline` functions without
  `--wrap-static-fns` and does not evaluate function-like macros such as `MTL_BIT32(n)` in `#define` flags
  **[inferred]** (the Rust code only uses enum-typed flags, consistent with this).
- **Header-only helpers**: many `static inline` helpers in public headers (`mtl_p_sip_addr`, `mtl_para_pmd_set`, …) **[verified]** — fine for C/SWIG, invisible to bindgen, and they bake struct layouts into the application.
- **Unit-test harness**: `-Denable_unit_tests=true` builds `tests/unit/UnitTest`, which `#include`s production `.c`
  files so statics are visible and non-static symbols preempt `libmtl.so`; it runs a no-hugepage EAL without NIC or
  root **[verified]** `tests/unit/session/st40_harness.c:11-20`, `tests/unit/README.md`. Hidden visibility would not
  break this (the harness compiles the sources itself) **[inferred]**, but internal headers are not C++-clean
  (C keywords like `new` as identifiers), so a new API core must be testable through C harness shims.

---

## 6. Requirements for the new APIs lifecycle (derived)

### 6.1 Proposed session state machine

```text
             create()                start()                  stop(DRAIN)             (drain done)
  (none) ───────────► CREATED ───────────────► STARTED ───────────────────► DRAINING ───────────► STOPPED
                        │  ▲                    │  │ stop(ABORT)                                   │  │
                        │  └──────── start() ───┼──┼───────────────────────────────────────────────┘  │
                        │                       │  └────────────────────────────────► STOPPED          │
                        │                       │ unrecoverable fault                                   │
                        │                       ▼                                                       │
                        │                     ERROR ── stop()/destroy() only                            │
                        └──────────────── destroy() (from any state) ──────────────────────────► DESTROYED
```

Requirement list (each maps to a finding above):

1. **R-L1 explicit per-session start/stop**, reversible, independent of `mtl_start()` (today only instance-wide; §1.2). `CREATED` must allocate everything so `start()` cannot fail for resource reasons (or define which failures it may return).
2. **R-L2 drain vs abort**: `stop(mode)` or separate calls. DRAIN = wait for submitted TX buffers/transmitted frames with a bounded timeout; ABORT = drop and return buffers flagged as dropped. Today `st20p_tx_free` does an unbounded-ish sleep-poll drain (§1.4); `st20_tx_free` aborts.
3. **R-L3 wake semantics**: a documented "interrupt waiters" primitive that makes blocked `get/poll` return immediately with a distinct code, and a stop that implies it. Today `wake_block` does not cause early return (§1.4).
4. **R-L4 no callback after destroy returns**, and document whether callbacks can run *during* destroy in the caller's thread (§1.2) and that destroy from a callback is forbidden or deferred.
5. **R-L5 stale-handle safety**: calls after destroy must not dereference freed memory — requires handle validation not based on the raw pointer (generation-tagged table or never-freed wrapper) (§1.3). NULL must return `-EINVAL`.
6. **R-L6 instance teardown with live sessions**: either reject (`-EBUSY`) or destroy them safely; today it likely deadlocks (§1.4).
7. **R-L7 events for every media type**: link down/up, TX recovery (with count of dropped buffers), fatal, PTP lock/unlock/holdover, manager lost. Today only TX video emits anything (§2).
8. **R-L8 async-signal-safe quiesce**: one call permitted from a signal handler (samples today call `mtl_abort` which wakes nothing; §1.1).
9. **R-L9 thread-safety table per call**: which calls are safe concurrently with each other, from callbacks, from tasklet context; which may block and for how long; which thread callbacks run on (and that migration may change it, §1.5).
10. **R-L10 error classes** (proposed mapping):

| Code | Meaning in new API |
|---|---|
| `-EAGAIN` | back-pressure only: no buffer/slot right now (non-blocking call) |
| `-ETIMEDOUT` | blocking call hit its timeout |
| `-EINTR` or `-ECANCELED` | blocking call interrupted by wake/stop (distinct from timeout) |
| `-ESHUTDOWN` / `-EPIPE` | session stopped / being destroyed; caller should exit its loop |
| `-EINVAL` | bad argument, NULL or stale handle |
| `-ENOTSUP` | capability absent (PMD/NIC/feature) |
| `-EBUSY` | resource in use (destroy race loser, instance has live sessions, buffer still owned) |
| `-ENOMEM` / `-ENOSPC` | allocation / queue / lcore exhaustion |
| `-EIO` | device/hardware fault only — stop using it as the catch-all |

11. **R-L11 creation errors reported**, not only logged: `int create(..., handle_t* out)` or thread-local last-error.

---

## 7. ABI evolution strategy options

| Option | Shape | Pros in MTL context | Cons in MTL context | SWIG / bindgen |
|---|---|---|---|---|
| A. `struct_size` first member | `ops.size = sizeof(ops)`; lib reads `min(size, sizeof)`, zero-fills the rest | Minimal churn from today's by-value `ops`; cheap; C99; append-only is easy to review | Append-only; embedded struct arrays (`port_params[]`) still break; forgotten `size` needs `*_ops_init()` | Works: plain structs; Python wants an init helper |
| B. Vulkan-style `sType`/`pNext` chain | base struct + typed extension structs via `const void* next` | Optional features (redundancy, RTCP, ext-frame, DMA, GPU direct) become extensions; old apps never see new ones | Verbose; per-extension validation; unfamiliar to broadcast app devs | SWIG: `void*` chains need typemaps; bindgen: fine, unsafe casts in Rust |
| C. Opaque attribute object + setters | `a = mtl_attr_create(kind); mtl_attr_set_u32(a, KEY, v); create(mt, a, &h)` | Layout hidden (full ABI freedom); per-key validation with precise errors; easy to add keys | Many symbols or key enum; typos become runtime errors; no static type checking or initializer ergonomics | Best for both: opaque pointers + scalar setters; no unions/arrays exposed |
| D. libfabric-style versioned info | `fi_getinfo(FI_VERSION(1,x), hints, &info)`; lib fills a struct matching the requested version | Negotiation of capabilities (NIC/PMD/pacing) with one call; app declares the ABI it was compiled against | Lib must keep old struct layouts forever; heavier than MTL needs; still exposes structs | Structs with nested pointers — moderate |
| E. Symbol versioning + soname | GNU version script, `soversion`, `-fvisibility=hidden` + `MTL_API` export macro | Independent of struct strategy; lets loader reject mismatches; hides 235 internal exports | Needs a policy and a `.map` file per release; Windows needs `__declspec` path via the same macro | Transparent to bindings |

Observations: **[inferred]** A+E is the smallest step from today and fixes the "silent mismatch" problem; C is the most binding-friendly and decouples layout entirely; B/D fit best if the new API centres on capability negotiation. A hybrid (C for create-time config, plain small POD structs with `size` for hot-path metadata such as buffer/frame descriptors) keeps the data path free of setter calls.

Also needed whatever the choice **[inferred]**: numeric runtime version getter (`mtl_version_num()`), a header/library check at init (pass `MTL_VERSION` compiled into the app), enums without exported `_MAX` sentinels (or documented as non-ABI), flags as `uint64_t` + `#define`/`static const` rather than 64-bit enums, and an experimental-symbol convention.

---

## 8. Notes on the PR #1610 graceful-shutdown design (doc only)

Read `git show pr1610:doc/new_API/GRACEFUL_SHUTDOWN.md` only. **[verified]** It proposes: `mtl_session_destroy()`
self-sufficient and idempotent (CAS, `-EBUSY` loser), waking blocked `buffer_get`/`event_poll` via eventfd, draining,
then retiring a **generation-tagged handle** so stale/garbage handles return `-EINVAL`; `mtl_session_stop()`
async-signal-safe (flag + 8-byte eventfd write), reversible with `start()`; no flush (`buffer_flush()` "not yet
implemented").

Points relevant to this topic **[inferred]**:
- It addresses R-L3/R-L5/R-L8 directly; the generation table is exactly what today's guard lacks.
- It returns `-EAGAIN` for "tearing down/stopped", colliding with `-EAGAIN` as back-pressure (R-L10).
- eventfd is Linux-only; Windows needs an alternative (§5).
- No DRAIN mode; completeness left to the app (R-L2).
- "Async-signal-safe" must also hold for the guard's atomics and any logging inside `stop()`.

---

## Open questions for the maintainer

1. **ABI promise level.** Should the new API commit to a stable ABI across releases (soname with `soversion`, version script, hidden visibility) or only a stable *API* with rebuild required? This decides whether options A/C/E are mandatory now. Options: (a) API-only, bump soname every release; (b) stable ABI for the new session API, legacy APIs unversioned; (c) full ABI with symbol versions.
2. **Config shape: struct vs attribute object.** Create-time configuration via `size`-prefixed structs (familiar, static typing) or opaque attribute objects with setters (layout-free, best for SWIG/bindgen)? Hybrid possible. Matters because it fixes the binding story and every future feature addition.
3. **Per-session start/stop semantics.** Should `stop()` release NIC resources (queues, flow rules, IGMP membership) or only pause tasklets? Releasing enables resource reuse but makes `start()` fallible; pausing is cheap but keeps multicast joined.
4. **Drain vs abort on stop/destroy.** Is a bounded DRAIN (TX: all submitted buffers on the wire; RX: deliver completed frames) required in v1, or is ABORT + app-level accounting acceptable as in PR #1610? Current `st20p_tx_free` does a sleep-poll drain that can block ~1 s per frame.
5. **Error code for "stopped/destroying".** `-EAGAIN` (PR #1610) vs `-ESHUTDOWN`/`-EPIPE`, and a separate code for wake/interrupt vs timeout. Needed so worker loops can distinguish "retry" from "exit".
6. **Stale-handle guarantee.** Is "calls on a destroyed handle return `-EINVAL`, never crash" a requirement? It needs a handle table (index+generation, maybe handle as integer, which also helps bindings) instead of raw pointers.
7. **Recovery transparency vs events.** Should TX/RX recovery stay transparent with an informational event, or should the app get per-buffer outcomes (dropped/late/recovered)? Today only TX video emits a bare `RECOVERY_ERROR`; audio/anc/fmd/RX are silent.
8. **Runtime link and PTP state.** Should the new API add link-state monitoring (LSC interrupt or polling) and PTP lock/holdover events? Today runtime link loss is invisible and PTP `locked` never clears. Options: instance-level event queue, per-session events, or status polling only.
9. **Instance teardown with live sessions.** Should `mtl_uninit()` refuse (`-EBUSY`) or auto-destroy sessions? Current auto-destroy path appears to self-deadlock on TX video/audio (§1.4); worth confirming with a unit test before the redesign.
10. **Callback threading contract.** Which callbacks may run in the caller's thread during `destroy()`, may the app call `destroy()`/`stop()` from a callback, and is thread migration (`MTL_FLAG_*_VIDEO_MIGRATE`) allowed to change the callback thread? Required to write the per-call thread-safety table.
11. **Language baseline.** Formalise C11 for `lib/` (atomics already used) while keeping public headers C99/C++-clean? Affects whether the new API headers may use `_Static_assert`, `_Atomic`, or designated-initializer helper macros.
12. **Windows parity.** Is Windows (MinGW/MSVC consumers) a first-class target for the new API? If yes, blocking/wake primitives cannot be eventfd/`poll`-based, and 64-bit flag enums must go.
13. **Experimental tier.** Adopt a DPDK-like `__mtl_experimental` marker plus opt-in macro for new API pieces, so the new API can land incrementally without an ABI promise?
14. **Legacy API coexistence.** Will the `st20_*`/`st20p_*` families remain as thin wrappers over the new core (so fixes such as §1.4 land once), or be frozen and deprecated with a removal release? Determines whether the new lifecycle machinery must also back the old handles.
