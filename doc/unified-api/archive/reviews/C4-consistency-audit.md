# C4 — Consistency, traceability and completeness audit

| | |
|---|---|
| Scope | `doc/unified-api/` — README, 00–14, OPEN-QUESTIONS, DECISIONS, side-findings, research/00–13 |
| Baseline | `main` @ `545a266a` (checked with `git show HEAD:path`); the working tree has uncommitted edits in 20 `lib/` files |
| Method | full read of every document; `grep`/`awk`/`python3` scripts for IDs, anchors, links, tables, line length, list indentation, and `path:line` citations |
| Labels | **[verified]** = checked in the documents or in code at HEAD; **[inferred]** = follows from what was read, not executed |
| Linters | `markdownlint`, `markdownlint-cli2`, `mdl` not installed; checks were scripted against `.github/linters/.markdown-lint.yml` (MD007 indent 2, MD013 400, MD026 `.,;:!`) |

## Summary

| Section | Findings |
|---|---|
| 1. Cross-document consistency | 44 (C-01 … C-44) |
| 2. Traceability | 50 MUST/SHOULD requirements: 26 fully covered, 12 partial, 12 with no guarantee (6 of them MUST); 4 guarantees in no phase exit; 7 B-questions without a Proposed decision; 0 broken Q-IDs or anchors; 2 more traceability defects (T-01, T-02) |
| 3. Research coverage | 176 research questions checked; 11 important ones dropped or only implicitly decided (proposed wording below) |
| 4. Mode coverage | 22 easily-forgotten modes: 13 handled, 3 deferred, 3 explicitly dropped, 3 silently missing; plus 8 matrix features silently missing |
| 5. Claims vs research/code | 36 claims spot-checked: 27 match, 1 stale (fixed before the baseline), 8 cite working-tree lines instead of HEAD; an automated sweep found 45 of 72 citations into edited files whose text differs between HEAD and the working tree |
| 6. Markdown hygiene | 0 broken links, 0 bad anchors, 0 heading-punctuation hits; 5 lines > 400; 2 tables with an extra column; 3 research notes with under-indented sub-bullets (109 lines) |

## 1. Cross-document consistency

Each finding: where, what is wrong, and the exact edit. Line numbers are in the current files.

### 1.1 States, statuses, policies and field names

- **C-01** [verified] Session states. `01-goals-and-requirements.md:176` (R-LIFE-1) lists `CREATED → READY →
  (ARMED) → RUNNING → DRAINING → STOPPED, plus ERROR`. `03-object-model-and-lifecycle.md:94-113` has no READY
  state, but it has FLUSHING, says STOPPED ≡ CREATED, and `03:287` adds a DESTROYING state that is in neither
  the diagram nor the `03 §3.2` table. `08-observability.md:57` has `uint32_t state` with no enum anywhere.
  - Fix `01:176`: "Session states CREATED → (ARMED) → RUNNING → DRAINING / FLUSHING → STOPPED (≡ CREATED with history), plus ERROR and a transient DESTROYING; start/stop reversible; destroy from any state."
  - Fix `03:113`: append "DESTROYING (transient): every call except `destroy` returns `-ESHUTDOWN`; a second `destroy` returns `-EBUSY`." Add a DESTROYING column to the `03 §3.2` table with those values.
  - Fix `10 §1`: add `enum mtl_session_state { MTL_STATE_CREATED, MTL_STATE_ARMED, MTL_STATE_RUNNING, MTL_STATE_DRAINING, MTL_STATE_FLUSHING, MTL_STATE_ERROR, MTL_STATE_DESTROYING };`, and say whether STOPPED is a separate value or CREATED.
- **C-02** [verified] Late-policy name. `06-timing-pacing-and-sync.md:284` uses `MTL_LATE_SEND`. `07:62`, `09:99`, `12:69` and `DECISIONS.md:26` use `SEND_LATE`. Fix `06:284`: `MTL_LATE_SEND` → `MTL_LATE_SEND_LATE`.
- **C-03** [verified] Skipped-slot field. `06:285` uses `slots_skipped = n` and `06:502` uses `slots_skipped`, but the struct field is `slots_skipped_before` (`06:322`, `08:32`). Fix `06:285` and `06:502`: use `slots_skipped_before`.
- **C-04** [verified] How EXACT non-compliance is reported. `06:227` says "flagged `non_compliant_timing` in the result", and `06:323` has the flag `NON_COMPLIANT_TIMING`. `07:62` instead makes such a unit status `LATE` with reason `EXACT_NON_COMPLIANT`, so an on-time EXACT unit would be reported as late.
  - Fix `07:62`: meaning "sent, but after its deadline (policy `SEND_LATE`)"; reasons "`TOO_LATE`".
  - Add under the `07 §1.1` table: "An `EXACT` launch that meets its deadline is `ON_TIME` with `timing.flags & MTL_TX_TIMING_NON_COMPLIANT` set."
- **C-05** [verified] Definition of ON_TIME. `07:61` says "first packet no later than its deadline +
  tolerance". `06:317` defines `deadline` as the "latest pick-up that meets the slot" (an admission instant,
  not a wire instant), and "tolerance" is defined nowhere. Fix `07:61`: "picked up no later than
  `timing.deadline`; wire accuracy (`observed_first − scheduled_first`) is reported but does not change the
  status". Also open the question in §3 (Q-CMP-6).
- **C-06** [verified] Two different margins. The `06:269` diagram defines `margin_submit = deadline_pickup − submit_time`, and `06:321` defines `margin_ns = deadline − pickup_time`. `06:327` explains margin as "how close was I?", which is the submitter's view. `08:73` histograms `margin_ns`. Fix: keep one quantity the application can act on.
  - `06:321`: `int64_t margin_ns; /* deadline − submitted: positive = submitted early enough */`
  - Optionally add `int64_t pickup_slack_ns; /* deadline − pickup */`, and relabel the diagram at `06:269` `margin_ns = deadline − submitted`.
- **C-07** [verified] Source-release field names. `05-memory-and-buffers.md:241` uses `source_released_tai` and `transport_done_tai`. `07:55` has `struct mtl_time source_released`, and the transport-done instant is `timing.observed_last` (`06:320`). Fix `05:241`: "(`source_released` and `timing.observed_last`)".
- **C-08** [verified] RX overflow field. `03:240` uses `rx_overflow_policy` with bare `DROP_NEW` and `RECLAIM_OLDEST_READY`. `05:165` and `10:202` use `pool.rx_overflow` with `MTL_RX_DROP_NEW` and `MTL_RX_RECLAIM_OLDEST_READY`. Fix `03:240-241`: "`pool.rx_overflow`: `MTL_RX_DROP_NEW` … or `MTL_RX_RECLAIM_OLDEST_READY`".
- **C-09** [verified] Lease-state list. `05:21-23` lists `FREE / APP_WRITABLE / QUEUED / IN_FLIGHT / READY / APP_READING`, so it drops DONE and RECEIVING (`03 §4.1-4.2`, `08:77`). Fix `05:22-23`: "TX: FREE / APP_WRITABLE / QUEUED / IN_FLIGHT / DONE; RX: FREE / RECEIVING / READY / APP_READING".
- **C-10** [verified] The update verb has two names. `02:224`, `03:132`, `04:61` and `04:257` use "update destination / source" (`mtl_session_update_destination` / `source`). `09:65`, `09:185` and `10:61` use `mtl_session_update_flow`.
  - Fix `04:257`: row name `mtl_session_update_flow`.
  - Fix `04:61`: "`mtl_session_update_flow`, `stop`, `detach` …".
  - Fix `03:132` and `02:224`: "update flow (destination / source)".
- **C-11** [verified] CQ binding call. `07:124` says `mtl_cq_bind`, but `07:154` and `10:56` say `mtl_session_bind_cq`. Fix `07:124`: `mtl_session_bind_cq`.
- **C-12** [verified] Data-path signatures disagree.
  - `02:166` has `mtl_tx_acquire(s,&buf,&slot,t)` and `02:211` has `mtl_rx_dequeue(s,&buf,&res,t)`.
  - `07:159` has `mtl_rx_dequeue(s, &buf, &unit, timeout)`.
  - `10:70-79` and `12:86` use `(s, timeout_ns, &buf, &view, &hint)` and `(s, timeout_ns, &buf, &view, &unit)`.
  - Fix, using 10 as the reference: `02:166` → `mtl_tx_acquire(s,t,&buf,&view,&hint)`; `02:211` → `mtl_rx_dequeue(s,t,&buf,&view,&unit)`; `07:159` → `mtl_rx_dequeue(s, timeout, &buf, &view, &unit)`.
  - `mtl_tx_reap` is consistent: `07:160` matches `10:75`.
- **C-13** [verified] `mtl_group_start` arity. `06:422` passes two arguments, but `10:45` declares three (`…, struct mtl_time* chosen`). Fix `06:422`: add `, &t0` and declare `struct mtl_time t0;` on the line above.
- **C-14** [verified] `mtl_session_destroy` arity. `03:272` calls `mtl_session_destroy(s)`, but `10:60` declares `(s, uint32_t flags)` and `03:305` names `MTL_DESTROY_FORCE`. Fix `03:272`: `mtl_session_destroy(s, 0)`.
- **C-15** [verified] Instance creation. `02:146` says the instance is created by `mtl_instance_create`. `03:30` and `03:321`, `11:97`, and the Q-ARCH-2 recommendation (a) at `OPEN-QUESTIONS.md:145` all say v1 uses `mtl_init` / `mtl_handle`. Fix `02:146`: "`mtl_init` (v1, `mtl_handle`); versioned init later (Q-ARCH-2, Q-ABI-2)".
- **C-16** [verified] Region naming. The diagrams at `02:131` and `03:17` use `mtl_mem_region`, `05:291` uses `mtl_region`, and the handle is `mtl_region_h` (`03:53`, `05:65`, `10:19`). Fix: replace `mtl_mem_region` with `mtl_region` at `02:131` and `03:17`.
- **C-17** [verified] Memory-domain names. `05:56` has `MTL_MEM_HOST | MTL_MEM_HOST_HUGEPAGE | later: DMABUF, DEVICE`, `05:43` has `DMABUF/DEVICE`, and `05:281` introduces `DEVICE_SHARED`. Fix `05:56`: "`… | later: MTL_MEM_DMABUF, MTL_MEM_DEVICE, MTL_MEM_DEVICE_SHARED (Level Zero shared USM, COPY-only)`".
- **C-18** [verified] Progressive phase. `09:72` and `09:197` say "implementation Phase 5". `14:95` and `00:94` put progressive in Phase 6. Fix `09:72` and `09:197`: "Phase 6".

### 1.2 Handles, ABI rules and struct shapes

- **C-19** [verified] Handle bit layout overflows 64 bits. `03:59` has `type:8 | reserved:8 | index:16..24 | generation:24..32`, which can total 72 bits, and `03:81` says "32-bit compare". Fix `03:59`: `object id = | type:8 | reserved:8 | index:16 | generation:32 |` (64 bits).
- **C-20** [inferred] One type for buffers and leases. `mtl_buffer_h` is both the object that
  `mtl_buffer_create` / `mtl_buffer_destroy` / `attach` use (`05:117-118`, `10:173-182`) and the lease that
  `acquire` returns with a fresh generation (`03:71-75`). But `03:60` puts a session index in every buffer ID,
  while buffers are created before any session attach. Fix: add to `03 §2.1`: "`mtl_buffer_create` returns a
  *base* handle (no session, generation 0). `acquire`/`dequeue` return a *lease* handle derived from it
  (session index + lease generation). CP buffer calls accept either; DP calls accept only a current lease."
  Alternatively introduce `mtl_lease_h`.
- **C-21** [verified] Flag types against R-ABI-1 (`01:182`) and `11:56` ("Flags are `uint64_t`").
  - Input flags are 32-bit in 5 places: `05:62` `mtl_mem_desc.flags`, `06:159` `mtl_tx_submission.flags`, `09:63` `mtl_flow.flow_flags`, `10:60` `mtl_session_destroy(…, uint32_t flags)`, `10:73` `mtl_tx_publish(…, uint32_t flags)`.
  - Output records use narrower flags: `06:49` (`uint16_t`), `06:323` (`uint32_t`), `07:32` (`uint16_t`).
  - Fix: change the five inputs to `uint64_t`, and amend `11:56`: "Input flag fields and flag arguments are `uint64_t` with `#define` constants; compact output records (`mtl_time`, CQ header, timing) may use narrower flag fields."
- **C-22** [verified] `09:165-168` puts `enum mtl_req` fields in a public struct, which breaks `11:54` rule 5 ("Enumerated fields are `uint32_t` in structs"). Fix: `uint32_t hw_pacing; /* enum mtl_req */` and likewise for `direct_path`, `dma_offload` and `hw_timestamps`.
- **C-23** [verified] Growable structs embedded mid-struct, which breaks `11:52-53` rule 4.
  - `09:21-27` embeds `struct mtl_flow flows[MTL_MAX_LEGS]` (which has `struct_size`, `09:55`), `mtl_pool_config` (`05:161`) and `mtl_capability_request` (`09:164`) by value, none of them last.
  - `08:59-61` embeds the stats blocks by value.
  - `08:72` and `08:74` size arrays by growable enums (`units_dropped[reason]`, `pkts_rejected[…]`).
  - Fix: either remove `struct_size` from embedded sub-structs and give each a fixed `uint64_t reserved[4]` tail, or amend rule 4 to allow that. Size reason arrays with a frozen constant (`MTL_TX_REASON_SLOTS 16`).
- **C-24** [verified] Out-structs without `struct_size`, against R-ABI-1 (`01:182`) and `11:43`.
  `mtl_slot_hint` (`06:335`) is written through a pointer by `mtl_tx_acquire`. `mtl_buffer_view` and
  `mtl_rx_timing` are used (`10:71`, `07:77`) but never defined. Fix: add `uint32_t struct_size;` as the first
  field of `mtl_slot_hint`, and define `mtl_buffer_view` and `mtl_rx_timing` in `05 §4` and `06 §11`. Or list
  the exempt value types (`mtl_time`, `mtl_rational`, `mtl_media_time`, `mtl_launch`, `mtl_tx_timing`)
  explicitly in `11:43` rule 1.
- **C-25** [verified] The size is carried in two places, and in one place it is missing.
  - `08:65` and `10:64-66` take `size_t size` although the struct already starts with `struct_size` (`08:53`).
  - `07:149` takes `entry_size`, although `07:41` fixes the entry size at CQ creation.
  - `10:92` `mtl_eq_read(eq, ev, max)` has no entry size, although `mtl_event` is size-versioned (`07:170`) and read as an array.
  - Fix: drop the `size` parameters (`struct_size` governs). Add `uint32_t entry_size` to `mtl_eq_read`. State in `07:149`: "`entry_size` must equal the CQ's configured entry size, else `-EINVAL`".
- **C-26** [verified] Functions used in the design are missing from the `10 §1` header sketch.
  - Missing: `mtl_eq_trywait` (`04:172-175` says every CQ *and* EQ has trywait; R-CMP-7),
    `mtl_instance_cancel_all_waiters` (`04:50`), `mtl_session_detach_buffers` (`05:169`), `mtl_mem_get_info`
    (`05:68`, `07:201`), `mtl_timeline_get_anchor` (`06:104`), `mtl_cq_destroy` / `mtl_eq_destroy`
    (`03:34-35`), `mtl_group_get_state` (`07:200`), `mtl_sched_get_status` (`07:198`, `08:132`; R-OBS-3 is a
    MUST), `mtl_port_count` (`03:328`), `mtl_video_layout_query` (`05:139`), `mtl_stat_get_u64` /
    `mtl_stat_list` (`08:119-120`), `mtl_session_set_option`, `mtl_version_num` and `mtl_session_api_init`
    (`11:36`, `11:39`).
  - Fix: add prototypes with `MTL_API_*` class macros.
- **C-27** [verified] The thread-safety table is incomplete, against R-THR-3 (MUST, `01:168`).
  - `04 §10` (`04:242-259`) has no rows for `mtl_tx_reap`, `mtl_rx_wait_progress`, `mtl_session_get_info` / `get_state`, `bind_cq` / `bind_eq`, `mtl_group_*`, `mtl_timeline_*`, `mtl_buffer_*`, `mtl_eq_post`, `mtl_port_*`, `mtl_instance_cancel_all_waiters`.
  - `10:75` annotates `mtl_tx_reap` as `MTL_API_DP` although it takes a timeout.
  - Fix: add the rows. At `10:75` add the same comment as `10:70`: `/* timeout > 0 → WT */`.

### 1.3 Events, results and configuration

- **C-28** [verified] Event table gaps.
  - `07:212` defines `MTL_EVENT_OVERFLOW`, but it is not in the `07 §3.2` table.
  - `04:204` disables an inline hook "with an event" that has no type.
  - `07:202-203` (`EPOCH_TICK`, `USER`) have no getter ("—"), while `07:15`, R-CMP-5 (`01:148`) and G-41 (`13:84`) say every event kind has one.
  - Fix: add the rows `OVERFLOW | EQ | lost count | — (re-read all state getters)` and `INLINE_HOOK_DISABLED | session | budget exceeded | mtl_session_get_status`. In `07:15` and `13:84` write "every *state* event kind has a state getter; notices (`EPOCH_TICK`, `USER`, `OVERFLOW`) are exempt".
- **C-29** [verified] RX unit fields used but not defined.
  - `07:93` and `05:261` rely on a "missing-range map" that `mtl_rx_unit` (`07:75-87`) does not have.
  - `07:86` copies user meta into "the buffer's meta area", which `05 §4` (`05:103-116`) never defines.
  - Fix: add `uint32_t missing_count; struct { uint32_t first_pkt, pkt_count; } missing[MTL_MAX_MISSING_RANGES];` to `mtl_rx_unit`, and `uint32_t meta_capacity;` to `mtl_buffer_desc`, or name where each lives.
- **C-30** [verified] `mtl_session_config` (`09:16-30`) lacks fields that other documents reference.
  - `unit`: `09:85` says "the session `unit` enum reserves `MTL_UNIT_PACKET_CHUNK`", but there is no `unit` field.
  - `next`: `11:64-65` and the Q-ABI-3 recommendation reserve it.
  - Timing sub-fields: `rtp_mode` (`06:238-239`), `media_index_offset` (`06:450`), `progressive_late` (`06:383`), ANC timing model (`06:219`), `rx_signal_timeout` (`07:193`), deliver-or-discard for incomplete units (`06:470`).
  - `09:26` says the completion config does "CQ/EQ binding", which duplicates `mtl_session_bind_cq/eq` (`10:56-57`).
  - Fix: after `struct_size`, add `const void* next; /* must be NULL in v1 (Q-ABI-3) */` and `uint32_t unit; /* MTL_UNIT_FRAME (default); MTL_UNIT_PACKET_CHUNK reserved */`. Extend the `timing` comment with the six names. Change `09:26` to "mode (binding is by `mtl_session_bind_cq/eq`)".
- **C-31** [verified] Zero used as a sentinel, against R-TIME-1 (`01:128`, "validity is carried by flags, never by zero").
  - `06:50` says "`accuracy_ns` 0 = unknown; valid only with `MTL_TIME_ACCURACY_VALID`", but the flag list at `06:49` does not contain `MTL_TIME_ACCURACY_VALID`.
  - `06:101` uses `grid 0/0 = derive`.
  - Fix `06:49`: add `| _ACCURACY_VALID`. Fix `06:50`: "valid only with `MTL_TIME_ACCURACY_VALID`" (drop "0 = unknown").
  - Fix `06:101`: derive the grid unless `anchor_mode` has the bit `MTL_TIMELINE_GRID_EXPLICIT`.
- **C-32** [verified] "Exactly one result" versus the completion modes.
  - R-CMP-1 (`01:144`), G-01 (`13:24`), G-05 (`13:28`) and the `07 §6` identity (`07:274-275`) require a published result for every accepted unit.
  - `MTL_COMPLETE_EXCEPTIONS` and `MTL_COMPLETE_NONE` (`07:140-141`) suppress results.
  - Fix `01:144`: "… exactly one terminal outcome: a CQ result, or a counter increment when the session's completion mode suppresses that result". Qualify G-01 and G-05 the same way. In `07:274` write "published + suppressed-by-mode".
- **C-33** [verified] Error code for "release on the wrong session". `03:73` says it fails with `-ESTALE`, but the `07:249` definition of `-ESTALE` does not cover it, and `07:248` `-EBADF` ("foreign … handle") could. Fix `07:249`: "stale buffer lease (double submit/release, after abort, or a lease of another session)".
- **C-34** [verified] `-ESHUTDOWN` scope. `07:247` says "stopped (for calls that need RUNNING)", but `03:127-128` allow TX acquire/submit in CREATED/STOPPED (preroll). Fix `07:247`: "draining, flushing, ERROR, or being destroyed; for RX `dequeue` also CREATED/STOPPED once no READY unit remains".
- **C-35** [verified] Q-MIG-3 recommendation. `11:125` and D-27 recommend closing PR #1610 now; `OPEN-QUESTIONS.md:937` recommends "(b) then (a)" (keep it open as a reference first). Fix `11:125`: "Recommendation (Q-MIG-3): keep the PR open as a reference until the new header lands, then close it as superseded, with credit".
- **C-36** [verified] Migration. `04:112-114` requires migration to quiesce (option b of Q-THR-6), but `OPEN-QUESTIONS.md:227` recommends (a), dropping migration for unified sessions. Fix `04:112`: "Runtime migration is not offered for unified sessions in v1 (Q-THR-6); if it is added later it must quiesce …".
- **C-37** [verified] Who reserves NIC resources. Q-LIFE-9 recommendation (a) (`OPEN-QUESTIONS.md:361-364`) is
  "create reserves everything; start only attaches the tasklet and joins multicast". `03:156-158` has start
  validating "flows installed, scheduler quota", and `02:222` says create does "nothing yet". Fix `03:156`:
  "Start re-checks what can have changed since create (pool complete, layouts, CQ capacity). Queues, flows and
  scheduler quota are reserved at create (Q-LIFE-9 (a))."
- **C-38** [verified] Index text. The Q-CORE-1 row at `OPEN-QUESTIONS.md:22` lists four choices; the body (`:103-115`) and `00:106` list five. Fix `:22`: "(push model, CQ/EQ, no app code on tasklets, one buffer contract, new L2 core over existing engines)".
- **C-39** [verified] Goal IDs collide with guarantee IDs. The goals are `G1…G9` (`01:66-76`), used at `03:322`, `11:7`, `12:7` and `14:7`, while the guarantees are `G-01…G-44` (13, README `:67`). Fix: rename the goals `GO-1…GO-9` in `01 §3` and at those four references, and add `GO-n` to the README conventions (`README.md:67`).
- **C-40** [inferred] Audio RTP formula. `06:198` has `RTP = floor(T0 × Fs) + first_sample + p × S`, but `06:351-352` indexes packet `p` from the stream start, so `first_sample` is counted twice. Fix `06:198`: `RTP(p) = floor(T0 × Fs) + p × S   (p = absolute packet index since T0)`.
- **C-41** [verified] The progressive example never finishes. At `10:247` the loop `rows = 128; rows <= 1080; rows += 64` ends after 1024, so rows 1024–1079 are never rendered and `MTL_PUBLISH_FINAL` is never sent. Fix `10:247-250`:
  - `for (uint32_t done = 64; done < 1080; ) { uint32_t next = done + 64 > 1080 ? 1080 : done + 64;`
  - `render_rows(&v, done, next); mtl_tx_publish(s, b, next, next == 1080 ? MTL_PUBLISH_FINAL : 0); done = next; }`
- **C-42** [verified] `11:118` gives the MXL POC size as "—"; R08 §5.1 (`research/08-consumers-ecosystem.md:361`) says "52 in 8 files". Fix: "52 in 8 files".
- **C-43** [inferred] CQ capacity does not count `SOURCE_RELEASED` entries. `07:123` says private-CQ capacity = pool size (+ progress entries), but an opted-in `MTL_CQE_TX_SOURCE_RELEASED` (`07:104-106`) adds a second entry per unit. Fix `07:123`: "… (+ progress entries, + 1 per unit when `SOURCE_RELEASED` is enabled)".
- **C-44** [verified] Wrong question reference. `13:116` cites "Q-TIME-7 area" for the user-pacing snap semantics, but Q-TIME-7 is "pacing engine per port". Fix: cite the new Q-TIME-19 (§3), or Q-TIME-0 until that question exists.

Error codes against the `07 §5` table: every code used in 00–14 (`-EAGAIN`, `-EBADF`, `-EBUSY`, `-ECANCELED`,
`-EDEADLK`, `-EINVAL`, `-ENOMEM`, `-ENOSPC`, `-ENOTSUP`, `-ERANGE`, `-ESHUTDOWN`, `-ESTALE`, `-ETIMEDOUT`) is
in the table [verified]. The inconsistencies are in the semantics (C-33, C-34), plus one condition with no
code: calling a TX verb on an RX session. Add it to the `07:250` `-EINVAL` row: "wrong-direction verb".

`MTL_START_*` (`03:149`, `06:422`, `10:126`, `10:221`), `MTL_COMPLETE_*`, `MTL_PATH_*`, `MTL_ANCHOR_*` and the CQE kinds are consistent across documents [verified].

## 2. Traceability

### 2.1 Requirement → design section → guarantee

MAY requirements (R-MEM-9, R-TIME-11) are excluded. "Partial" means the guarantee tests only part of the requirement.

| R-ID | Level | Design section | Guarantee | Status |
|---|---|---|---|---|
| R-OBJ-1 | MUST | 09 §1, 02 §3, 03 §1 | — (14 Phase 2 exit is prose only) | **GAP** |
| R-OBJ-2 | MUST | 03 §4, 10 §1 | — (G-07 covers the wrong session, not the wrong direction) | **GAP** |
| R-OBJ-3 | MUST | 05 §4 | G-12 (spans only) | partial |
| R-OBJ-4 | MUST | 07 §1, 06 §4.1 | — | **GAP** |
| R-OBJ-5 | MUST | 03 §2 | G-07, G-29 | OK |
| R-OBJ-6 | SHOULD | 04 §4.2, 07 §2.2 | — | **GAP** |
| R-MEM-1 | MUST | 05 §2, 10 §2 | G-15 | partial |
| R-MEM-2 | MUST | 05 §3 | G-10, G-11 | OK |
| R-MEM-3 | MUST | 05 §3.3 | G-10, G-11 | OK |
| R-MEM-4 | MUST | 05 §3.2 | G-16 | OK |
| R-MEM-5 | MUST | 05 §6 | G-13, G-14 | OK |
| R-MEM-6 | MUST | 05 §5 | G-35, G-36 | OK |
| R-MEM-7 | SHOULD | 05 §9, 09 §3 | G-15 | OK |
| R-MEM-8 | SHOULD | 03 §4.2, 05 §5 | 13 §6 row "app holds all RX buffers" (no ID) | partial |
| R-TIME-1 | MUST | 06 §2.1 | G-17 | OK |
| R-TIME-2 | MUST | 06 §4–5 | G-18 | OK |
| R-TIME-3 | MUST | 06 §4.3 | G-19 | OK |
| R-TIME-4 | MUST | 06 §4.3 | G-20 | OK |
| R-TIME-5 | MUST | 06 §7 | G-26, G-28 | OK |
| R-TIME-6 | MUST | 06 §2.2–2.3, 07 §3.2 | G-41 (generic) | partial |
| R-TIME-7 | SHOULD | 06 §3, §10; 03 §5 | G-23, G-24 | OK |
| R-TIME-8 | SHOULD | 06 §8 | G-21 | OK |
| R-TIME-9 | SHOULD | 06 §7.6 | — | **GAP** |
| R-TIME-10 | SHOULD | 06 §11 | G-25 | partial |
| R-CMP-1 | MUST | 03 §4.1, 07 §6 | G-01, G-02 | OK (see C-32) |
| R-CMP-2 | MUST | 07 §2 | G-04, G-05 | OK |
| R-CMP-3 | MUST | 07 §1.1 | G-28 (LATE/DROPPED only) | partial |
| R-CMP-4 | MUST | 05 §7 | G-06 | OK |
| R-CMP-5 | MUST | 07 §3 | G-41 (coalescing untested) | partial |
| R-CMP-6 | MUST | 07 §5 | G-30, G-38 (two codes only) | partial |
| R-CMP-7 | SHOULD | 04 §5.3 | — | **GAP** |
| R-OBS-1 | MUST | 08 §2 | G-40, G-42 | OK |
| R-OBS-2 | MUST | 08 §2.2 | G-43 (counts, not time) | partial |
| R-OBS-3 | MUST | 08 §3, 07 §3.2 | G-41 | OK |
| R-OBS-4 | SHOULD | 08 §2.2 | — | **GAP** |
| R-OBS-5 | SHOULD | 08 §6 | G-44 (identity untested) | partial |
| R-THR-1 | MUST | 04 §2–3, §6 | G-38 | OK |
| R-THR-2 | MUST | 04 §4–5 | G-39 | OK |
| R-THR-3 | MUST | 04 §3, §10; 10 §1 | — | **GAP** |
| R-THR-4 | MUST | 05 §6.3 | G-14, G-39 (indirect) | partial |
| R-THR-5 | SHOULD | 04 §5.1–5.2 | G-39, 13 §8 W3 gate | OK |
| R-LIFE-1 | MUST | 03 §3 | G-35 (start only) | **GAP** |
| R-LIFE-2 | MUST | 03 §3.4 | G-31 | OK |
| R-LIFE-3 | MUST | 04 §5.4, 03 §3.4 | G-30 | OK |
| R-LIFE-4 | MUST | 03 §6.1 | G-37, G-29 | OK |
| R-LIFE-5 | SHOULD | 03 §7 (open: Q-LIFE-2) | — | **GAP** (deferred) |
| R-CAP-1 | MUST | 09 §6, 06 §6 | G-13, G-14, G-34 | OK |
| R-ABI-1 | MUST | 11 §2.1 | G-33 (struct_size only) | partial |
| R-ABI-2 | MUST | 11 §2 | — | **GAP** |
| R-ABI-3 | SHOULD | 11 §2 | — | **GAP** |

Every requirement has a design section [verified]. The six MUST gaps break the quality bar in `01 §6` ("every MUST in scope has a contract test").

### 2.2 Proposed guarantees to close the gaps (append to 13)

- G-45 (R-OBJ-1, §1) — "The same verb sequence passes G-01…G-09 for every essence × direction." Tier U/UB. How: parameterise the G-01 suite over the five create functions.
- G-46 (R-OBJ-2, §1) — "A TX verb on an RX session, or an RX verb on a TX session, returns `-EINVAL` and changes nothing." Tier U.
- G-47 (R-OBJ-4, §1) — "Every result and delivery returns the submission's (TX) or buffer's (RX) `user_cookie` verbatim." Tier U. How: random cookies, compared one to one.
- G-48 (R-OBJ-3, §2) — "A buffer's planes never change between uses; per-use fields exist only in the submission/result." Tier U. How: hash `mtl_buffer_get_view` before and after 10^4 uses.
- G-49 (R-LIFE-1, §4) — "Every call returns the code in the `03 §3.2` table for every state." Tier U. How: a table-driven state × call matrix.
- G-50 (R-THR-3, §5) — "Every exported function carries exactly one `MTL_API_{CP,DP,WT,AS}` annotation." Tier build. How: a header lint in CI.
- G-51 (R-ABI-1/2, §4) — "Unknown flag bits return `-EINVAL`; the library has a SONAME and exports only `mtl_*` symbols in the version script." Tier U + build. How: `readelf -d`, `nm -D`.
- G-52 (R-CMP-7, R-OBJ-6, §5) — "`trywait` + wait fd never loses a wake-up, for private and shared CQs and for EQs." Tier U. How: race stress of the `04 §5.1` protocol.
- G-53 (R-TIME-9, §3) — "Submitting before `hint.submit_deadline` yields `ON_TIME` in the hinted slot." Tier UB.
- G-54 (R-TIME-6, §3) — "`mtl_time_get_status` clears LOCKED on sync loss and posts `TIME_STATE`." Tier UB. This pins SF-20.
- G-55 (R-OBS-2/4, §5) — "`queued_media_ns` equals the sum of the queued units' durations; each histogram's `count` equals the number of published results." Tier U.
- G-56 (R-MEM-8, §2) — "RX leases released out of order, from other threads, return to FREE; the overflow policy acts only on READY slots." Tier U.
- G-57 (R-CMP-3, R-CMP-6, §1/§4) — "Each status and each `07 §5` error code is produced by its documented trigger." Tier U/UB. How: a table-driven trigger list.

### 2.3 Guarantees in no phase exit

`14-implementation-roadmap.md:52,61,71,80,89` never list **G-34, G-36, G-37, G-39** [verified]. G-37 appears only in the fault matrix (`13:91`).

- Fix `14:52`: "… G-29…G-33, G-35, G-38, **G-39**, G-40, G-43 …".
- Fix `14:61`: add "**G-34** for the capabilities introduced".
- Fix `14:80`: "G-10…G-16, **G-36**, **G-37** green".

### 2.4 DECISIONS.md

- Every "Where" target of D-01…D-27 is an existing section [verified]: 02 §2, §6; 03 §2, §3, §6; 04 §2, §3, §5; 05 §1–3, §6, §7; 06 §2.1, §3, §3.2, §4–5, §4.3, §7, §7.5–7.6, §10; 07, 07 §5; 08 §2; 09 §1.1, §2.1, §6; 11 §2, §2.1, §4, §6; 14.
- Every "Depends on" Q-ID exists [verified].
- Seven B questions have no Proposed decision, although each has a recommendation: Q-ARCH-2, Q-THR-8, Q-LIFE-6, Q-TIME-1, Q-MODE-3, Q-ABI-3, Q-ABI-5. Suggested new rows:
  - D-28 "v1 reuses `mtl_handle`" (Q-ARCH-2, 03 §7)
  - D-29 "single submitter / single consumer per session; MP-safe `rx_release`" (Q-THR-8, 04 §4)
  - D-30 "async-signal-safe `cancel_waiters`" (Q-LIFE-6, 04 §5.4)
  - D-31 "time sources and estimated-clock labelling" (Q-TIME-1, 06 §2.2)
  - D-32 "separate `mtl_st22_session_create`" (Q-MODE-3, 09 §1)
  - D-33 "reserve `next` in create configs" (Q-ABI-3, 11 §2.2)
  - D-34 "names" (Q-ABI-5, 11 §7)
- Other positions the design takes without a decision row: completion modes (07 §2.1), destroy with held leases (03 §6.3), RX overflow policy (03 §4.2), default late policy per media mode (06 §7.2). Add rows, or link them to the new questions in §3.

### 2.5 Q-IDs and anchors

- 74 `### Q-…` headings, 74 index rows, 19 marked B, 38 P, 17 L. This matches `00:101-102` [verified].
- Every Q-ID mentioned in any file exists [verified]. Every `(#q-…)` anchor in the index resolves to a heading [verified]. No other file links to an anchor; Q-IDs elsewhere are plain text.
- Suggestion: link them, for example `[Q-TIME-7](../OPEN-QUESTIONS.md#q-time-7)`.

### 2.6 Other traceability defects

- **T-01** [verified] `doc/user-pacing-timestamp-contract.md` ("the contract") is not in `545a266a`: `git show HEAD:…` fails and `git status` shows it untracked. It is still cited as the basis of the timing design and test oracle (`06:8`, `06:27`, `13:107`, research/05 §8). Fix: commit it with this document set, or state its status in `06:8`: "untracked working draft, not in `545a266a`".
- **T-02** [verified] README `:71` says citations are "`path:line` at `545a266a`", and `side-findings.md:7` says the agents read `git archive HEAD`. Several citations match the working tree instead (§5.2).

## 3. Research coverage

Every "Open questions for the maintainer" list in R01–R13 was mapped to OPEN-QUESTIONS:
15+14+12+13+14+13+16+13+12+14+12+14+14 = 176, consistent with "~170" (`OPEN-QUESTIONS.md:7`). Most are
covered, merged, or settled by an explicit design choice (for example R01 Q4, Q6 and Q15; R06 Q5 and Q8; R09
Q4 and Q5; R12 Q3). The dropped questions below are ones where the design takes a position silently, or where
there is a real maintainer trade-off. Each has proposed wording.

- **Q-CMP-6 (P, new)** — from R11 Q1, R06 Q3, R05 Q9.
  - Question: "What makes a TX unit `ON_TIME`?"
  - Why: 07 §1.1 and 06 §7.5 use different reference points (C-05), and there is no tolerance.
  - Options: (a) admission verdict: picked up by the pick-up deadline; (b) wire verdict: first packet within a tolerance of `scheduled_first` (needs Q-TIME-10); (c) status from (a), wire error reported separately.
  - Recommendation: (c), with a per-session tolerance reported in `get_info`.
- **Q-TIME-18 (B, new)** — from R05 Q5, R10 Q4, R11 Q2.
  - Question: "Default late policy per media mode."
  - Why: 06 §7.2 decides DROP for INDEX/TAI and RESLOT for AUTO, and D-13 depends only on Q-TIME-4.
  - Options: (a) as proposed; (b) DROP everywhere; (c) SEND_LATE for live.
  - Recommendation: (a).
- **Q-TIME-19 (P, new)** — from R05 Q6; fixes C-44.
  - Question: "How does legacy USER_PACING (nearest epoch, possibly before the request) map onto the new model, and does the shim keep 'nearest' or switch to NOT_BEFORE?"
  - Why: 13 §7 records a difference from the contract with no question behind it.
- **Q-TIME-20 (P, new)** — from R05 Q12.
  - Question: "Is CTM/LLTM-conformant ANC packet scheduling (06 §5.1, E7) in scope for v1, and which model is the default?"
  - Why: it is a wire change for multi-packet ANC; today packets are spread over the frame.
- **Q-LIFE-10 (P, new)** — from R06 Q6, R13 Q8.
  - Question: "On VF reset or port restart, do sessions survive (pause and resume with `PORT_RESET`), go to ERROR, or need re-creation?"
  - Why: 14 Phase 5 schedules "VF reset handling" with no defined outcome; Q-LIFE-7 covers only link loss.
- **Q-THR-9 (P, new)** — from R03 Q6.
  - Question: "Where do TX-hang recovery and RX auto-detect re-init run (admin thread, per-instance worker, or split), and what added recovery latency is acceptable?"
  - Why: 04 §2 assigns them to WK without the latency trade-off.
- **Q-THR-10 (P, new)** — from R03 Q10.
  - Question: "What happens to `MTL_FLAG_TASKLET_THREAD` and `MTL_FLAG_TASKLET_SLEEP` for unified sessions?"
  - Why: the sleep path uses condvars, alarms and `nanosleep` on scheduler threads, which interacts with the waker design (04 §5.2) and G-39.
- **Q-MEM-11 (P, new)** — from R04 Q5.
  - Question: "For imported non-hugepage memory, keep MTL's invented-IOVA allocator (`mt_dma.c:21`) or require IOVA == VA?"
  - Why: 05 §3.2 assumes IOVA = VA, and the choice decides collision handling with DPDK's own mappings.
- **Q-MODE-8 (L, new)** — from R07 Q10.
  - Question: "RTCP retransmission scope: video only (remove the no-op flags elsewhere), generalise, or later?"
  - Why: 09 §3 and §8 decide it with no question.
- **Extend Q-MODE-4** — from R02 Q11: add "and whether the ~105 conversion functions (`st_convert_api.h`, `st_convert_internal.h`) move out of the core library or headers".
- **Extend Q-ABI-3** — from R09 Q12: add option "(c) named, versioned backend-extension tables (`mtl_open_ext(obj, name, version, &ops)`) for DATA_PATH_ONLY, AF_XDP knobs and queue meta".

Research numbers used in `OPEN-QUESTIONS.md:7` and `README.md:13` ("13 research notes") are consistent: R00 is input, not a research note [verified].

## 4. Mode coverage

### 4.1 research/07 §7 "easily-forgotten modes"

| # | Mode | Status in the design | Where |
|---|---|---|---|
| 1 | Slice-level TX/RX | handled (progressive) | 06 §9, 09 §2, §8 |
| 2 | RTP-level app-built packets | deferred (NG2, reserved unit) | 01 §4, 09 §2.1, D-25 |
| 3 | `uframe_pg_callback` | dropped | 09 §8 |
| 4 | st20p `PKT_CONVERT` | dropped | 05 §6.3, 09 §4, §8 |
| 5 | Derive pipelines (incl. st22p codestream) | handled | 09 §2, §4 |
| 6 | Ext frame, both TX flavours incl. `EXT_FRAME_MANUAL_RELEASE` two-phase release | **partly silent**: attached pools cover `set_ext_frame`; the two-phase release is never mapped | 05 §2, §8 |
| 7 | Split-forward (TX ext frame at an offset in another session's RX frame, with `linesize`) | **silently missing** | — |
| 8 | RX dedicated vs dynamic ext frames | handled (dynamic → later `provide`) | 05 §8, Q-MEM-10 |
| 9 | GPU VRAM RX frames | deferred | 05 §10, Q-MEM-8 |
| 10 | DATA_PATH_ONLY | deferred (verify first) | 09 §5, §7, §8; Q-MODE-5 |
| 11 | Exact vs user vs normal pacing | handled | 06 §5.2, §12 |
| 12 | `RTP_TIMESTAMP_EPOCH`, `rtp_timestamp_delta_us` | handled | 06 §5.3, §12; Q-TIME-5 |
| 13 | DISABLE_BULK / TSC_NARROW / STATIC_PAD_P / start_vrx / pad_interval | handled for 4 knobs ("advanced sub-struct"); **TSC_NARROW silently missing**, and it is a pacing way, not a knob | 09 §8 |
| 14 | 2-thread RX (> 40 Gbps) | **silently missing** (04 §9 mentions only the `USE_MULTI_THREADS` race) | — |
| 15 | Header split | dropped for v1 | NG6, Q-MEM-9 |
| 16 | Field-as-frame interlace, library-managed parity | handled (index parity) | 06 §4.2 |
| 17 | ST40 split-by-packet; interlace auto-detect | auto-detect handled (09 §3); **split-by-packet silently missing** | — |
| 18 | ST30 `BUILD_PACING`, `fifo_size`, RL warm-up | **silently missing** (09 §8 lists video knobs only) | — |
| 19 | ST22P codec threads, encoder `resp_flag` BLOCK_GET | handled (plugin ABI frozen) | 09 §4, Q-MODE-4 |
| 20 | Notify-driven vs polled completion | handled | 07 |
| 21 | Non-RFC 4175 transport formats | handled (Q-MODE-7) | OPEN-QUESTIONS |
| 22 | Debug knobs in public ops | handled (separate debug API) | 09 §8 |

Totals: 13 handled, 3 deferred (2, 9, 10), 3 dropped (3, 4, 15). #6 and #13 are handled only in part, and their missing parts are counted in 4.3. Three are silently missing in full: #7, #14, #18. #17 counts as handled for auto-detect, and its split-by-packet part is silently missing.

### 4.2 research/07 §3 matrix features with no disposition

- `DEDICATE_QUEUE` vs shared queue per ST30/40/41 session (R07 §3.1).
- Per-session `FORCE_NUMA` + `socket_id` (R07 §3.1, §3.2, §6). 04 §9 only *reports* the socket.
- `MTL_FLAG_RANDOM_SRC_PORT` / `MULTI_SRC_PORT` (R07 §5.2).
- ST41 RX DIT/K filter (R07 §3.2 "+DIT/K"); `mtl_flow` has no field for it.
- `rx_burst_size` (R07 §3.2).
- "Deliver incomplete frames" flag. 06:470 says "(or discarded, per config)", but no config field exists (C-30).
- Pacing ways `BE` and `PTP` (R07 §5.1, R05 §1), which have no pacing class in 06 §6 (`HW_RATE`, `HW_LAUNCH`, `SW`, `ANY`).
- `MTL_FLAG_TASKLET_SLEEP` (R03 Q10; see Q-THR-10).

### 4.3 Fix: add rows to `09 §8` (09:192-209)

- "`TSC_NARROW`, `BE`, `PTP` pacing ways | map to `MTL_PACING_SW` with an accuracy profile, or drop `BE`/`PTP` | today's `enum st21_tx_pacing_way` has 7 values; 06 §6 has 3 classes"
- "2-thread RX (> 40 Gbps) | later; reported in `get_info` as extra scheduler consumers | implicit today"
- "ST40 split-by-packet | ST40 config option, v1 | wire behaviour"
- "ST30 `BUILD_PACING`, `fifo_size`, RL warm-up | advanced sub-struct (as for video) | `fifo_size` is ignored today (#948)"
- "Split-forward (sub-rectangle TX from an RX buffer) | v1 via imported RX pool + TX buffer with `stride` ≠ linesize → path CONVERT/COPY, or later DIRECT if the builder supports linesize | 05 §4.1 requires stride == linesize for direct TX"
- "st20p `EXT_FRAME_MANUAL_RELEASE` | subsumed by leases (release = terminal result) | two-phase release today"
- "DEDICATE_QUEUE, per-session NUMA, src-port flags, ST41 DIT/K filter, `rx_burst_size` | `mtl_session_config` / `mtl_flow` fields or port-level only (state which) | today's per-session flags"

## 5. Claims vs research and code

### 5.1 Spot checks

| # | Claim (design location) | Checked against | Result |
|---|---|---|---|
| 1 | 475 public functions, ~207 flag bits, 24 namespaces, 18 families (`00:76`, `01:20-22`) | R02 TL;DR `:14-15`, `:55` | [verified] match |
| 2 | 4 frame-exchange models, 12 metadata structs (`00:76`, `01:20`) | R02 `:15` | [verified] match |
| 3 | 757 exported symbols, 235 internal (`11:15`) | R13 §4 `:139` | [verified] match |
| 4 | "368 of ~900 `return -E*`" (`07:260`) | R13 §3 `:118` (sum 892) | [verified] match |
| 5 | PR 283 commits behind, 16 commits, base `74c9af0e`, 7 textual conflicts (`00:7`, `00:19`, `11:138`) | R01 `:6-9`, `:395` | [verified] match |
| 6 | 74 questions, 19 B (`00:101-102`) | OPEN-QUESTIONS count | [verified] match |
| 7 | ~57 ticks at 1080p59.94 (`06:32`, `13:53`, `OPEN-QUESTIONS.md:714`) | R12 `:173` (57.39 ticks), `:228`; R05 `:18` "+56/57 [inferred, simulated]" | [verified] match; label as simulated in `13:53` |
| 8 | +66 ns epoch error (`06:34`) | R12 `:52`, `:233` | match, but R12 labels it **[inferred, computed]**; add the label at `06:34` |
| 9 | ≤ 512 packets ≈ 2 ms at 1080p60 (`06:265`) | R05 `:167`, `:291` (≈1.9 ms) | [verified] match |
| 10 | ≈27 ms phase lock (`06:346`) | R08 `:318` | [verified] match |
| 11 | 64-entry drop-on-full ring (`07:20`) | R01 `:170` | [verified] match |
| 12 | C11 atomics in 13 files (`11:151`) | R13 `:157` | [verified] match |
| 13 | ~30 `_MAX` sentinels (`11:19`) | R13 `:143` | [verified] match |
| 14 | Consumer call-site counts (`11:111-117`) | R08 §5.1 `:355-362` | [verified] match; the MXL row is missing (C-42) |
| 15 | `st20_pipeline_tx.c:29-45`, `:774-789` (BLOCK_GET mutex + condvar) (`02:50`, `04:22`) | `git show HEAD:` | [verified] match |
| 16 | `st20_pipeline_tx.c:781-788` (wake does not return early) (`04:185`) | HEAD | [verified] match |
| 17 | `mt_handle_guard.h:73-95` reads `impl->type` through the raw pointer (`03:70`) | HEAD `:75-95` (`if (*type != want)` at `:81`) | [verified] match |
| 18 | `st_tx_video_session.c:2873-2895` chain heuristic (`05:151`) | HEAD `:2876-2895` | [verified] match; the fallback logs `warn()` (`:2889`) and `info()` (`:3423`), so "silently" means "log only" |
| 19 | `st_tx_video_session.c:1796-1805` EXACT falls back (`06:232`, `13:59`) | HEAD (returns 0) | [verified] match |
| 20 | `mt_ptp.c:540-552` sticky `locked` (`06:61`) | HEAD (only sets `true`) | [verified] match |
| 21 | `mt_ptp.c:1062-1067` UTC→PHC switch (`06:69`) | HEAD `:1061-1066` | [verified] match |
| 22 | `mt_dma.c:32-41` overlap check misses an enclosing range; `:21` "1M" vs 64 KiB (`05:307`) | HEAD | [verified] match |
| 23 | `st_tx_ancillary_session.c:1108` ANC spread over the frame (`06:213`) | HEAD | [verified] match |
| 24 | `st_video_transmitter.c:444-461` beyond-horizon send (`06:307`) | HEAD (err log, then burst at `:464`) | [verified] match |
| 25 | `st30_pipeline_tx.c:671` reads the RX FORCE_NUMA flag (SF-01) | HEAD | [verified] match |
| 26 | `st_pipeline_api.h:71` plugin magic shift; `:2309` `<= _END` (SF-26, SF-27) | HEAD (`_END` is a sentinel at `:194`) | [verified] match |
| 27 | `st_tx_fastmetadata_session.c:769-772` wrong union member (SF-03) | HEAD `:770` | [verified] match |
| 28 | "packets after the first carry the previous packet's slot time under USER_TIMESTAMP without USER_PACING (`st_tx_audio_session.c:803-819`)" (`06:363-365`, SP-09, R05 F9) | HEAD `:803-808` continues from `ptp_time_cursor`; added by `442847c0` (#1713), an ancestor of HEAD | **[verified] stale**; see fix below |
| 29 | `mt_main.c:869-870` "only map for MTL_PORT_P" (`05:83`, `05:306`, SF-07) | HEAD `:864-865`; the working tree has it at `:869` | **[verified] working-tree line** |
| 30 | `st_tx_video_session.c:3734,3838-3845` uinit/detach deadlock (`03:314`, SP-01) | HEAD `tv_mgr_detach` `:3798-3806`, `tv_mgr_uinit` `:3898-3914`; the working tree matches `:3729-3737`, `:3836-3847` | **[verified] working-tree lines** |
| 31 | `st_tx_video_session.c:2449-2455` ST22 invalid size → done (`07:63`, SF-11) | HEAD `:2468-2474`; the working tree has it at `:2449-2455` | **[verified] working-tree lines** |
| 32 | `st_rx_video_session.c:4284-4288` `query_ext_frame` forces INCOMPLETE (`05:252`) | HEAD `:4287-4291` | **[verified] working-tree lines** |
| 33 | `mt_main.c:416` `mtl_init` mutates params (SF-30) | HEAD `:404`; working tree `:416` | **[verified] working-tree line** |
| 34 | `mt_main.c:707-763` NULL derefs (SF-31) | HEAD `mtl_get_lcore` at `:700`; working tree `:708` | **[verified] working-tree lines** |
| 35 | `doc/design.md:384`, `:604` (DD-05, DD-08) | HEAD `:390`, `:618` (`doc/design.md` is edited in the working tree) | **[verified] working-tree lines** |
| 36 | KB "secondary-process stats" at `:768-769` (R07 §8 #7) | HEAD `.github/copilot-docs/mtl-knowledge-base.md:794`; working tree `:769` | **[verified] working-tree line** |

Fix for row 28: at `06:363-365`, shorten the bullet to "Per-packet RTP is computed from the sample index."
(drop the "today packets after the first …" clause). Delete SP-09 (`side-findings.md:66`). Mark R05 F9
(`research/05-timing-pacing.md:371`) "fixed by #1713 (`442847c0`) before the baseline". Rows 29–36: re-pin
each citation to the HEAD line given in the result column.

### 5.2 Automated sweep of line citations

A script took every `name.[ch]:N[-M]` citation in 00–14 and side-findings that points into one of the 20
`lib/` files edited in the working tree. It compared the cited lines at HEAD with the same lines in the
working tree. Result: **45 of 72 such citations cite text that differs between the two**. Rows 29–34 above
show that some of these match the working tree and not the baseline. Others match HEAD (for example
`st_tx_video_session.c:2682`, `:3848-3855`, `:2694-2698`, `mt_sch.c:873-923`, `mt_main.c:1100-1127`).

The affected lines:

- 03: 314
- 04: 20, 23, 25, 226, 237, 238
- 05: 83, 151, 252, 270, 283, 306, 309
- 06: 31, 365, 465, 508
- 07: 63
- side-findings: 21, 25–32, 37, 39, 40, 47–50, 58–62, 66, 85

Fix: re-pin every `path:line` against `git show 545a266a:path` before publishing, and add a one-line check to the README conventions, for example "`git grep -n` at `545a266a` must find the cited text". The side-findings header at `side-findings.md:7` must drop "research agents read `git archive HEAD`" until that is true.

## 6. Markdown hygiene

The checks were scripted against `.github/linters/.markdown-lint.yml` because no markdownlint binary is installed (`command -v markdownlint markdownlint-cli2 mdl` found none).

- **Relative links:** 0 broken (all `](path)` targets in 00–14, OPEN-QUESTIONS, DECISIONS, side-findings, README and research/ exist) [verified].
- **Anchors:** 74 of 74 `#q-…` targets resolve [verified]. There are no other in-page anchors.
- **Headings with trailing `.,;:!` (MD026):** 0 [verified].
- **Lines > 400 characters (MD013, which checks tables too):** 5 [verified].
  - `04-threading-and-execution.md:20` (411) — split the H1 cell: move the citations `(st_tx_video_session.c:2682, st_rx_video_session.c:3516)` into a footnote line under the table.
  - `04-threading-and-execution.md:48` (434) — shorten the DP contract cell to "APP threads (and inline hooks, §6). Lock-free, O(1), no syscall, no allocation, no log above DEBUG, bounded. Returns `-EAGAIN` instead of waiting (§4)."
  - `side-findings.md:23` (403) — drop "(esp. `:134-139`)" from the evidence cell.
  - `side-findings.md:64` (452) — split SP-07 into SP-07a (st40p TX leak / double free), SP-07b (st40p RX `meta_num`), SP-07c (st20p RX `PKT_CONVERT` + `EXT_FRAME` NULL write, `get_fb_addr`), SP-07d (st22p leak, `frame_done` ordering).
  - `side-findings.md:88` (520) — split DD-13 into one row per header (`mtl_api.h`, `st30_api.h`, stats getters, `DATA_PATH_ONLY` docs, `mt_sch`, `st20_combined_api.h`, `st20_api.h`).
- **Table column counts:** 2 rows have an extra cell, both from an unescaped `|` inside a code span [verified].
  - `side-findings.md:48` — `port_params[i].flags |= …` → `port_params[i].flags \|= …`.
  - `research/13-lifecycle-errors-abi.md:135` — `((a)<<16|(b)<<8|(c))` → `((a)<<16\|(b)<<8\|(c))`. research/ is input; fix it only if the notes are published.
- **List indentation (MD007 indent 2):**
  - Nested bullets with 3 spaces under `1.` items (`00-summary.md:41-69`, 14 lines; `research/03`, 25; `research/04`, 39; `research/09`, 18) are *correct*. CommonMark requires the content column (3), and MD007 only applies when every parent is unordered. No change is needed.
  - In `research/01-pr1610-analysis.md:349-477` (46 lines), `research/06-observability.md:155-187` (25) and `research/08-consumers-ecosystem.md:24-437` (38), sub-bullets under `1.` items are indented only 2 spaces. They do not nest: each starts a new top-level list indented 2, which MD007 flags, and it restarts the ordered list [verified].
  - Fix: indent those sub-bullets by 3 spaces (4 under `10.`–`16.`). They are in research notes, so fix only if the notes are published.
- **This file:** longest line < 400, nested lists use 2-space indent, and every `|` inside a table cell is escaped or absent.
