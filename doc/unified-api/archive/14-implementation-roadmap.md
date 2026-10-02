# 14 — Implementation roadmap

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5); sequencing proposal with effort ranges, no dates |
| Date | 2026-09-30 |
| Principles | freeze semantics before code (review §15); **engines first** (GO-9, R-MIG-1); a user-visible result early (Phase 0.5); the test substrate before the features it tests; every phase ends with its **P** guarantees green ([13](13-guarantees-and-tests.md)) and the legacy gate; the new API stays in its experimental DSO until the ABI freeze |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

Revision 3 re-sequences the plan after review C5, which measured it at 50–75
engineer-months with no user-touchable artefact before Phase 1 exit, and the payoff (the
timing model) in Phase 3 `[C5 §1.1, §1.5]`. Items changed in revision 3 are marked
**r3**.

## 0. Shape of the plan (r3)

```text
            decisions ─┐
 Phase 0  ─────────────┼─ instance params + soname · side fixes (PR #1770) · spikes S0–S8 · header compiles in CI
                       │  · substrate design · EXTERNAL DESIGN REVIEW (exit)
 Phase 0.5 ────────────┘  st_timeline_* helper · ST30P/ST40P flags · audio start rule · SF-15   ← first user result
 Engines track  E2 → E4/E8/E9 → E1/E3 → E11/E12 → E5/E6/E7/E10/E13   (legacy gets each; bug fixes on, wire changes opt-in)
 Phase 1   core + substrate (null backend, test clock, debug inject) + ST20 over st20p + L4 simple + Python wrapper
 Phase 2   all essences · operators (update_flows, legs, link monitor, capacity, names) · G-27 · re-base go/no-go
 Phase 3   L2 timing core (modes, timelines, groups, RX timing) over the engines track
 Phase 4   memory (regions, imports, holds, MTL_POOL_DYNAMIC, early SOURCE_RELEASED)
 Phase 5   robustness at the I tier (VF reset, recovery on workers, fault matrix)
 Phase 6   progressive, packet level, plugin rewrites, legacy re-base (committed), ABI freeze
 Phase 7   later: NMOS contract extras, SDP, IPMX transport (RTCP, free run, SENDER mode), PEP
```

Phase 0.5 and the engines track do not depend on the new header, so they run in parallel
with Phase 0's spikes and with Phase 1.

## 1. Phases 0 and 0.5

### 1.1 Phase 0 — decisions

- Maintainer answers to the ≤ 10 decisions listed in [00](00-summary.md) (r3: every other
  question is a "Proposed default, object by review" in
  [DECISIONS.md](DECISIONS.md)).
- The contract document (`doc/user-pacing-timestamp-contract.md`, untracked today)
  committed and updated with this design's selection rules (13 §7).

### 1.2 Phase 0 — side fixes

The research and the reviews found bugs that exist regardless of the redesign. They are
listed with evidence in [side-findings.md](side-findings.md), which now carries a Status
column. Open PR #1770 (38 commits, +3208/−371, not merged) fixes or partly fixes about
forty of them, among them SF-01…06, SF-08, SF-09, SF-16, SF-17, SF-23, SF-27, SF-35,
SF-36, SP-02, SP-05, SP-07a/d, most DD items, SC-03, SC-07 and SC-08, and the library side
of SP-08 (`MSG_NOSIGNAL`). Still open and worth fixing before Phase 1:

- recovery reports in-flight frames as `COMPLETE` and runs on the tasklet (SF-12);
  recovery zeroes `sh_info` while chain mbufs may still be in descriptors (SF-41);
- the builder claiming a frame and returning without completing it (SF-38) and the
  double-completion window with recovery (SF-39);
- `mtl_uninit` self-deadlock with live sessions (SP-01, to confirm);
- the MtlManager socket is world-writable and trusts client-reported identity (SF-47), and
  the manager itself can die of SIGPIPE (SF-48);
- `update_destination` holding the session spinlock across the ARP wait (SF-14);
- the complete-frame RX path counting a refused delivery as received (SF-45).

### 1.3 Phase 0 — the instance boundary and the ABI hygiene of libmtl (r3)

- Versioned **`struct mtl_instance_params`** (`struct_size`, `api_version`, port specs,
  queue counts, lcores, time source, flags as plain integer words) with a versioned init,
  **before** the new API (A11, Q-ABI-2 answered yes). The default-instance **merge table**
  classifies every field as invariant, mergeable or ignored on a second acquire
  ([03 §7](03-object-model-and-lifecycle.md)).
- libmtl gets a **soname, a version script and hidden default visibility** (Q-ABI-1):
  `lib/meson.build:151` builds the shared library with no `soversion`, `version` or
  version script today; the script exports the public prefixes and makes everything else
  local, and adds the private node `MTL_INTERNAL` the unified DSO links against.
- `sketch/check.sh` runs in CI from Phase 0, not Phase 1 (G-74) `[C5 §11]`; in Phase 1 the
  doc test moves to `tests/unit/unified/` next to the library it then checks.

### 1.4 Phase 0 — spikes (r3: S0, S7 and S8 added)

Each spike is a throw-away branch with a measurement note.

| Spike | Question it answers | Output |
|---|---|---|
| **S0 baseline** | today's tasklet iteration p99.99/max, DP-equivalent call costs (`st20p_tx_get_frame`/`put_frame`), completion latency, sessions per scheduler, with `MTL_FLAG_TASKLET_TIME_MEASURE` on the reference machine | confirms or revises the 13 §8 budgets before Phase 1 |
| S1 waker | wake latency p50/p99/p99.9 and CPU % for W0, W2, fixed-interval W3 and deadline-driven W3 on a pinned-core host, at 125 µs and 1 ms unit periods, with the waiter's core awake and in C6; timer slack and affinity effects | numbers for Q-THR-2 / Q-THR-2a; the W2 auto threshold |
| S2 exact media-time math | wire diff of RTP between today's code and `floor` exact math at 1001 rates; unit-test impact | data for Q-TIME-15/16; the legacy opt-in flag design |
| S3 slot interface over st20p | cost and intrusiveness of hold/submit/done-hook/reclaim/rejected-at-pick-up; atomic RMWs per unit (04 §4.1), fence cost inside `tx_burst`, reaper scan cost per pool size; completion latency end-to-end; the cost of re-basing the legacy st20p functions on it | go/no-go for Q-ARCH-1 / Q-ARCH-1a; the Phase 6 re-base costing |
| S4 region import on two ports + DMA engine | does explicit `rte_dev_dma_map` per device work on E810 VF/PF with VFIO; PA-mode behaviour; shmem/memfd pinning; page-alignment behaviour | design input for 05 §3.2 and [15 §2](15-security-and-deployment.md) |
| S5 observed TX time | accuracy of TSC-at-`tx_burst` (enqueue) vs HW TX timestamp (E810/E830); what TSN launch gives | Q-TIME-10 |
| S6 idle descriptor cleanup | rate and cost of `rte_eth_tx_done_cleanup` on an idle session; effect on pacing; TSQ behaviour; completion latency against iavf/ice `tx_rs_thresh`/`tx_free_thresh` (04 §4.4) | Q-CMP-7; the `completion_latency_ns` values |
| **S7 time base** | frame-start error of the published, slewed time base (servo over PHC/TSC cross-timestamps, refresh ≤ 100 ms) versus a direct PHC read, under RL and TSC pacing, across a refresh | the time-base budget in 13 §8; go for E9's publication |
| **S8 queue stop/start** | whether `rte_eth_dev_tx_queue_stop`/`start` on iavf and ice release chained external mbufs and run their free callbacks | go for the stalled-queue destroy path (04 §4.5) |

### 1.5 Phase 0 — substrate design, review and exit

- Design (not yet code) of the test substrate: the null backend, the test time source and
  `mtl_debug_inject` with its fault list, plus the `gtest.sh` fault steps and the two new
  `nicctl.sh` subcommands (13 §6, §9).
- **External design review (exit criterion, r3).** A review of the header and 03–09 with
  at least three named external consumers: the FFmpeg/GStreamer plugin owners, the MXL
  team, and the external engine team behind the #11xx–#13xx issues; plus the short
  questionnaire to private users (Q-MIG-1) `[C5 §1.9]`. Their objections are answered in
  the documents before Phase 1 code starts.

**Phase 0 exit:** the ≤ 10 decisions answered; spikes written up and 13 §8 budgets
confirmed; instance params and the libmtl soname merged; the header compiles in CI; the
external review held and answered; the legacy gate green.

### 1.6 Phase 0.5 — the A/V answer on the legacy API (r3)

Review C5 showed that USER_TIMESTAMP + USER_PACING already yields exact RTP on ST20/ST30/
ST40 sessions, and that the gaps are small `[C5 §1.5, R05 §6]`. Phase 0.5 closes them on
the **legacy** API, weeks after the maintainer answers Q-TIME-0, and validates the timing
model with real users before 06 is frozen. The helper's contract is
[06 §15](06-timing-pacing-and-sync.md).

| Item | Detail |
|---|---|
| public `st_timeline_*` helper | exact rational anchor and index arithmetic over the legacy API (≈ 500 lines, the E2 math), returning the TAI and RTP values an app passes as USER_PACING/USER_TIMESTAMP for video frame k, audio sample n and ANC for frame k; unit-tested against the 13 §7 oracle. It replaces RxTxApp's 30-line `double` version (`tests/tools/RxTxApp/src/rxtx_app.c:673-703`) |
| `ST30P_TX_FLAG_USER_TIMESTAMP` | missing today (R05 §6) |
| ST40P USER_TIMESTAMP without USER_PACING | ST40P ignores the flag unless USER_PACING is also set today |
| sample-accurate audio start rule | the first audio packet's RTP is `floor(T0 · Fs)` on the helper's timeline; documented for st30p |
| SF-15 on the legacy pipelines | salvage PR #1610's `lib/src/new_api/mt_session_event.c` (106 lines: value-backed ring + eventfd) to replace the pipelines' tasklet mutex/condvar with an armed bit and a non-blocking wake written only when a waiter is armed; merged with credit to the PR #1610 authors |

**Phase 0.5 exit:** RxTxApp sends the maintainer's A/V/ANC example (06 §10.2) from one
file on the legacy API, and every packet's RTP matches the oracle on a capture; the
BLOCK_GET pipelines take no mutex on the tasklet, with latency no worse than today; the
legacy gate green.

## 2. Engines first (r3)

Revision 2 said "legacy defaults preserved behind old flags", which added a legacy/new
fork to each engine change without saying which users get what `[C5 §1.4]`. The rule
now:

- **bugfixes are on by default** for legacy users;
- **wire-visible changes** (RTP ±1 tick, default video RTP, ANC RTP, ST22 CBR, the
  linear read schedule) are **off by default on the legacy API** behind a new opt-in flag,
  and **on by default in the unified API**;
- every E-change carries a G-99 test: flag off → wire identical to the baseline; flag on
  → the oracle passes.

| # | Change ([06 §14](06-timing-pacing-and-sync.md)) | Kind | Legacy default | Legacy opt-in (names indicative) | Unified |
|---|---|---|---|---|---|
| E1 | media time and launch carried separately into `tv_*`; RTP from media time | wire-visible (default video RTP) | off; plumbing on (fixes metadata loss paths) | `ST20_TX_FLAG_RTP_FROM_MEDIA_TIME` | on |
| E2 | exact rational epoch math; `floor` for RTP | wire-visible (±1 tick at 1001 rates) | off | `*_TX_FLAG_EXACT_RTP` per family | on |
| E3 | admission at pick-up with per-frame status, reason and margins; bounded SEND_LATE; source kinds and `min_tx_delay` | reporting: fix; SEND_LATE bound: behaviour | reporting on (fields appended to the library-allocated `st_frame` and TX meta); SEND_LATE bound off | `*_TX_FLAG_BOUNDED_LATE` | on |
| E4 | enqueue and HW observed times; TX self-check counters | additive | on | — | on |
| E5 | linear read schedule for W/NL (SF-34); TLINE/2 for the second field; pre-fill cap | wire-visible (a compliance fix) | off | `ST20_TX_FLAG_LINEAR_SCHEDULE` | on |
| E6 | audio packet RTP from the sample index; launch offset; carry buffer | wire-visible when user timestamps are not contiguous | off (integer `samples_per_packet` is SF-35, already in PR #1770) | `ST30_TX_FLAG_RTP_FROM_SAMPLE_INDEX` | on |
| E7 | ANC RTP from media time; deterministic target in the window, inherited from the video's timeline (06 §5.5); empty keep-alive on underrun | wire-visible | off | `ST40_TX_FLAG_RTP_FROM_MEDIA_TIME`, `ST40_TX_FLAG_KEEPALIVE` | on |
| E8 | RX per-leg arrival times; exact `media_index` inverse; deadline force-complete; RTP offset and SENDER mode | additive; the deadline fixes RX stalls at stop; risk medium (06 §14) | arrival times and deadline on | — | on |
| E9 | explicit time sources; `CLOCK_TAI` validation; lock state that clears (SF-20); step events; published, slewed time base | fixes; publication gated by S7 | on | — | on |
| E10 | ST22 `rate_mode`: CBR padding (constant bytes and packets per frame, as ST 2110-22 requires) or VBR_MAX (today's behaviour); synchronous oversize check (`-MTL_ENOSPC`, reason `CODESTREAM_OVERSIZE`) | CBR is wire-visible | VBR_MAX (unchanged) | `ST22_TX_FLAG_CBR` | **CBR default**, VBR_MAX opt-in (flagged non-compliant) |
| E11 | lift `ST20_FB_MAX_COUNT = 8` (`include/st20_api.h:24`): frame arrays made dynamic in the engine | internal | legacy ops keep the limit (the constant is ABI) | — | `max_count` reported in the requirements |
| E12 | grow the ST40 RX meta array beyond `ST40_MAX_META = 20` (05 §11) | internal | legacy ops keep the limit | — | `max_packets` up to 255 |
| E13 | fastmeta rate from the associated video or explicit, launch offset, index parity (06 §14) | wire-visible (RTP from media time) | off | `ST41_TX_FLAG_RTP_FROM_MEDIA_TIME` | on |
| R1 | recovery on a worker; in-flight frames `DROPPED/RECOVERY` not `COMPLETE`; `sh_info` never touched (SF-12, SF-41) | fix | on | — | on |
| R2 | the slot interface in the pipelines gives legacy callbacks exactly-once `notify_frame_done` (SF-05, SF-38, SF-39) | fix | on | — | on |

Order: E2 first (Phase 0.5 needs its math), then the additive E4/E8/E9, then E1/E3 (needed
for ST20 results at Phase 1 exit), E11 and E12 in Phase 2, and E5/E6/E7/E10/E13 before Phase 3 exit.

**Pod safety (addendum K).** The fixes EK1–EK18 of [16 §11](16-kubernetes-and-crash-safety.md) are bug
fixes, on by default for legacy users too, with two exceptions that change behaviour.
Refusing no-IOMMU (EK8) only warns on the legacy API and refuses on the unified one unless
`instance.allow_noiommu` is set. The non-blocking open (EK18) is unified-only, because
legacy `mtl_init` callers expect to return with links up. EK1, EK2, EK6 and EK7 come first:
they remove a use-after-free, CPU theft between pods, and a crash loop on a valid
configuration (SF-56, SF-57, SF-61).

**The legacy gate.** Every phase, and every E-change, passes the legacy KahawaiTest suite
(mandatory level, `--pacing_way` default and `tsc`) and the acceptance smoke suite with
the legacy defaults, plus a pcap diff against the pre-change baseline for the wire-visible
changes (G-99).

## 3. Phases 1–6

### 3.1 Phase 1 — core, substrate, video over st20p, library pools

Scope:

- handles (typed, ID 0 = null, grow-only chunked tables, random generations), session
  state machine with `discard_queued`, start/stop (DRAIN/FLUSH), deferred destroy with
  the stalled-queue reset, `interrupt`/`uninterrupt`, the command channel (immediate and
  boundary commands, ack timeout), `MTL_E*` codes, `mtl_last_error`,
  `enum mtl_state_reason`, debug-build call-class enforcement, the inline-safe DP subset
  for busy-loop threads (04 §3.3);
- lease table + reaper, completion modes (NONE default for library pools), unread-results
  ring, `blocked_on` (incl. `APP_LEASES`), private CQ/EQ, **shared EQs with a
  subscription mask** (r3, from Phase 2), portable wait objects, try-wait 1/0/<0, waiters
  per wait target, the waker, with W2 below 1 ms (if M6 accepts it) and in `TASKLET_THREAD`
  mode;
- the **slot interface in st20p TX/RX** (hold, submit with `seq`, done hook, reclaim,
  rejected-at-pick-up, idle descriptor cleanup) and the **RX deadline hook**, which
  carries the due time of the RX timing model (06 §11.7);
- ST20 TX/RX adapter over st20p with library pools and AUTO media mode, results with the
  statuses the engine can give (ON_TIME / DROPPED / FLUSHED);
- **the test substrate** (r3): the null backend, the test time source and
  `mtl_debug_inject`, and `nicctl.sh vf_link` in `gtest.sh`;
- L2-owned counters as per-writer blocks, cumulative only; gauges by scan; session names
  and `mtl_instance_list_sessions`; `mtl_session_get_info`; dry-run query for video;
- L4: `mtl_simple.h` (r3), `mtl_instance_open_simple`, `mtl_flow_parse`, `mtl_fps_parse`;
- a **reference Python wrapper** over the installed DSO, with `mtl_lease_copy_in/out`,
  tested on the null backend (r3);
- ABI: the experimental DSO `libmtl_unified.so.0.<rev>` with node
  `MTL_UNIFIED_EXPERIMENTAL`, soname bumped on every incompatible change; `struct_size`,
  `*_init()`, size checks.

Exit: G-01…G-09, G-29…G-33, G-35, G-38, G-39 (PMD backend), G-40 (L2 counters), G-42,
G-43, G-45 (ST20), G-46…G-52, G-57 (for the statuses, reasons and codes in Phase 1 scope),
G-64, G-65, G-69…G-74, G-77, G-79, G-80, G-81 (UB), G-82, G-88, G-92, G-93, G-95, G-97
green; the 13 §8 budgets met for ST20; one sample, the Python wrapper and a GStreamer st20
sink prototype on the new API with latency no worse than today; an opt-in RxTxApp path.

### 3.2 Phase 2 — all essences, operators

Scope:

- the slot interface in st22p, st30p, st40p; adapters for cvideo, audio, anc and the
  fastmeta `st41` session (frame RX added);
- engine counters converted to per-writer blocks; the admin CPU-busy scan without the
  session spinlock; one stats schema; per-leg counters;
- **the link monitor** (r3, from Phase 5): LSC interrupt where the PMD supports it,
  otherwise the admin thread polls `rte_eth_link_get_nowait` every 100 ms; netlink on the
  kernel-socket and AF_XDP backends (09 §7.2). It is a prerequisite of `LEG_STATE`
  `[C5 §8.2]`. A leg that goes down is skipped (`pkts_skipped` per leg) and its queue is
  reset through the stalled-queue path; `SESSION_RECOVERY` / `LEG_STATE` / `RX_SIGNAL` /
  `FLOW_STATE` events;
- legs reserved regardless of link, `mtl_session_set_leg_enabled`, admin × oper state;
- `mtl_session_update_flows`, all-or-nothing across legs, with `NOW | AT_TAI |
  AT_MEDIA_INDEX` activation (D-26 revised); it and `set_leg_enabled` are boundary
  commands on the Phase 1 command channel;
- `mtl_session_reconfigure` in STOPPED for library pools;
- capability query and requested-vs-granted reporting for pacing, data path, DMA (06 §6,
  09 §6); dry-run query for every essence; `mtl_port_get_capacity` and
  `MTL_QUERY_CHECK_CAPACITY`;
- the refcounted default instance `mtl_instance_acquire_default` / `mtl_instance_release`
  over the Phase 0 params, with the merge table (runtime `mtl_port_open` is Phase 6);
- MtlManager: reconnect with re-registration, re-announce of held lcores, and the pod
  safety of EK5 ([16 §10.1](16-kubernetes-and-crash-safety.md));
- shared CQs (Q-CMP-5) with their ready summary (04 §4.3) and the L4 CQ dispatcher
  (Q-CMP-2);
- ST40 empty keep-alive and ST41 keep-alive underrun defaults in the unified API;
- E11 and E12; `nicctl.sh vf_reset` in `gtest.sh`.

Exit: every essence passes the Phase 1 guarantees (G-45 for all five); G-34 for the
capabilities introduced; G-40 for engine counters; G-61; G-75, G-87, G-89, G-90, G-91,
G-98; **G-27 measured** on at least one NIC × pacing class (r3, moved from Phase 1); no
silent downgrade remains for the covered cases (G-13 for REQUIRE, reporting for PREFER);
an NMOS-style demo activates new flows on both legs at one TAI instant and disables one
leg without a frame from mixed senders. **Go/no-go on the Phase 6 re-base**, from S3 and
the Phase 1–2 measurements.

### 3.3 Phase 3 — timing core in L2

Scope: media modes (AUTO / INDEX / TAI with `media_time_offset_ns`); source kinds and
`min_tx_delay`; admission with per-unit results and margins; bounded SEND_LATE; slot
hints; observed times and the windowed TX self-check; audio sample-accurate submission,
carry buffer and absorb window; ANC windows; cvideo `rate_mode`; time sources and the
published time base; lazily anchored, named and epoch timelines with the r3 T0 formula
and `preroll_ns`; TX and RX groups (joining a running group); clock-step policies; the RX
timing model (06 §11: timeline binding, `media_index`, `link_offset_ns`, `mtl_rx_align`,
over the Phase 1 RX hook); CAPTURE
NEAREST snapping; `discard_queued` with REBASE; `mtl_tx_write`, `mtl_tx_withdraw`,
`mtl_time_convert`, `mtl_time_cross_timestamp`; the index helpers. The engine side is the
engines track (§2).

Exit: G-17…G-26, G-28, G-53, G-57 (the remaining statuses, reasons and codes), G-59 (UB),
G-62, G-63, G-66, G-67, G-68, G-76, G-85, G-86, G-94 green (U/UB); the maintainer's
A/V/ANC example (06 §10.2) runs through a group on the new API and its RTP matches the
oracle; EBU LIST narrow passes on at least one NIC × pacing class (G-27 regression).

### 3.4 Phase 4 — memory

Scope: regions (`mtl_mem_alloc` / `mtl_mem_import`; page-aligned; lazy per-device mapping
and `mtl_mem_map_device`; region budget; backing detection), refcounts, buffers and
layouts (`stride ≥ row_bytes` DIRECT), attached pools TX/RX, `acquire_buffer`, `BY_INDEX`,
RX hold counts (`mtl_tx_submission.hold`, engine fix M10), `mtl_rx_transfer`,
`mtl_session_get_pool_region`, the requirements query, data-path policy and per-unit path,
the engine fixes M1–M10 of 05 §11 (M9: linesize and RX `ext_frames[]` set at attach), RX overflow and fill policies, the export-pool rules, reconfigure
re-validation of attached pools, GPU pinned host memory import; **`MTL_POOL_DYNAMIC` and
`mtl_tx_acquire_dynamic`** and **early `SOURCE_RELEASED`** for COPY/CONVERT paths (r3,
both from Phase 6) `[C5 §6.3, §6.13]`; the L4 FourCC / GStreamer / FFmpeg pix_fmt mapping;
and, because they exist today (port first, 11 §R4.15), per-frame TX addresses
(`mtl_tx_acquire_layout`), `query_ext_frame` (`mtl_rx_provide`) and GPU frame buffers
(`mtl_mem_import_device`), whose declarations are still under `MTL_LATER`.

Exit: G-10…G-16 (G-16 BE until its I job), G-36, G-37, G-56, G-60, G-78, G-83, G-84, G-96
green; FFmpeg RX zero-copy (`av_buffer_create` over leases) and GStreamer TX export-pool
prototypes with zero copies shown by the path counters; the MXL POC on imported memory.

### 3.5 Phase 5 — robustness at the I tier

Scope: VF reset handling (Q-LIFE-10), recovery and auto-detect on workers with the
quiesce handshake (engine part in R1), stalled-queue destroy on real VFs, the tasklet log
ring, new USDT probes, RX link offset and presentation (E8 remainder), the fault matrix
orchestrated in `gtest.sh`.

Exit: G-41, G-44, G-54, G-55, G-58 at every tier they list; G-81 at I; the fault-injection
matrix (13 §6) green at the I tier for every row marked P there.

### 3.6 Phase 6 — advanced and migration

Scope, each independently schedulable:

- progressive TX/RX (06 §9), with the session-layer adapter for ST20 line mode;
- `REPEAT_LAST` underrun;
- packet-chunk sessions (RTP level, Rivermax-like);
- runtime `mtl_port_open(mt, const struct mtl_port_params*, …)` and a capacity
  reservation object;
- FFmpeg and GStreamer plugin rewrites on the unified API;
- **the legacy pipeline public functions re-based on L2** as thin wrappers over the slot
  interface (r3: a commitment, subject only to the Phase 2 go/no-go) `[C5 §1.6]`;
- ABI freeze: the unified symbols move to `MTL_1.0` in libmtl, and the deprecation policy
  (§7) starts for the legacy APIs.

Exit (ABI freeze): G-51 for `MTL_1.0`; the FFmpeg and GStreamer plugins on the new API
pass the acceptance smoke suite.

### 3.7 Port first: where the addenda land (Kubernetes, NMOS, IPMX)

**The rule (2026-10-01).** Phases 1–6 port what MTL does today onto the unified API. New
transport features are designed in the headers so the API will not need to change for them,
but nothing in Phases 0–6 spends effort on them. The Kubernetes work stays in: it is the
safe lifecycle of today's functionality (shutdown, crash safety, CPUs, probes).

| Item | Phase | Why there |
|---|---|---|
| `mtl_instance_close(mt, timeout)`, `mtl_instance_shutdown`, R4 handle slots, R8, health, environment reasons ([16](16-kubernetes-and-crash-safety.md)) | 1 | the instance object is built-in Phase 1; shutdown is its destroy path |
| CPU arbitration without SysV, `runtime_dir`, pinned threads, time read-only in pods | engines track (EK) + 1 | engine fixes; the options arrive with the instance |
| MtlManager pod safety (EK5) | 2 | with the manager reconnect already planned there |
| device removal and reset as states (EK14) | 2 | with the link monitor and legs |
| the atomic `mtl_session_update` of today's `update_destination` / `update_source`, with its planned instant, `status.update_*`, and the switch at the slot boundary by the clock ([17 §2.2](17-nmos-and-ipmx.md)) | 2 | it replaces an existing call; the contract costs nothing extra once the update is atomic |
| per-leg enable and disable (today's redundancy legs) | 2 | already planned there |
| the rest of the NMOS contract: `REAPPLY`, `DRY_RUN`, cancel, mute (every leg disabled), reserved legs, port changes while running, R options at the boundary, `rx.join_lead_ns`; `mtl_sdp.h` | **7** | new features; an NMOS Node can be built on Phase 2 with stop, update and start |
| IPMX: `MTL_TIME_SOURCE_FREERUN` and AUTO at runtime, `MTL_MEDIA_SENDER`, `MTL_SUBMIT_SENDER_TIME`, `session.profile`, `mtl_rtcp.h`, IGMPv2, `tx.precede`, `video.vtotal`/`htotal`, `cvideo.max_bitrate_bps`, RX header extensions (SF-68) | **7** | MTL as an IPMX transport is a later goal |
| `mtl_crypto.h` (PEP) | **7**, after a cost spike | IPMX-only |

### 3.8 Phase 7 — NMOS and IPMX transport (later)

Starts after the ABI freeze (Phase 6 exit), unless the maintainer pulls an item forward.
Scope: the Phase 7 rows above, in this order: the NMOS contract and `mtl_sdp.h`; IPMX timing
(free run, SENDER mode) and `mtl_rtcp.h`; the IPMX profile and wire details; then PEP after
its spike. The declarations are under `MTL_LATER` in the headers today (`mtl_sdp.h`,
`mtl_rtcp.h`, `mtl_crypto.h`), and the fields and options they need are reserved, so the
freeze does not block them. Open items the verification of the IPMX design left for this
phase are in [reviews/RV-verification.md](reviews/RV-verification.md) (RN-17, RN-18, RN-20,
RN-21, RN-24, RN-26, RN-30).

Effort, not yet reviewed **[estimate]**: EK1–EK21 about 4–6 EM and MtlManager pod safety 1–2
EM, inside Phases 1–2 (about 5–8 EM added to §4's total). Phase 7 is outside the total:
the NMOS contract and `mtl_sdp.h` 2–3 EM, IPMX timing and RTCP 4–6 EM, PEP 2–4 EM.

## 4. Effort and staffing (r3)

C5 estimated the revision 2 plan at ≈ 50–75 engineer-months (EM), using one EM ≈ 1.0–1.5
kLOC of library code landed with unit tests in this codebase `[C5 §1.1]`. The estimate is
accepted as order-of-magnitude. The commit figures it rests on were re-checked at
`545a266a`: 610 commits in 2026; 133 commits touching `lib/` + `include/` in the last six
months (≈ 22 a month; +8.5 k/−10.2 k lines by `git log --shortstat`, C5 had +7.3 k/−9.2 k
over a slightly different window); one author wrote 74 of those 133.

| Phase | C5 (EM) | r3 (EM) | Why it changed |
|---|---|---|---|
| 0 | 2–3 | 3–5 | + instance params, libmtl soname, S0, S7, substrate design, external review |
| 0.5 | — | 1.5–2.5 | new |
| engines track (E1–E13, R1, R2) | inside 3 | 6–10 | E1–E10 moved out of Phase 3; legacy flags and the legacy gate added; E11 and E12 from Phase 2 |
| 1 | 8–12 | 11–16 | + null backend, test clock, debug inject, RX deadline hook, shared EQ, W2 auto, Python wrapper, L4 simple layer, stalled-queue destroy |
| 2 | 6–9 | 7–10 | + link monitor, legs, atomic `update_flows`, capacity, reconfigure, manager reconnect; − shared EQ |
| 3 | 9–14 | 4–6 | L2 timing only; the engine work is in the engines track |
| 4 | 5–8 | 6–10 | + `MTL_POOL_DYNAMIC`, early `SOURCE_RELEASED`, hold counts |
| 5 | 5–8 | 4–7 | − link monitor |
| 6 | 12–20 | 11–18 | − dynamic pools and `SOURCE_RELEASED`; the re-base is committed |
| **total** | **≈ 50–75** | **≈ 55–85** | revision 3 adds the substrate, the operator surface and the legacy forks |

| Staffing scenario | Whole plan | First user result (Phase 0.5) | Legacy users served (0 + 0.5 + engines) | "Stop after Phase 2" (all essences, operators, no L2 timing) |
|---|---|---|---|---|
| 1.5 FTE (≈ today's library throughput) | 3.1–4.7 years | 6–10 weeks after Q-TIME-0 is answered, one engineer, parallel to Phase 0 | 7–12 months | 1.6–2.4 years |
| 3 FTE dedicated | 18–28 months | the same | 4–6 months | 10–15 months |

Phase 0.5 is deliberately small so the first artefact does not wait for the plan's
size to be accepted.

## 5. Success criteria per phase (r3)

In user terms `[C5 §1.9]`; the engineering exit criteria are in §1–§3 and
[01 §7](01-goals-and-requirements.md) repeats this table.

| Phase | Done when a user can … | Evidence |
|---|---|---|
| 0 | read a compiling header and see the external reviewers' objections answered | review notes; `sketch/check.sh` green |
| 0.5 | send A/V/ANC from one file with RxTxApp on the legacy API with exact RTP | oracle on a capture |
| engines | enable each engine fix with one legacy flag; see no wire change without it | G-99 pcap diffs |
| 1 | run a sample and a GStreamer st20 sink on the new API with ≤ today's latency; run the doc examples and the Python wrapper with no NIC | latency measurement; null-backend CI job |
| 2 | send/receive all five essences; do an IS-05-style atomic activation; disable/enable a leg; get a capacity answer that admission confirms | I tier; G-75, G-89, G-91 |
| 3 | send the maintainer's A/V/ANC example through a group on the new API; pass EBU LIST narrow | oracle; M tier |
| 4 | receive zero-copy in FFmpeg and send from a GStreamer export pool with zero copies | path counters |
| 5 | pull a link, reset a VF, step PTP and kill MtlManager in CI with the documented outcomes | I-tier fault matrix |
| 6 | use the FFmpeg and GStreamer plugins on the new API; build against a frozen ABI | acceptance smoke; G-51 |

## 6. Programme deliverables (r3)

**Programmer's guide.** The design documents are not a guide `[C5 §1.10]`. A user-facing
guide under `doc/` grows with the phases, built from the compiled `sketch/examples/`:

1. First session with `mtl_simple.h`, on the null backend, then on a VF.
2. Sessions, leases and results: acquire, submit, reap; dequeue, release; completion modes.
3. Errors, states and shutdown: the `MTL_E*` table, interrupt, stop, destroy.
4. Timing: media time vs launch, the epoch timeline, groups, live capture (TAI + CAPTURE),
   playout (INDEX), receivers (RX timing).
5. Memory: regions, imports, zero-copy, holds and forwarding.
6. Framework recipes: GStreamer (export pool, LATENCY query, `unlock`), FFmpeg, OBS.
7. Operations: names, stats, events, capacity, NMOS activation, legs.
8. Testing your application: the null backend, the test clock, fault injection.
9. Migration from st20p/st30p/st40p, table by table.
10. Deployment and security ([15](15-security-and-deployment.md)).

**Other deliverables:** the defaults table (09) with its CI check; the reference Python
wrapper; the security and deployment document ([15](15-security-and-deployment.md));
Windows compile-only CI for the new headers (15 §8.4).

## 7. Deprecation and release policy (r3)

Tags follow `vYY.MM` (v25.02, v25.12-rc1, v26.01; PR #1768 prepares v26.09).

- Until the ABI freeze the unified API lives only in `libmtl_unified.so.0.<rev>`; its
  soname changes on every incompatible change, so `ld.so` rejects stale binaries. There is
  no compatibility promise and no deprecation period for it.
- At the ABI freeze release, the unified API moves to `MTL_1.0` and the legacy APIs it
  replaces are marked deprecated (a `MTL_DEPRECATED` attribute plus the release notes).
- A deprecated legacy API is removed **no earlier than two releases (two `vYY.MM` tags)
  after** the release that deprecated it, and only if the migration table (10 §12) covers
  every use in-tree and the maintainer signs off.
- The legacy session-level APIs (`st20_*`, …) are frozen from the ABI freeze (Q-ABI-4):
  bugfixes only.
- Every release note lists the legacy opt-in flags introduced by the engines track and
  which are candidates to become default at the ABI freeze.

## 8. How work flows through the repository's gates

The repository's six-gate loop applies to every phase (`.github/copilot-instructions.md`):

| Gate | For this effort |
|---|---|
| 0 knowledge | the relevant design document here + the knowledge-base section |
| 1–2 failing test | the guarantee's contract test at the cheapest tier (13), on the null backend where possible |
| 3–4 implement, green build | `mtl-developer`; `./format-coding.sh`; `./build.sh unit` |
| 5 review | `mtl-reviewer` on the saved diff |
| 6 hardware | `mtl-system-admin` / KahawaiTest for data-plane changes, plus the legacy gate |

## 9. Risks

| Risk | Phase | Mitigation |
|---|---|---|
| The pipelines need the slot interface (held slot, once-only completion, reclaim, rejected-at-pick-up, last-packet hook, RX force-complete) — more than "an adapter" | 1–2 | budgeted (§4); UB tests through the slot interface; S3 |
| TX completion latency ≈ `nb_tx_desc` packets on the chain path, unbounded while idle | 1 | idle descriptor cleanup; the expected latency reported; S6 |
| Lost wake-ups if the fences are wrong | 1 | seq_cst fences (04 §5.1); litmus test G-52 |
| Waker CPU cost, timer slack, inherited affinity | 1 | deadline-driven waker, per-scheduler wake words, explicit affinity, W2 below 1 ms (M6); S1; the 13 §8 budget |
| Tasklets `trylock` the session spinlock, so every app/admin/stat holder makes them skip; stats have several writers | 1–2 | per-writer counters; gauges by scan; no spinlock on read paths |
| Backend syscalls on tasklets (kernel socket, AF_XDP) and PMD reads for built-in PTP time | 1–5 | G-39 scoped per backend; published time base for every source (S7) |
| Engine fixes change the legacy wire | engines | opt-in flags; G-99 pcap diffs; the legacy gate every phase |
| The legacy/new fork multiplies the pacing matrix `[C5 §1.4]` | engines–3 | exhaustive at U/UB against the oracle; the I tier runs only the two default columns per pacing class |
| **CI runner capacity (r3):** the I tier needs NIC runners; the fault steps (link down, VF reset, manager kill) disrupt other jobs on a shared runner; the M tier is lab-only | 1–5 | a dedicated NIC runner (or a serialised job) for fault steps from Phase 2; the null-backend jobs run on ordinary runners; M-tier runs scheduled per phase exit, not per PR |
| **Reviewer load (r3):** one author wrote 74 of the 133 `lib/`+`include/` commits of the last six months; they would also review most of this plan's code `[C5 §1.10]` | all | CODEOWNERS with a second reviewer for `lib/src/unified/` and the slot interface; PRs ≤ ≈ 1.5 k lines (the largest library change this year); the `mtl-reviewer` gate before human review |
| Live-capture users hit infeasible timing (L, JT-NM windows) | 3 | source kinds, `timing_warning` with the shortfall, `get_info` values |
| Scope creep (packet level, device memory) delays the core | all | reserved shapes only; Phase 6 items are independent |
| Two APIs to maintain during the transition | 1–6 | engines first (fixes land once); legacy frozen at the freeze; the committed Phase 6 re-base; the deprecation policy (§7) |
| Private downstream users depend on behaviour not visible in-tree | 0 | the external review and questionnaire (Q-MIG-1) as the Phase 0 exit |
| The plan is not staffed at the size it needs | all | Phase 0.5 and the engines track pay legacy users on their own; the "stop after Phase 2" scenario is a coherent product |
