# Response to C5 — adversarial user and feasibility review

| | |
|---|---|
| Status | Designer's response to [C5](C5-adversarial-user-review.md); drives **revision 3** of every document in this directory |
| Date | 2026-09-30 |
| Rule | Every C5 finding gets a verdict: **Accepted** (changed as the remedy proposes), **Accepted, modified** (changed, differently from the remedy, with the reason), **Rebutted** (not changed, with evidence), or **Deferred** (a real gap, scheduled to a named phase) |

C5's verdict is accepted: revision 2 was not ready to freeze a Phase 1 header. The model
(media time vs launch, leases, CQ/EQ, nothing on tasklets) is kept. The header, the RX
half, the framework recipes, the plan's premises and its sequencing are reworked here.

- **Part A** lists the decisions that cut across documents; each document applies them with
  the exact names given here.
- **Part B** answers every C5 finding in order: 207 entries, of which 175 accepted as
  proposed, 31 accepted with a different remedy (each with its reason), one deferred to
  Phase 4 and one partly rebutted.
- **Part C** says where the answers went: the header, the design documents, the questions
  and the decision log.

## Where this response disagrees with C5

C5 was right far more often than wrong. These are the places where the evidence or the
standards led somewhere else, so the maintainer can check them first:

| C5 § | C5 said | Revision 3 | Evidence |
|---|---|---|---|
| 5.3 | the exact second-field RTP alternates +750/+751, so G-19 and G-22 cannot both pass | true only for a 119.88-field raster; with T0 on the frame grid the offset is a constant +1501 at 1080i59.94, +1800 at 1080i50, +1500 at 1080i60. G-22 is still rewritten to state the rule, not an offset | exact rationals ([06 §4.5](../06-timing-pacing-and-sync.md)) |
| 5.7 | RX `media_index = floor((media − T0)/period)` | off by one whenever the sender truncated a fractional tick (half the frames at 59.94p); the exact inverse of `RTP = floor(M·R)` is used | 0 mismatches vs 2525 of 5050 ([06 §11.2](../06-timing-pacing-and-sync.md)) |
| 5.8 | make `REANCHOR` the CAPTURE default | NEAREST snapping for every source kind: drop/gap adaptation keeps RTP on the grid and never re-phases; REANCHOR would jump the RTP offset at every re-anchor | [06 §4.3](../06-timing-pacing-and-sync.md) |
| 5.12 | CBR with a deferred drop is hostile; keep today's variable size as a mode | ST 2110-22 *requires* constant bytes and packets per frame, so CBR stays the default; the hostility is fixed by a per-unit ceiling, padding and a synchronous `-MTL_ENOSPC` | R12 §6 ([06 §5.2](../06-timing-pacing-and-sync.md)) |
| 5.15 | a legacy ST40 session next to a unified ST20 session disagrees by ≈ 55–58 ticks | the other way round: legacy ANC already stamps its epoch and agrees within ±1 tick; the outlier is **legacy ST20** on its default TX-cursor RTP, +54.4…+55.7 ticks (604–619 µs by VRX0, not one value of 608 µs) | `st_tx_ancillary_session.c:420-427` ([06 §13](../06-timing-pacing-and-sync.md)) |
| 4.10 | auto-select W2 below 1 ms | kept as the designer's recommendation but **not** settled: W2 is a syscall on a pinned core, which the maintainer's rule forbids, so it is decision M6 | [04 §5.2](../04-threading-and-execution.md) |
| 6.2 | a hold count via `mtl_rx_hold` or a transfer to an array of TX targets | the hold travels in the TX submission (`.hold`); a buffer belongs to one pool, and TX sessions create their own buffers over the RX region | [05 §5.4](../05-memory-and-buffers.md) |
| 3.6 | auto-promote a cookie in completion NONE, or warn | fail fast: `-MTL_EINVAL`, reason `COOKIE_WITHOUT_RESULTS`, because a promotion would re-create the silent stall C3 found | [07 §2.2](../07-completions-events-and-errors.md) |
| 1.1 | 50–75 EM | accepted as order of magnitude, but revision 3 *adds* scope (test substrate, operator surface, legacy forks): ≈ 55–85 EM, with a first user result in Phase 0.5 | [14 §4](../14-implementation-roadmap.md) |
| 1.8, 6.14, 8.2 | several `path:line` citations | a number of C5's line numbers do not match `545a266a` (for example the ALLOW_DOWN_PORTS prune is at `st_tx_video_session.c:4000-4045`, not `:3935-3975`; `mt_eth_link_dump` is `mt_util.c:409-422`); the findings stand, the citations are corrected in the documents | `git show 545a266a:<path>` |

## Part A — Cross-cutting resolutions (normative for revision 3)

### A1. The header is a real file and it compiles

- The normative sketch is now a real header set:
  - `sketch/include/mtl/experimental/mtl_unified.h`;
  - `sketch/include/mtl/experimental/mtl_simple.h` (L4);
  - `sketch/include/mtl/experimental/mtl_debug.h` (debug and test API).
- The examples live in `sketch/examples/`, as C99 files and one C++17 twin.
- `sketch/check.sh` compiles everything with:
  - `gcc -std=c99 -Wall -Wextra -Wpadded -Werror`;
  - `g++ -std=c++17 -Wall -Wextra -Werror`.
  This is the "doc test". [10](../10-api-sketch.md) now describes the header; if prose and
  header disagree, the header wins.
- Every constant, enum, struct and function named in any document is declared in the
  header.
- Every public struct is checked with a C99 size-check typedef:
  `typedef char mtl_sz_<name>[(sizeof(struct <name>) == N) ? 1 : -1];`.
- There is no implicit padding. Every hole is a named must-be-zero `reserved` field, and
  `-Wpadded -Werror` enforces it.

### A2. CQ records

- `MTL_CQ_ENTRY_SIZE` stays **256** and the size-check enforces it.
- Every CQ record kind is at most **240 B**, which leaves ≥ 16 B for growth inside the
  entry.
- Times inside CQ records are **`int64_t` TAI nanoseconds** plus one `uint32_t time_valid`
  bitmask per record, one bit per time field.
  - The self-describing `struct mtl_time { ns, clock, flags, accuracy }` stays for API
    arguments and getters.
  - "Every time in a CQ record is TAI or invalid" is a header rule.
  - This one change removes 8 B per time field and makes `mtl_tx_result` and
    `mtl_rx_unit` fit, per-leg times included.
- `mtl_time_diff_ns(a, b, valid_mask, &out)` helps tests and apps.
- Detail that still does not fit goes into an optional follow-on kind
  (`MTL_CQE_RX_MISSING` carries the missing-range table), enabled per session.

### A3. Array reads take the stride; `struct_size` is input only

- Array readers take the caller's record size explicitly:
  - `int mtl_tx_reap(mtl_session_h, void* res, size_t res_size, uint32_t max, int64_t timeout_ns)`;
  - `int mtl_cq_read(mtl_cq_h, void* buf, size_t record_size, uint32_t max, int64_t timeout_ns)`;
  - `int mtl_eq_read(mtl_eq_h, struct mtl_event* ev, size_t ev_size, uint32_t max, int64_t timeout_ns)`.
- The library writes `min(record_size, native)` bytes per record at pitch `record_size` and
  never modifies a caller-set stride.
- In each written record, `hdr.size` is an **output**: the number of bytes written.
- `struct_size` on single structs is **input only**. The library never rewrites it.
- Array reads return a count ≥ 0. `0` means empty, and only when `timeout_ns == 0`; with a
  timeout > 0 and nothing ready they return `-MTL_ETIMEDOUT`.
- Single-object verbs (`acquire`, `dequeue`) return `0` or a negative error code.

### A4. Handles

- The handle types are:
  - `mtl_instance_h`, which replaces the raw `mtl_handle` in every new prototype
    (bridge: `mtl_instance_from_legacy`);
  - `mtl_session_h`, `mtl_buffer_h`, `mtl_region_h`, `mtl_cq_h`, `mtl_eq_h`,
    `mtl_timeline_h`, `mtl_group_h`;
  - **`mtl_lease_h`** (new): access-moving verbs take and return only leases;
    `mtl_lease_buffer(lease)` gives the base buffer.
- **ID 0 is the null handle of every type.** Every call rejects it with `-MTL_EBADF`.
  - Each type gets `MTL_<TYPE>_NULL`, `mtl_<type>_is_null()` and `mtl_<type>_eq()`
    (static inline plus exported twins for bindings).
  - In a config struct, a null handle means "the documented default". A null handle is
    never a live object.
  - The epoch timeline has a real handle, `mtl_timeline_epoch(mt)`.
- Lease encoding is `session index:16 | slot:16 | generation:32`.
  - Generations are seeded randomly per session and skip 0.
  - A foreign lease therefore fails deterministically on the index, not with a 1/256 tag
    check.
- Object tables are grow-only chunked arrays; nothing is sized at 256 per instance.
- `mtl_stat_get_u64` becomes typed getters: `mtl_session_stat_get`, `mtl_port_stat_get`,
  `mtl_instance_stat_get`.

### A5. Error vocabulary and return conventions

- Error codes are `MTL_E*` constants with fixed values: equal to Linux errno where Linux
  has the code, private values (≥ 1000) where Windows UCRT lacks it. The header is one ABI
  on every OS.

| Code | Meaning, and only this |
|---|---|
| `-MTL_EAGAIN` | nothing now, `timeout == 0`; includes RX `dequeue` in CREATED/ARMED/STOPPED with nothing READY |
| `-MTL_ETIMEDOUT` | the timeout expired; same handling as EAGAIN |
| `-MTL_ECANCELED` | waiters interrupted (`*_interrupt`), sticky until `*_uninterrupt` |
| `-MTL_ESHUTDOWN` | **app-initiated** only: the session is FLUSHING, DRAINING or DESTROYING because the app asked |
| `-MTL_EIO` | the session is in **ERROR**; the reason is in `mtl_session_get_status().reason` |
| `-MTL_ENODEV` | the device behind the session or port is gone or reset and resources cannot be re-reserved |
| `-MTL_EBADF` | null, foreign or destroyed handle |
| `-MTL_ESTALE` | a lease of the right session whose generation moved on (already returned or submitted) |
| `-MTL_EINVAL`, `-MTL_ERANGE`, `-MTL_ENOSPC`, `-MTL_EBUSY`, `-MTL_ENOTSUP`, `-MTL_EEXIST`, `-MTL_EDEADLK`, `-MTL_ENOMEM` | as in 07 §5 |

- `*_trywait()` returns **1** = something is ready (do not block), **0** = armed and safe to
  block on the wait object, **< 0** = error. It never returns `-EAGAIN`.
- `enum mtl_state_reason` enumerates every reason carried by `SESSION_STATE`, by
  `mtl_session_get_status()` and by `mtl_error_info`.
- Status enums start with `*_UNSET = 0`, so a half-filled record never reads as success:
  - `MTL_TX_STATUS_UNSET = 0`, `MTL_TX_ON_TIME = 1`, …;
  - likewise for RX.
- `mtl_last_error_detail()` is replaced by `int mtl_last_error(struct mtl_error_info* out)`:
  - it copies `{ code, reason, call_seq, detail[128] }` into caller memory;
  - every failing call sets it on the calling thread;
  - DP failures set `code`, `reason` and `call_seq` and leave `detail` empty;
  - a successful call does not clear it, and `call_seq` tells the caller which call failed.
- Portable wait objects replace wait fds:
  `int mtl_<obj>_get_wait_object(obj, struct mtl_wait_object* out)`, where the struct is
  `{ intptr_t native; uint32_t kind /* MTL_WAIT_FD | MTL_WAIT_WIN_HANDLE */; uint32_t reserved; }`.

### A6. Naming (one vocabulary)

| Revision 2 | Revision 3 |
|---|---|
| `mtl_st22_session_create`, `mtl_fmd_*` | `mtl_cvideo_session_create` (compressed video, ST 2110-22), `mtl_fastmeta_session_create` (ST 2110-41); essence names everywhere: video, cvideo, audio, anc, fastmeta |
| `mtl_tx_abort` (return an unsubmitted lease) | `mtl_tx_release`, mirroring `mtl_rx_release` |
| `mtl_tx_cancel` (pull a queued unit) | `mtl_tx_withdraw`; its outcome is `FLUSHED/WITHDRAWN` |
| `mtl_*_cancel_waiters` / `mtl_*_resume_waiters` | `mtl_*_interrupt` / `mtl_*_uninterrupt`; `mtl_instance_interrupt_all` / `mtl_instance_uninterrupt_all` |
| `MTL_STOP_ABORT` | `MTL_STOP_FLUSH`, matching the FLUSHING state and the FLUSHED status |
| `mtl_session_flush` (stays RUNNING) | `mtl_session_discard_queued(s, const struct mtl_discard_params*)`, with optional rebase (06) |
| `MTL_TX` / `MTL_RX` | `MTL_DIR_TX` / `MTL_DIR_RX` |
| `mtl_session_get_state` | kept as the cheap DP getter; **returns the state as the non-negative result** (no out pointer). `get_status` is the full CP copy |
| `mtl_session_update_flow` (one leg) | `mtl_session_update_flows(s, legs, n, const struct mtl_activation*)`, all-or-nothing |
| `mtl_session_api_init(MTL_API_VERSION)` | removed; `api_version` in `mtl_instance_params`; one version space (`MTL_VERSION_NUM`) |
| `mtl_session_stats_new_epoch` | removed; counters are cumulative only |
| `mtl_last_error_detail` | `mtl_last_error` |

### A7. Initialisers and defaults

- Every input struct has an exported `void mtl_<struct>_init(struct mtl_<struct>*)` that
  sets `struct_size` and zeroes the rest.
- Value-initialiser macros `MTL_<STRUCT>_INIT(...)` exist **only in C**
  (`#if !defined(__cplusplus)`) and yield values, never pointers. No compound-literal
  pointer macros exist.
- **Zero-default rule** (header lint in CI): no field of any input struct has a non-zero
  default. The fields that had one are renamed:
  - `numa_socket` / `numa_node` → `uint32_t numa`, where 0 = auto (the ports' socket) and
    `MTL_NUMA(n)` = n + 1;
  - `fmd_dit` / `fmd_k` "0xff = off" → `fmd_dit` / `fmd_k` plus flag bits
    `MTL_FLOW_MATCH_FMD_DIT` / `MTL_FLOW_MATCH_FMD_K`;
  - every other `-1` or "max = off" default is removed the same way.
- One **defaults table** in 09 lists every field whose zero means something other than
  zero, and CI checks it against the header.
- Flags are plain integer literals in the header (`0x1u`, `0x2u`, …), not `(1ull << n)`.
- The header marks every field and function with `/* since 0.N */`.
- `mtl_struct_known_size(enum mtl_struct_kind)` lets an app ask which size the running
  library understands.
- The strict rule stays: non-zero bytes beyond the known size return `-MTL_EINVAL`. It is
  documented as a compatibility policy, with the version-check pattern.

### A8. Where a knob lives (11 §2)

| Mechanism | Holds |
|---|---|
| `mtl_session_config` field | cross-essence, validated at create |
| media config (`mtl_video_config`, …) | essence-specific; the seven essence-specific fields move here from `timing`/`options` |
| `next` extension block | optional feature groups (progressive, RTCP, test) |
| key-value option | backend-specific only, enumerable through `mtl_session_option_list` |
| flag bit | orthogonal booleans |

Reserved tails are sized by observed growth: `pool.reserved[6]`, and `timing` ≥ 96 B
spare.

### A9. Call classes are enforced

- In debug builds, a thread-local "current class" is set at every entry point.
- `mt_rte_zmalloc`, mutex lock, sleep and `info()`/`warn()` assert that the class is not
  DP, DPC or AS.
- A signal-safety test raises the signal inside every CP call under a `malloc` and
  `pthread_mutex_lock` interposer.
- `MTL_API_WT` means "DP when `timeout == 0`"; that is enforced dynamically.
- **DP from a busy-loop thread** (user tasklet, RX packet lcore) takes the inline-safe path:
  - it only *trylocks* the reaper lock, returning `-MTL_EAGAIN` if contended;
  - it never drains an eventfd (the next app-thread call drains it).
  - The inline-safe verbs are: `mtl_tx_acquire`/`mtl_rx_dequeue`/`mtl_tx_reap` with
    `timeout == 0`, `mtl_tx_submit`, `mtl_tx_release`, `mtl_rx_release`, and the getters.
  - Every other call from a busy-loop thread returns `-MTL_EDEADLK`.

### A10. Waiters and multi-threaded use

- The rule "one waiter per session" is replaced by **one waiter per wait target**, each
  target with its own armed bit:
  - buffers (acquire);
  - results (reap);
  - RX ready (dequeue);
  - events.
- `mtl_tx_acquire`, `mtl_tx_release` and `mtl_rx_release` are MP-safe by default, because
  they already CAS the slot.
- Only `mtl_tx_submit`/`mtl_tx_publish` need one submitting context.
  `MTL_SESSION_MT_SUBMIT` makes them MP-safe too, and it is **required** for exported
  framework pools.
- The reaper lock is elided when the session declares `MTL_SESSION_SINGLE_READER`.

### A11. The instance boundary comes first

- Q-ABI-2 is answered yes: versioned `struct mtl_instance_params` (`struct_size`,
  `api_version`, port specs, queue counts, lcores, time source, flags) lands **before** the
  new API, as Phase 0 work.
- The default instance has a published **merge table**, with every field classified as:
  - *invariant*: a mismatch returns `-MTL_EINVAL` with reason `INSTANCE_PARAM_MISMATCH`;
  - *mergeable*: ports must already be open in v1; queue counts are summed up to the
    hardware maximum at first open only;
  - *ignored on second acquire*: reported in `mtl_instance_get_info`.
- Runtime `mtl_port_open` leaves v1 and moves to Phase 6, where it takes
  `struct mtl_port_params`. In v1 a second caller whose ports are not open gets
  `-MTL_ENODEV` with reason `PORT_NOT_OPEN`.
- `mtl_instance_release` only drops a reference. The instance lives until the last
  DESTROYING session retires. Process exit without release is supported.

### A12. The test substrate ships first

- **Null backend.** Port spec `null:<n>`. It completes units at their scheduled time from
  the instance clock and needs no NIC, root or hugepages. It ships in the library as
  experimental (Phase 1).
- **Test time source.** `mtl_time_test_source(mt, int64_t base_tai_ns, struct mtl_rational rate)`
  and `mtl_time_test_advance(mt, ns)`. The published time base honours them.
- **Fault injection.** `mtl_debug_inject(mtl_object, enum mtl_fault, const struct mtl_fault_params*)`
  with these faults: `LEG_DOWN`, `LEG_UP`, `TX_QUEUE_HANG`, `PORT_RESET`, `PTP_STEP(Δ)`,
  `PTP_LOST`, `MANAGER_LOST`, `FORCE_ERROR(reason)`, `DROP_PKTS(pattern)`. It is built with
  `-Denable_debug_api=true` and lives in `mtl_debug.h`, in Phase 1.
- `gtest.sh` gains `nicctl.sh`-driven link-down and VF-reset steps for the I tier.

### A13. L4 "simple" layer (`mtl_simple.h`)

```c
int mtl_simple_tx_open(const char* port_spec, const char* flow_spec, uint32_t w, uint32_t h,
                       const char* fps, const char* format, mtl_simple_h* out);
int mtl_simple_tx_frame(mtl_simple_h, void** addr, uint32_t* stride, int64_t timeout_ns);
int mtl_simple_tx_send(mtl_simple_h);
int mtl_simple_rx_open(/* same */);
int mtl_simple_rx_frame(mtl_simple_h, const void** addr, uint32_t* stride, int64_t timeout_ns);
int mtl_simple_rx_done(mtl_simple_h);
int mtl_simple_close(mtl_simple_h);
```

- A simple session is started at open, with a library pool and completion NONE.
- Errors carry `mtl_last_error`.
- A simple handle bridges to the full handles: `mtl_simple_session(h)`.

### A14. Plan premises

- The function-count argument is dropped. The case rests on premises 2–5 and named issues.
- An honest surface count sits in 01.
- The incremental alternative is costed in 02 §2.3.
- NG1 is rewritten.
- New **Phase 0.5** (timeline helper over the legacy API) and **engines-first**: every E1–E10
  engine fix reaches legacy users. Bugfixes are on by default. Wire-visible behaviour
  changes stay behind a legacy flag and are on by default only in the unified API.
- Effort estimate and FTE scenarios go in 14.
- Numeric performance budgets go in 13 §8.
- A design review with external consumers is a Phase 0 exit criterion.
- The decision package is shrunk to ≤ 10 maintainer decisions; every other question
  becomes "Proposed default, object by review".

## Part B — Verdicts on every C5 finding

Every numbered finding of C5, and every bullet of its minor sections, has one entry. "Where"
names the document and section that changed; the documents themselves carry the change
marked **r3** with a `[C5 §x.y]` citation.

Verdicts: **Accepted** — changed as C5 proposed; **Accepted, modified** — changed, differently
from the remedy, for the stated reason; **Rebutted** — not changed, with evidence;
**Deferred** — a real gap, scheduled to a named phase.

### §1 The plan: premises, scope, sequencing

- **§1.1 effort 50–75 EM — Accepted, modified.** 14 §4 effort table per phase (C5's EM beside
  r3's) and FTE scenarios; 00 phase table carries EM. *Why:* order of magnitude accepted; r3 adds
  scope, so ≈ 55–85 EM. Velocity re-checked: 610 commits in 2026; 133 `lib/`+`include/` commits in
  six months (≈ 22/month, +8.5 k/−10.2 k); one author 74 of 133
- **§1.2 "475 functions" premise — Accepted.** 01 §1 rewritten (four facts + #1622, #1653, #1620,
  #1341); 01 §1.1 honest count table; 00 evidence table drops the premise. *Why:* at `545a266a`:
  475 functions, ≈ 154 colour conversion, 104 pipeline verbs, 75 `*FLAG*` defines; the sketch has
  214 today by `sketch/check.sh` (≈ 34 `*_init()`, ≈ 21 null/eq twins)
- **§1.3 incremental alternative — Accepted.** new 02 §2.3: reach table with EM (≈ 6–9 EM, 4–6
  shared), cannot-reach table (GO-1, `struct_size` on ops, removing callbacks, lease/result for
  imports), conclusion engines first; 00 recommendation. *Why:* Q-CORE-1 cannot be decided without
  it
- **§1.4 NG1 dishonest — Accepted.** 01 NG1 rewritten; GO-9 "engines first"; R-MIG-1; 14 §2 per-E
  table (legacy default / opt-in flag / unified) incl. E11, R1, R2; legacy gate every phase; G-99.
  *Why:* bugfixes on for legacy, wire-visible changes opt-in on legacy, on in unified
- **§1.5 A/V answer in weeks — Accepted.** 14 §1.6 Phase 0.5 (`st_timeline_*` ≈ 500 lines vs the
  13 §7 oracle, ST30P flag, ST40P fix, audio start rule, SF-15 salvage); exit = maintainer's
  A/V/ANC example with exact RTP on the legacy API; 00, 01 §7. *Why:* verified RxTxApp's double
  helper at `rxtx_app.c:673-703`
- **§1.5 Phase 0.5 — Accepted.** new 06 §15: `st_timeline_*` contract; `legacy_ts_ns` exact under
  legacy rounding (`st_fmt.c:986-993`). *Why:* 0 mismatches in 800 000 cases
- **§1.6 three passes over the same code — Accepted, modified.** 02 §2.2: the L0 hooks are the
  L2↔engine **slot interface** (hold/submit/done-hook/reclaim; RX hold/release/force-complete);
  Phase 6 re-base committed, go/no-go at Phase 2 exit (S3); ST20 progressive adapter stays. *Why:*
  wrap-now and extract-core become one piece of code; S3 still costs the re-base
- **§1.7 93 questions — Accepted.** 00 "What needs the maintainer" and OPEN-QUESTIONS.md "The ten
  decisions" (M1–M10); every other question has a proposed default logged in DECISIONS.md.
- **§1.8 testing realism — Accepted.** 13: P/BE on every guarantee; L2-only guarantees on U over
  the null backend; UB through the slot interface; §8 numeric budgets confirmed by S0; G-27 BE,
  moved to Phase 2 exit; fault injection funded (§6, §9; Phase 0 design, Phase 1 code). *Why:* 24
  harness files `#include` `.c` and 60 commits touched them in 2026 (C5: 20 and 43)
- **§1.9 success criteria / user validation — Accepted.** 01 §7 and 14 §5 success criteria per
  phase in user terms; external design review (FFmpeg/GStreamer owners, MXL team, external engine
  team) = Phase 0 exit (14 §1.5); P9 operator persona; note that P1–P8 are archaeology.
- **§1.10 deliverables, deprecation, status — Accepted.** 14 §6 guide outline; 14 §7 deprecation
  policy (removal ≥ two `vYY.MM` releases after deprecation); new 15; CI capacity and reviewer
  load in 14 §9; side-findings Status column; SF-15 salvage of `mt_session_event.c` in Phase 0.5.
  *Why:* Windows: 2 of 11 open issues (#1672, #1301); salvage file verified (106 lines, branch
  `pr1610`)

### §2 The header as compiled

- **§2.1 — Accepted, modified.** entry stays 256 B (union size check); every kind ≤ 240 B; CQ
  times `int64_t` TAI ns + `hdr.time_valid`; `mtl_tx_result` 208 B, `mtl_rx_unit` 240 B with
  per-leg times and `leg_count`; `MTL_CQE_RX_MISSING` follow-on (header §15; 10 §1.5). *Why:* A2:
  int64 times keep every field C5 proposed to drop; 384 not needed
- **§2.2 — Accepted.** the header is real (`sketch/`), declares every constant, struct and
  function named in the design (incl. all 34 constants and 15 structs C5 listed,
  `mtl_session_info` with SDP values and the granted SSRC, UDP source port and MACs per leg), and
  compiles in `check.sh` now, not in Phase 1 (10 §1.1, 11 §8). *Why:* A1
- **§2.3 — Accepted.** 37 exported `*_init()`, one per `struct_size` struct (lint in `check.sh`),
  all taking only the struct pointer; `MTL_<STRUCT>_INIT(..)` value macros only under `#if
  !defined(__cplusplus)`, no pointer macros; `examples_cpp.cpp` twin (10 §1.6, §15). *Why:* A7
- **§2.4 — Accepted.** zero-default rule C5; `numa` fields with `MTL_NUMA(n)`; `flow.port` with
  `MTL_FLOW_PORT(n)` (09); `fmd_dit`/`fmd_k` enabled by `MTL_FLOW_MATCH_FMD_DIT/K`;
  `MTL_AUDIO_NO_ABSORB` instead of a sentinel; `numa_socket`/`numa_node` removed (header §8–§11;
  11 §2.1 rule 8). *Why:* A7
- **§2.4 (05/09 fields) — Accepted.** `mtl_mem_desc.numa` and `options.numa` → `uint32_t numa`,
  `MTL_NUMA(n)`; `fmd_dit`/`fmd_k` + `MTL_FLOW_MATCH_FMD_DIT/K`; additionally `mtl_flow.port` → 0
  = the leg's own port, `MTL_FLOW_PORT(n)`; `udp_port` required; `direction` required. *Why:* a
  literal `flows[1]` with `port = 0` would put both legs on one NIC (same class of bug)
- **§2.5 — Accepted.** `mtl_lease_h` for acquire/dequeue outputs and every access-moving verb;
  `mtl_lease_buffer()` (header §3; 10 §1.3). *Why:* A4
- **§2.6 — Accepted.** stride is an argument of `mtl_tx_reap`/`mtl_cq_read`/`mtl_eq_read` (and
  `unit_size` of `mtl_rx_dequeue`); `hdr.size`/`mtl_event.size` are outputs; `struct_size` never
  rewritten (C3, C4; 11 §2.1 rules 3–4). *Why:* A3
- **§2.7 — Accepted.** every hole is a named `reserved`; `-Wpadded -Werror` on C and C++; 106
  structs/unions size-checked (header §21; 11 §2.1 rule 5). *Why:* A1
- **§2.8 — Accepted, modified.** ID 0 null for every type, `MTL_<TYPE>_NULL` values in C and C++,
  inline `is_null`/`eq` with exported twins (`MTL_UNIFIED_NO_INLINE`); the epoch timeline is a
  function `mtl_timeline_epoch(mt)`, not a constant (header §3, §13). *Why:* the epoch timeline is
  per instance, so a compile-time constant cannot name it
- **§2.9 — Accepted.** `mtl_instance_h` in every prototype,
  `mtl_instance_from_legacy`/`to_legacy`; `mtl_instance_params` replaces `mtl_init_params`; typed
  stat getters; lease `session index:16 \| slot:16 \| generation:32`, random non-zero generations
  (C7; header §8, §12). *Why:* A4, A11
- **§2.10 — Accepted.** `*_trywait` → 1 / 0 / < 0 (C1; 10 §5; 12 §2). *Why:* A5
- **§2.10 (trywait semantics) — Accepted.** 04 §5.3, 07 §2.4/§5.1: `*_trywait` returns 1 ready / 0
  armed / < 0 error, never `-MTL_EAGAIN`. *Why:* the revision-2 loop had one code meaning opposite
  things in two adjacent calls
- **§2.11 — Accepted, modified.** strict rule documented as policy with the version-check pattern
  (11 §2.3); `mtl_struct_known_size(kind)`; `since 0.N` on every section banner, inherited by the
  structs, fields and functions in it, and required on anything added later (C9). *Why:* per-item
  markers at one version are noise; the rule forces a marker on any later item
- **§2.12 — Accepted.** `mtl_session_api_init` and `MTL_API_VERSION` removed;
  `mtl_instance_params.api_version`; `MTL_UNIFIED_API_VERSION` in the `MTL_VERSION_NUM` encoding
  (11 §2.5). *Why:* A6, A11
- **§2.13 — Accepted, modified.** `MTL_E*` constants with Linux errno values (≥ 1000 reserved);
  `struct mtl_wait_object {intptr_t native; uint32_t kind; …}` returned by `mtl_*_get_wait_object`
  (header §2, §5; 11 §3). *Why:* one struct instead of two out-params; every code exists on Linux,
  so no private values are needed yet
- **§2.14 — Accepted.** knob-placement rule (11 §2.2); the seven fields moved to
  `mtl_anc_config`/`mtl_audio_config`/`mtl_video_config`/`mtl_cvideo_config`, plus `sender_type`
  and `progressive_late` (09); `pool.reserved[6]`, `timing.reserved[12]` (96 B) (10 §1.7). *Why:*
  A8
- **§2.14 (move into media configs) — Accepted, extended.** 09 §1.2–1.4: the seven fields moved;
  `sender_type` and `progressive_late` moved as well (the A8 rule makes them essence-specific);
  `timing` gets `reserved[12]` (96 B), `pool.reserved[6]`; key-value options named (`MTL_OPT_*`)
  in 09 §1.5. *Why:* —
- **§2.15 — Accepted.** classes enforced in debug builds and by the signal-safety interposer test
  (C10; 11 §2.7); the macros stay as documentation. *Why:* A9 (implementation in 04)
- **§2.16 — Accepted.** `mtl_last_error(struct mtl_error_info*)` {code, reason, call_seq,
  detail[128]} + `mtl_call_seq()`, `mtl_error_name`, `mtl_reason_name`; `enum mtl_state_reason` in
  full, values as 07 §5.4 (header §4–§5; 10 §1.4). *Why:* A5
- **§2.17 — Accepted, modified.** A6 vocabulary everywhere (`cvideo`, `fastmeta`,
  `mtl_tx_release`, `mtl_tx_withdraw`, `*_interrupt`, `MTL_STOP_FLUSH`, `discard_queued`,
  `MTL_DIR_*`); `get_state` stays the DP getter, `get_status` the CP copy;
  `mtl_cq_wait`/`mtl_eq_wait` removed. *Why:* folding them would make every state poll a CP copy
- **§2.18 — Accepted.** `libmtl_unified.so.0.<rev>`, soname bumped per incompatible change, node
  `MTL_UNIFIED_EXPERIMENTAL`, private `MTL_INTERNAL`, `MTL_1.0` at freeze; Q-ABI-1 soname for
  libmtl in Phase 0 (11 §2.6). *Why:* Part A 2.18
- **§2.19 — Accepted.** `view`, `unit`, `hint` are `MTL_NULLABLE`; cache-by-index documented
  (`mtl_buffer_view` comment, ex06, C++ twin); `unit_size = 48` gives a header-only fill (header
  §16–§17). *Why:* Part A 2.19
- **§2.20 stats — Accepted.** `mtl_session_stats` has no `version`, no `epoch_start`, `name` and
  `created`, a named `union mtl_dir_stats`, `mtl_leg_stats`, `mtl_queue_gauges` (gauges plus
  cumulative entries/exits per lease state) and a fixed `union mtl_media_stats` tail (header §12).
  *Why:* 8.8, 8.9, rules 2–3
- **§2.20 legs / reject slots — Accepted, modified.** `MTL_MAX_LEGS 2` in configs; CQ records keep
  `[2]` arrays plus `leg_count`; overflow detail in the follow-on kind; `MTL_RX_REJECT_SLOTS` 32.
  *Why:* int64 times made the `[2]` arrays fit; a leg count keeps them honest
- **§2.20 enums / UNSET — Accepted.** every status enum starts `*_UNSET = 0`; `enum
  mtl_state_reason`, `mtl_tx_reason`, `mtl_rx_reject`; fields name their enum in comments. *Why:*
  A5
- **§2.20 read returns — Accepted.** C1/C2: arrays return a count ≥ 0 (0 only with timeout 0),
  singles `-MTL_EAGAIN`, expiry `-MTL_ETIMEDOUT`. *Why:* A3
- **§2.20 pointer structs — Accepted.** `struct_size` = sizeof on the target ABI; 64-bit only
  (`#error`); `*_init()` mandatory for bindings; `MTL_ADDR` → `uintptr_t` for view addresses (11
  §2.1 rule 9).
- **§2.20 `mtl_time_convert` — Accepted.** `uint32_t to_clock`. *Why:* rule 6
- **§2.20 sentinels — Accepted.** `struct mtl_activation` with `enum mtl_activation_kind`;
  `MTL_TIMEOUT_INFINITE = -1` is the only negative sentinel.
- **§2.20 `uint32_t* state` — Accepted.** `mtl_session_get_state()` and `mtl_group_get_state()`
  return the state.
- **§2.20 `mtl_time` helpers — Accepted.** "every time in a CQ record is TAI or invalid" (C8,
  `hdr.time_valid`); `mtl_time_diff_ns(a, b, time_valid, required, &out)`. *Why:* A2
- **§2.20 named rates — Accepted, modified.** `enum mtl_fps` IDs + `mtl_fps_rational(id)` +
  `mtl_fps_parse()`; C-only `MTL_RATIONAL(n, d)`. *Why:* a rational initialiser macro is not
  assignable in C++ or bindings; an ID plus a function works everywhere
- **§2.20 SWIG/bindgen, literals — Accepted.** `MTL_API` empty under `SWIG`/`__bindgen`, which
  also select the exported twins; all flags plain literals.

### §3 First-time user, samples, bindings

- **§3.1 — Accepted, modified.** `mtl_simple.h` with A13's seven calls (open takes a port-spec
  string, not an instance) plus `mtl_simple_session()` and `mtl_simple_interrupt()`; 10 §2, §14.
  *Why:* the port-spec form removes the instance concept too; an AS interrupt keeps signal
  handling possible
- **§3.2 — Accepted.** every example checks start/submit/release returns and releases the lease on
  submit failure; `MTL_BLOCKED_APP_LEASES`; `dequeue` in CREATED/ARMED/STOPPED is
  `-MTL_EAGAIN`/`-MTL_ETIMEDOUT` (header §17). *Why:* A5
- **§3.2 — Accepted.** 03 §3.2 (RX `dequeue` in CREATED/ARMED/STOPPED → `-MTL_EAGAIN` / normal
  wait → `-MTL_ETIMEDOUT`); 07 §2.3 `MTL_BLOCKED_APP_LEASES`; examples are. *Why:* a forgotten
  `start` or a leaked lease must not look like shutdown or back-pressure
- **§3.3 — Accepted.** null backend `"null:<n>"` (port-spec grammar, `MTL_BACKEND_NULL`),
  `mtl_debug.h` with `mtl_time_test_source/advance` and `mtl_debug_inject(struct mtl_object, …)`;
  ex12 RX uses `"null:1"`. *Why:* A12
- **§3.3 no null backend / time / fault injection — Accepted.** 02 §2 null backend; 13 §9
  substrate table; R-TEST-1…4; G-92, G-93; U tier on one long-lived null instance (13 §6.1);
  Python GIL test on the null backend; 15 §6 gating.
- **§3.4 moving cursor — Deferred (Phase 4).** declared now: `MTL_POOL_DYNAMIC`,
  `mtl_tx_acquire_dynamic`; migration row (10 §16). *Why:* implementation needs the dynamic slot
  binding of 05 (Phase 4)
- **§3.4 library-pool forward — Accepted.** `mtl_session_get_pool_region` (+
  `info.pool_slot_pitch`), `mtl_session_get_buffers`, `mtl_buffer_get_desc`, `.hold` (ex09).
  *Why:* 6.2
- **§3.4 `rtp_timestamp_delta_us` — Accepted.** `timing.media_time_offset_ns` (ns, TAI). *Why:*
  5.4
- **§3.4 VSYNC / sched idx — Accepted.** `MTL_EVENT_EPOCH_TICK` + `MTL_EQ_SUB_EPOCH_TICK`;
  `mtl_session_info.sched_index`.
- **§3.4 migration gaps — Accepted.** 10 §16 adds `put_frame_abort`, `frame_size` → `unit_bytes`,
  `USER_P_MAC` → `MTL_FLOW_USER_MAC`, Rust trampolines → CQ reads, `ops.linesize`,
  `get_pacing_params`, `mtl_init`, `update_destination`, `fifo_size`, `DISABLE_AUTO_DETECT`, the
  interlaced field-rate `fps` (citations verified at 545a266a). *Why:* Part A 3.4, 05/06/09
  requests
- **§3.4 b1 (moving-cursor ext frame) — Accepted.** `MTL_POOL_DYNAMIC` (05 §5.5), or F attached
  buffers ≤ `max_count`; 05 §2.1, 09 §8.3. *Why:* the sample walks a `mtl_dma_mem_alloc` arena
  (`tx_st20_pipeline_ext_frame_sample.c:80-99`, `:147-162`)
- **§3.4 b2 (RX → TX with a library RX pool) — Accepted.** `mtl_session_get_pool_region` + TX
  buffers + `hold`; forward pools may use NONE (no reap loop); 05 §5.3–5.4, 09 §8.3. *Why:*
  `rx_st20p_tx_st20p_fwd.c:96-109`, `:161-165`
- **§3.4 `rtp_timestamp_delta_us` — Accepted.** 06 §4.6, §5.4, §13: `media_time_offset_ns`
  (AUTO/INDEX: stamped M only). *Why:* `tx_st20p_app.c:311`; `st_tx_video_session.c:762-806`
- **§3.5 — Accepted.** `mtl_cq_read(cq, void* buf, size_t record_size, max, timeout)`;
  `mtl_lease_copy_in/out`; `MTL_ADDR` → `uintptr_t`; reference Python wrapper as a Phase 1
  deliverable with scope (11 §4). *Why:* Part A 3.5
- **§3.6 waiters — Accepted.** one waiter per wait target
  (`MTL_WAIT_ACQUIRE/DEQUEUE/RESULTS/EVENTS`) (header §5; 10 §1.4). *Why:* A10
- **§3.6 cookie in NONE — Accepted.** non-zero `user_cookie` with `MTL_COMPLETE_NONE` →
  `-MTL_EINVAL`, `MTL_REASON_COOKIE_WITHOUT_RESULTS`; ex07 no longer sets a cookie. *Why:* fail
  fast, no silent drop
- **§3.6 `pool.count = 0` — Accepted.** 0 = video `max(min_count_direct, 3)`, others 4 (header §9;
  10 §2). *Why:* Part A 3.6
- **§3.6 bullet 1 (waiters) — Accepted, modified.** 04 §5.1: one armed word per wait target
  (BUFFERS, RESULTS, RX_READY, EVENTS) that **counts** waiters. *Why:* A10 says "one waiter per
  target"; counting costs the tasklet nothing extra and makes two MT_SUBMIT producers blocked in
  acquire legal (deviation D2)
- **§3.6 b3 (`pool.count` default) — Accepted.** 05 §5, 09 §9.1: video TX `max(min_count_direct,
  3)`, video RX 3, cvideo 3, audio/ANC/fastmeta 4. *Why:* —
- **§3.7 — Accepted.** field list for the 09 defaults table below (Requests, 09); every default C5
  found unstated has a header comment (`payload_type`, `dscp` = CS0, `late_policy`, `tx_queue`
  AUTO, `min_tx_delay_ns`, `pool.count`, `snap_mode` = NEAREST); RESLOT stays visible in NONE via
  `tx.slots_empty`. *Why:* 09 owns the table (09 §9)
- **§3.7 — Accepted.** 09 §9: three tables — zero replaced (57 rows, each with today's value where
  one exists), zero as a named mode (30 rows), required fields; the CI lint rule; the
  RESLOT-visibility note. *Why:* defaults verified in code: PT `st_pkt.h:15-18` (fastmeta is
  **115**, not 114), TTL/TOS `st_tx_video_session.c:944-945`, SSRC `:966`, rx burst
  `st_rx_video_session.c:3384-3389`, audio FIFO `st30_api.h:126` +
  `st_tx_audio_session.c:1931-1934`, queue selection `mt_queue.c:165`,
  `st_tx_audio_session.c:2078-2124`, `st_tx_ancillary_session.c:1693-1694`,
  `st_tx_fastmetadata_session.c:1438-1439`
- **§3.7 defaults — Accepted.** 06 §7.2, §5.1 (CAPTURE `min_tx_delay` = unit + `pickup_lead_ns`, L
  = 2 at 1080p59.94), §4.3 (NEAREST). *Why:* numbers below

### §4 Threading, completion and hand-off

- **§4.1 — Accepted.** 04 §3.3: inline-safe subset named (trylock only, no eventfd drain/write, no
  log, no alloc); every other WT/DPC/CP call from a busy-loop thread → `-MTL_EDEADLK`; 04 §3.4
  debug enforcement. *Why:* A9
- **§4.2 — Accepted.** 04 §3.2: immediate vs boundary commands; check every session visit outside
  a burst (verified the RX handler visits every session, `st_rx_video_session.c:3506-3535`, and
  the transmitter returns while waiting, `st_video_transmitter.c:180-199`); `sch_sleep_wakeup` on
  post (`mt_sch.c:52-56`); CP applies when detached; ack timeout 100 ms → ERROR/`CMD_TIMEOUT`,
  then lock-based detach, then quarantine. *Why:* C5's four cases (a)–(d) are all real
- **§4.3 — Accepted.** 04 §4.5 (bounded cleanup → queue stop/start on a worker for dedicated
  queues; shared queues reset when the last user goes; escalate to `PORT_RESET`; `SESSION_RETIRED`
  only after); 03 §6.2, §6.4; worker listed as completing context (04 §4.1.1); ERROR entry runs
  the same path (03 §3.6). *Why:* verified: no `rte_eth_dev_tx_queue_stop` anywhere in `lib/`;
  `mt_dev_tx_queue_fatal_error` only marks (`dev/mt_dev.c:1840-1869`). Whether iavf/ice release
  chained ext mbufs on queue stop is new spike S8
- **§4.4 — Accepted.** 04 §9.1: frame pools keep the atomic release; no app thread touches an
  MP/MC mempool a tasklet uses; future mbuf API gets refill + return rings; non-EAL threads have
  no cache. *Why:* verified `rv_put_frame` is an atomic decrement
  (`st_rx_video_session.c:222-232`)
- **§4.5 — Accepted.** 04 §8.1: per-socket seqlocked record, refresh ≤ 100 ms, least-squares TSC
  servo, slewed publication (never steps except declared `TIME_STEP`), monotonic/realtime
  cross-timestamps; spike S7 bound goes to 13 §8. *Why:* verified one PTP read per frame
  (`st_tx_video_session.c:695`) and one-shot `tsc_hz` (`mt_main.c:116`, `:578`)
- **§4.6 — Accepted.** 04 §4.3: per-scheduler (and per packet-lcore) ready-summary words owned by
  the CQ; members point at the CQ's single armed word. *Why:* O(members) per read was real at 1024
  RX audio sessions per scheduler (`st_header.h:50`)
- **§4.7 — Accepted.** 04 §4.1: tasklet half {state, result} and app half {generation, seq,
  cookie, hold, control} on separate lines; generation written by the acquirer. *Why:* the app
  still writes the state word at hand-over points; that single transfer is inherent and said so
- **§4.8 — Accepted.** 04 §7.1 answers Q-THR-10 for Phase 1: W2 default in thread mode,
  `rte_thread_register`, G-39 per mode; 04 §5.2 states that thread mode is unpinned so W2 does not
  conflict there. *Why:* verified plain `pthread_create` (`mt_sch.c:285-287`, `:252-257`) and no
  `rte_thread_register` in `lib/src`
- **§4.9 — Accepted.** 04 §4.4 RX deadline hook (due = first-packet arrival + period +
  `rx_flush_offset`, force-complete on command), Phase 1; waker max sleep 1 ms with no due time
  (04 §5.2); 03 §3.4/§3.5. *Why:* verified completion only when full or evicted
  (`st_rx_video_session.c:1850-1858`, `:1214-1221`)
- **§4.9 / §8.4 timing — Accepted.** 06 §11.7: due = first arrival + unit period +
  `rx_flush_offset_ns`, capped at presentation; §11.4 ARMED discards media < t. *Why:* matches the
  04 §4.4 RX-hook call
- **§4.10 — Accepted, modified.** 04 §5.2: W2 auto-selected below 1 ms unit period as the
  **designer's recommended default subject to Q-THR-2**, with its pinned-core cost stated (one
  non-blocking `write()` per wake, µs-scale, ≤ one per unit per armed target); alternatives the
  maintainer can pick: W0 polling, or W3 with the stated latency; `MTL_SESSION_WAKE_WAKER`
  opt-out; S1 at 125 µs and 1 ms. *Why:* designer clarification: W2 in lcore mode is a syscall
  on a pinned core, which the maintainer's rule forbids; the decision is the maintainer's
  (deviation D7)
- **§4.11 head-of-line — Accepted.** 03 §4.1 "Order", 04 §4.1: NONE frees DONE slots out of order
  at acquire; ALL/EXCEPTIONS copy the result into the unread ring and free the slot; results
  published in completion order, sorted by `seq` within a read. *Why:* verified chain two-leg
  completes once (`sh_info`, `st_tx_video_session.c:235-237`, `:1295-1298`); the real out-of-order
  cases are late drop (`st20_pipeline_tx.c:137-178`), recovery
  (`st_tx_video_session.c:4293-4301`), withdraw. G-09 rewritten
- **§4.11 completing contexts — Accepted.** 04 §4.1.1 table: PMD free in `tx_burst`, builder
  no-chain/ST22, recovery, destroying app thread (removed), queue-reset worker (new), app thread
  in `put_frame`, plugin thread, RX tasklet, RX packet lcore (own wake word), RX DMA drain. *Why:*
  ST22 "done" is at `st_tx_video_session.c:2655-2659` (C5: `:2637-2641`); destroy flush at
  `:2722-2728` (C5: `:2725`, correct)
- **§4.11 `sh_info` — Accepted.** 04 §4.1.1: recovery never touches `sh_info`; side finding N3.
  *Why:* verified `rte_mbuf_ext_refcnt_set(&frame->sh_info, 0)` at `st_tx_video_session.c:4300`
  (C5 cites `:4227-4229`) while the other leg is only padded (`:4284-4288`); the wrap/re-fire
  effect is **[inferred]**
- **§4.11 submission order — Accepted.** 04 §4.4 hook "sequence at submit"; 03 §4.1 transition
  table. *Why:* verified seq at `get_frame` (`st20_pipeline_tx.c:806-808`, C5: `:797-798`) and
  oldest-first pick in the misnamed `tx_st20p_newest_available` (`:62-77`)
- **§4.11 RX/ST22/ST30 hook gaps — Accepted.** 04 §4.4 rows: `notify_frame_ready < 0` keeps the
  frame (`st_rx_video_session.c:939-944`, `:1044-1049`); tasklet CAS READY→RECEIVING for RECLAIM
  (03 §4.2); DMA-busy drop counted `MTL_RX_REJECT_DMA_BUSY` (`:1202-1208`); incomplete delivery
  always enabled internally (`:976-982`, `st20_pipeline_rx.c:589-594`); `BY_INDEX` lookup; st22p
  CAS. *Why:* the st22p item is **Accepted, modified**: `framebuff->stat` *is* `_Atomic uint32_t`
  (`st22_pipeline_tx.h:23`); the defect is a check-then-store that is not a compare-exchange
  (`st22_pipeline_tx.c:263-265`), not a non-atomic field
- **§4.11 wording ("never write another layer's state") — Accepted.** 04 §4.4 intro: the hooks are
  new pipeline entry points L2 calls (`st20p_tx_hold_slot` / `release_slot` …), i.e. the extracted
  engine boundary. *Why:* consistent with Part A 1.6
- **§4.11 handle tables — Accepted.** 03 §2.2: grow-only chunked tables (256 chunk pointers × 256
  entries, 65 536 per type), never freed before teardown; lease = session index:16, slot:16,
  generation:32; generations random-seeded, skip 0. *Why:* C5's 256-pool cap came from
  `pool/owner:8`; the engine's maximum is 29 808 sessions per instance, inside 16 bits
- **§4.11 RMW count — Accepted, modified.** 03 §2.3, 04 §4.1: with `MTL_SESSION_SINGLE_READER` and
  no MT_SUBMIT the reaper lock and in-flight counter are elided for acquire/submit/reap/dequeue;
  releases keep the counter; progressive RX coalesces to one pending `ready_rows` per unit (07
  §1.3). *Why:* releases run from any thread during a deferred destroy, so their counter cannot be
  elided safely (deviation D4)
- **§4.11 unverified numbers — Accepted.** 03 §2.3 (in-flight RMW → S3), 04 §4.1 (fence inside
  `tx_burst` → S3), 04 §4.4 (completion latency depends on `tx_rs_thresh`/`tx_free_thresh` → S6),
  04 §5.2 (W2 1–2 µs awake / 3–6 µs from C6 → S1, marked unverified), reaper scan cost → S3.
  *Why:* every number is now labelled with the spike that produces it

### §5 Timing

- **§5 intro (1188-1196) — Accepted.** 06 §4.4 cites the exception (past RTP accepted after 20
  redundant errors per port, `st_header.h:81`); §11.5 replaces it with a per-unit relock. *Why:*
  verified `st_rx_video_session.c:1188-1196`
- **§5.1 — Accepted, modified.** 06 §3.1: `T0 = ceil(max(now + lead + preroll − k·P_k, B)/G)·G`, S
  = T0 + k·P_k; interlaced adds TFRAME to G (§3.3); lead = max(0, `min_submit_lead_ns`); also
  single-session `AT_MEDIA_INDEX k`. *Why:* k is not restricted; T0 is on the grid by
  construction; numeric check below
- **§5.2 — Accepted.** 06 §7.4, §3.1: `mtl_start_params.preroll_ns`; horizon from `max(now, S)`;
  queued unit beyond S + horizon → atomic `-MTL_ERANGE`/`BEYOND_HORIZON`; M < S →
  `FLUSHED/BEFORE_START`. *Why:* C5's 2 s audio case has an outcome; T0 computable from documented
  inputs
- **§5.3 — Accepted, modified.** 06 §4.5: fixed "+floor(TFRAME·90000/2)" deleted; one rule
  `floor(M(field)·90000)`; per-format offset table; G-22 rewritten. *Why:* C5's "G-19 and G-22
  cannot both pass" is **rebutted** for broadcast rasters (constant +1501 at 1080i59.94);
  +750/+751 only for a 119.88-field raster (a legacy field-rate `fps` mistaken for the frame rate,
  06 §3.4)
- **§5.4 — Accepted.** 06 §7.1: `min_submit_lead_ns` = M − deadline, new `pickup_lead_ns`, both in
  `get_info` and the query; worked number 1.279–1.294 ms; §4.6 `media_time_offset_ns`; §10.8
  recipes; no RESLOT for TAI. *Why:* C5's ≈ M − 1.3 ms confirmed; the redefinition makes "M ≥ now
  + lead" exact; `RESLOT_REPORT` not needed
- **§5.5 (recipe) — Accepted.** 10 §12 and `ex10_processor.c`: TAI + CAPTURE + `min_tx_delay_ns`
  budget, derived RTP; audio first-sample rule; ANC shares the video media time; PASSTHROUGH +
  AUTO → `MTL_REASON_PASSTHROUGH_AUTO`; 11 §6.2 processors. *Why:* Part A 5.5
- **§5.5 — Accepted.** 06 §5.4, §10.8: TAI + `u.timing.media_tai_ns` + CAPTURE + `min_tx_delay`
  budget; PASSTHROUGH + AUTO → `-MTL_EINVAL`; audio passthrough (first sample, +S, else
  DISCONTINUITY); ANC via the video's media time. *Why:* fixed L; derived RTP = input RTP for
  compliant input
- **§5.6 — Accepted, modified.** 06 §10.7: EPOCH + INDEX with `mtl_rational_index_at`, or AT_TAI
  with the same `tai_ns` and an explicit grid; §3.1: mismatch → `-MTL_EEXIST`; names per instance,
  separate namespaces. *Why:* `mtl_timeline_index_at` takes the session (period); explicit grid
  needed because the implicit snap depends on local members
- **§5.7 — Accepted, modified.** new 06 §11: EPOCH/AT_TAI only; exact `media_index` inverse; link
  offset; RX groups (one `link_offset_ns`, filters armed together); stale/gap/relock; receiver A/V
  answer (§11.6, `mtl_rx_align`); due time; A2 `mtl_rx_timing`. *Why:* the area's `floor((media −
  T0)/P)` is off by one on half the 59.94 frames (check below)
- **§5.8 — Accepted, modified.** 06 §4.4: `DROPPED/DUPLICATE_INDEX`, `DROPPED/BEHIND`; §4.3:
  NEAREST for every source kind with TFRAME/8 hysteresis; LOCKED_PHASE opt-in, RELOCK after 3; §8:
  `audio_absorb_samples`; §10.8: TAI from `base_time + running_time`. *Why:* C5's REANCHOR default
  rejected: it moves φ at each re-anchor, forever when drifting; NEAREST keeps RTP on N·TFRAME and
  adapts by drop/gap
- **§5.9 — Accepted.** 06 §10.8: TAI recommended; `mtl_session_discard_queued` with
  `MTL_DISCARD_REBASE` + `first_index` → next feasible slot; groups re-base video first. *Why:* A6
  verb; start keeps `-MTL_ERANGE`
- **§5.10 — Accepted, modified.** 06 §10.6: k = first member in add order; add on RUNNING;
  timeline `frame_offset` (owner = first-started video, updated by REBASE); partial-failure
  restart; member start in a RUNNING group allowed. *Why:* only same-rate frame members inherit:
  800.8 samples/frame is not an integer
- **§5.11 — Accepted.** 06 §5.1: §7.6.3 justification and "L ≥ 2 rejected" dropped;
  `link_offset_budget_ns` → `max_slot_delay` → `timing_warning`; JT-NM windows cited. *Why:* R12
  §2 (-10 §7.6.3 bounds the RTP value) and R12 §3.4 re-read
- **§5.12 — Accepted, modified.** 06 §5.2: `rate_mode` CBR (0, default) / VBR_MAX
  (opt-in, non-compliant flag); ceiling incl. box header (`st_tx_video_session.c:2481`); sync
  `-MTL_ENOSPC`; per field; row fixed (TVD, VRX0 = 0, `:582-585`). *Why:* ST 2110-22 §4/§5.2 (R12
  §6); the FFmpeg rule `pkt->size ≤ frame_size` (`mtl_st22p_tx.c:266-286`) is the CBR ceiling
- **§5.13 — Accepted.** 06 §4.2, §4.8, §5.2: R = 90000; `rate` 0 = video rate (grid member) or
  explicit (free-running); `interlaced`; `fastmeta_target_delay_ns`; E12. *Why:* verified
  `st_tx_fastmetadata_session.c:202-234`, `st41_api.h:134-175`
- **§5.14 — Accepted, modified.** 06 §8: grid kept; silence fills `[e, s)`; data at its exact
  sample (`samples_padded`); overlap trimmed (`samples_dropped`); only DISCONTINUITY re-phases.
  *Why:* no audio lost, grid kept
- **§5.15 live ANC (Q-TIME-18) — Accepted — decided.** 06 §5.5: RTP = RTP_v(k), window of frame
  E⁻¹(M) + L_v; standalone uses own L; `anc_window_anchor` AUTO/MEDIA; fastmeta follows. *Why:*
  -40:2023 §6.1/§6.3 re-read: TAD mirrors -21 TVD, N undefined in -40, association via RTP (§5.4,
  §6.3 NOTE); unblocks `gst_mtl_st40p_tx.c:719-720`
- **§5.15 late policy — Accepted.** 06 §7.2: media mode wins; RESLOT on INDEX/TAI → `-MTL_EINVAL`;
  `tx.slots_skipped` with NONE. *Why:* answers §3.7 AUTO + PLAYBACK
- **§5.15 hint formula — Accepted.** 06 §7.6: hint = admission test. *Why:* follows from §7.1
- **§5.15 negative indices — Accepted.** 06 §4.4: legal on EPOCH/AT_TAI when M(k) ≥ S. *Why:* G-19
  scope stated
- **§5.15 PsF — Accepted.** 06 §3.4, §4.2, §5.2: paced as interlaced, one unit/index/RTP per
  frame. *Why:* -10 §7.6.1, -21 §6.3.3, -40 §5.5
- **§5.15 AUTO backward step — Accepted.** 06 §2.4: wait ≤ horizon; beyond → `TIME_STEP` +
  AUTO-cursor re-anchor, flagged. *Why:* EPOCH cannot re-anchor
- **§5.15 clocks — Accepted.** 06 §2.1: `MTL_CLOCK_TSC` removed; `mtl_time_convert` via 04 §8
  cross-stamps; `mtl_time_cross_timestamp`. *Why:* record owned by 04 §8
- **§5.15 ST40 limits — Accepted.** 06 §4.7: `anc_count`/`udw_count` ≤ 255, line 0, RX
  `total_lines`, 2·pts + field, one UDW run. *Why:* `st40_api.h:308`, `:901`
- **§5.15 TAI non-epoch — Accepted.** 06 §4.6.
- **§5.15 hazard + nits — Accepted, modified.** 06 §1 note, §7.1, §13 note: 604–619 µs (54.4–55.7
  ticks) by VRX0 (`st_tx_video_session.c:566-579`, `:3365`). *Why:* direction **rebutted**: legacy
  ANC stamps its epoch (`st_tx_ancillary_session.c:420-427`, `:462`); the hazard is legacy ST20

### §6 Memory, buffers and data paths

- **§6.1 (03 side) — Accepted.** 03 §4.3 "Stride-aware DIRECT" replaces the wrong "DIRECT later if
  the builder learns strides". *Why:* verified `st20_tx_ops.linesize`
  (`include/st20_api.h:1215-1218`), builder offsets (`st_tx_video_session.c:1120`, `:1225`,
  `:1272`), padding copy (`:1274-1288`), sample (`rx_st20_tx_st20_split_fwd.c:263-265`,
  `:284-287`)
- **§6.1 — Accepted.** 05 §4.2 (new): any `stride ≥ row_bytes` is DIRECT for ST20 TX and RX; one
  stride per session; `pkts_copied_partial`; "builder learns strides" deleted from 05 §9 and 09
  §8. M9 slot-interface entry point in 05 §11. 09 §4 row `transport_linesize` → layout stride.
  *Why:* Verified: `st_tx_video_session.c:1225`, `:1270-1272`, `:1295-1298` attach at the linesize
  offset; copies only at `:1274-1288` (padding crossing) and `:1289-1292` (PA page); copy builder
  `:1120`, `:1162-1175`; `include/st20_api.h:1215-1218`; linesize validation `:3333-3339`, RX
  `st_rx_video_session.c:3343-3351`
- **§6.2 (03 side) — Accepted.** 03 §4.3: `struct mtl_tx_submission.hold` (an `mtl_lease_h`), RX
  HELD state, slot FREE when released **and** hold count 0; four TX sessions with own layouts over
  one region; `mtl_rx_transfer` is sugar; library RX pools as regions; 03 §4.2 and 07 §6 add HELD.
  *Why:* exclusive leases serialised split-forward over ≥ 4 frame periods
- **§6.2 — Accepted, modified.** 05 §5.4: `mtl_tx_submission.hold` + HELD RX state + 1 → 4
  walkthrough; 05 §5.3 `mtl_session_get_pool_region`; `mtl_rx_transfer` kept as sugar.
  **Modified:** a buffer belongs to at most one session's pool (multi-session attach removed), and
  a *forward pool* may use `MTL_COMPLETE_NONE`. *Why:* Holds replace the cross-session exclusive
  lease, so keeping multi-attach would leave two mechanisms. With holds, the TX slot is provably
  FREE before its RX slot is READY again, so NONE is safe and the forwarder needs no reap loop (C5
  §3.4 bullet 2). `rv_put_frame` is an atomic decrement (`st_rx_video_session.c:222-231`), safe
  from the TX completing context
- **§6.3 — Accepted.** 05 §2.1 "what v1 does for each producer" table (16 producers × path,
  copies, phase); `MTL_POOL_DYNAMIC` + `mtl_tx_acquire_dynamic` in 05 §5.5 (Phase 4, completion
  forced ALL); the Phase 6 escape hatch is replaced. *Why:* —
- **§6.4 — Accepted.** 05 §3.4: `max_regions` in `mtl_port_get_caps`; `regions_used/free` in
  `mtl_instance_get_mem_status`; `-MTL_ENOSPC`, reason `REGION_BUDGET`; "one region per pool
  arena" is the required pattern; EAL memory and library pools cost nothing. *Why:* Verified
  `MT_MAP_MAX_ITEMS = 256` (`lib/src/mt_main.h:62`); each `rte_extmem_register` takes a memseg
  list, `RTE_MAX_MEMSEG_LISTS = 128` shared with EAL (DPDK
  `lib/eal/common/malloc_heap.c:1188-1197`); MXL POC `mxl_bridge.c:213-218`
- **§6.5 — Accepted.** 05 §3.3: `va`/`length` aligned to the backing's page size, else
  `-MTL_EINVAL`, reason `UNALIGNED`; never rounds outward; plane offsets byte-granular
  (`stride_align`/`offset_align` = 1 in requirements); page sizes in caps and `mtl_mem_info`.
  *Why:* `mt_main.c:830-838` already rejects
- **§6.6 — Accepted.** 05 §6.3: `internal_buffer_count`/`internal_bytes` in requirements and
  `get_info`; `mtl_instance_get_mem_status` struct per NUMA node; CONVERT + `REQUIRE_DIRECT` →
  `-MTL_ENOTSUP`, reason `CONVERT_NOT_DIRECT`, at query and create (05 §6.1). *Why:*
  `st20_pipeline_tx.c:455`, `st20_pipeline_rx.c:564` pass the count to the transport
- **§6.7 — Accepted, modified.** 05 §4.5: **MTL never writes a TX buffer**, so the view stays
  readable after submit until the buffer is acquired again. `mtl_buffer_hold/unhold` (DP) excludes
  a buffer from any-free acquire (needed in NONE). `blocked_on = APP_HOLDS`. RX strict rule kept;
  monitor-plus-forward keeps its RX lease while TX holds. *Why:* Stronger and simpler than "valid
  until reaped", and it does not conflict with the 4.11 out-of-order slot freeing (): no free-list
  change needed
- **§6.8 — Accepted.** 05 §5.1: `N > H + 1 + ceil((rx_flush_offset + reorder_skew)/period)`;
  `RECLAIM_OLDEST_READY` + `BY_INDEX` reclaims only the target slot if READY. *Why:* —
- **§6.9 — Accepted.** 05 §4.3: unit = field, `rows = height/2` (PsF: `height`); a woven frame is
  two field buffers over one region, `stride = 2 × L`, second offset `+ L`: DIRECT. *Why:*
  `st_fmt.c:611` halves interlaced frames
- **§6.10 — Accepted.** access enforced at attach (header comment on `mtl_session_attach_buffers`,
  `MTL_REASON_ACCESS_MISMATCH`); ex04 imports `MTL_MEM_READ` for TX; the forwarder uses the
  READ\|WRITE pool region and 10 §11 says shared imported regions use `access = 0`. *Why:* Part A
  6.10
- **§6.10 — Accepted.** 05 §3.6: access enforced at attach (RX needs WRITE, TX needs READ),
  `-MTL_EINVAL`, reason `ACCESS`; the IOMMU map is always RW in v1; the 10 §10 example fix is
  requested from the. *Why:* DPDK maps read-write (`lib/eal/linux/eal_vfio.c:1442-1443`); a
  `PROT_READ` mapping is therefore COPY-only (**[inferred]** kernel write-pin)
- **§6.11 — Accepted.** 05 §3.5: `device_mask = 0` = lazy (import pins and registers, maps
  nothing; each attach maps the missing devices as a CP step); `mtl_mem_map_device(region, port)`;
  `mtl_mem_info.mapped_devices`; a later-opened port is mapped at its first attach. *Why:* DMA set
  decided at session create, `st_rx_video_session.c:2572-2575`
- **§6.12 — Accepted.** ex04: stop FLUSH → reap → destroy → wait `SESSION_RETIRED` on an app-owned
  EQ subscribed with `MTL_EQ_SUB_SESSION` → buffers → region → only then free the arena;
  `SESSION_RETIRED` posted in both cases; `MTL_DESTROY_FORCE` escalates (10 §6, 11 §6.2). *Why:*
  Part A 6.12
- **§6.12 — Accepted.** 03 §6.4 (wait for `SESSION_RETIRED` or FORCE; imported memory must not be
  freed before `mtl_mem_destroy` returns 0; pointer to 05 §3.3 / 11 §5). *Why:* the revision-2 §6
  comment would get `-EBUSY` on every buffer
- **§6.12 — Accepted.** 05 §3.7: four-step teardown order (stop → destroy → wait `SESSION_RETIRED`
  or `MTL_DESTROY_FORCE` → buffer/mem destroy → only then unmap); GstBufferPool rule. *Why:* —
- **§6.13 — Accepted.** 05 §5.6 two-phase recipe (negotiate `min_buffers ≥ min_count_direct + 1`
  and `GST_VIDEO_META`; per-buffer decision export pool / DYNAMIC import / copy fallback); early
  `SOURCE_RELEASED` for COPY/CONVERT in Phase 4 (05 §7 item 2). *Why:*
  `gst_mtl_st20p_tx.c:547-597` holds refs until `notify_frame_done` today
- **§6.14 b1 (`min_count_direct`) — Accepted.** 05 §4.1 table: formula `1 + ceil(nb_tx_desc /
  packets_per_unit)`, 2 for common formats, 3 under 512 packets, **0 = n.a.** for
  cvideo/audio/ANC/fastmeta and every RX; `completion_latency_ns` published next to it (model
  values marked as spike S0 outputs). *Why:* `MT_DEV_TX_DESC` `lib/src/dev/mt_dev.h:12`, override
  `mt_dev.c:1009`; heuristic `st_tx_video_session.c:2873-2895`
- **§6.14 b2 (`ST20_FB_MAX_COUNT`) — Accepted.** 05 §5.2: engine change **E11** (Phase 2) makes
  the frame arrays dynamic; until then `max_count = 8` is reported and a larger count fails at
  query/create with `-MTL_ERANGE`, reason `POOL_COUNT_MAX`. Answers Q-MEM-5. *Why:* C5's TX
  citation `:4002` is wrong at `545a266a`: the check is `st_tx_video_session.c:4073` (RX
  `st_rx_video_session.c:4266` is right; ST22 `:4189`/`:4403`)
- **§6.14 b3 (meta area) — Accepted, modified.** 05 §4.4: user meta ≤ 1332 B (1320 B when tagged),
  reported as `max_user_meta_bytes`; attached buffers need no meta region; ANC RX table sized by
  `max_packets` (0 = 255), 20 over the legacy engine until **E12**; `user_meta_type` (u32) +
  `user_meta_version`. **Modified:** the 12-byte wire tag is sent only for a non-zero type, so the
  zero default is wire-compatible with legacy. *Why:* `st_tx_video_session.c:271-272`,
  `include/mtl_api.h:89`, `mt_main.c:550`; `ST40_MAX_META` `include/st40_api.h:308`; user meta is
  its own RTP packet (`st_tx_video_session.c:4337-4396`)
- **§6.14 b4 (`path` + counters) — Accepted.** 05 §6.2: `path` = granted session path; per-unit
  `pkts_dma`, `pkts_copied_partial`; NONE mode aggregates them in stats. *Why:* RX per-packet DMA
  decision `st_rx_video_session.c:1804-1826`
- **§6.14 b5 (GPU pinned host memory) — Accepted.** 05 §3.2 row, §10: supported host import, with
  a G-test using `mmap` with `MAP_ANONYMOUS` and `MAP_LOCKED`; FFmpeg GPU_DIRECT flag deferred to
  the device domain (05 §10, 09 §8.1). *Why:* `mtl_st20p_rx.c:204-220`
- **§6.14 b6 (regular files) — Accepted.** 05 §3.2: accepted as COPY-only, detected via
  `/proc/self/maps` + `statfs` (`HUGETLBFS_MAGIC` → DIRECT, `TMPFS_MAGIC` → DIRECT, other → FILE),
  reported in `mtl_mem_info.backing`. C5's example is imprecise: the ext-frame sample mmaps its
  file (`:64`) only to copy it into `mtl_dma_mem_alloc` memory (`:80-99`). *Why:* —
- **§6.14 b7 (NUMA) — Accepted.** 05 §3.6: `numa_mismatch` bitmask in `mtl_mem_info`,
  `pool_numa_mismatch` in `get_info`; `MTL_MEM_NUMA_REQUIRED` fails import/attach with reason
  `NUMA_MISMATCH`. *Why:* —
- **§6.14 b8 (citation nits) — Accepted, partly rebutted.** 05 §4 now cites `:1279` (the `buf_len`
  check) and `:1285-1286` (unchecked addr/iova). **Rebutted:** `mt_main.c:864-865` is still
  correct; the working tree equals `545a266a` for `lib/`, so the claimed shift to `:869-870` does
  not exist. *Why:* `git show 545a266a:lib/src/mt_main.c`, lines 864-865

### §7 Framework plugins

- **§7.1 — Accepted.** 03 §1, §6.1 (guarantee 6), §6.3, §7.1: refcount-only release, instance
  outlives DESTROYING sessions, process exit supported; 07 §3.4: `SESSION_RETIRED` to the private
  EQ and every `MTL_EQ_SUB_SESSION` subscriber incl. the instance EQ;
  `get_eq`/`eq_read`/`get_wait_object` legal in DESTROYING; RETIRED tombstone getter (03 §2.2,
  §3.1). *Why:* A11 and Part A 7.1
- **§7.2 — Accepted.** 03 §4.4 exported-pool rule table; `MTL_SESSION_EXPORT_POOL` forces ALL and
  implies MT_SUBMIT; 07 §2.2. *Why:* the 11 §5 recipe is
- **§7.3 — Accepted.** 04 §4.2 contract table (acquire/release MP-safe; submit/publish one context
  or MT_SUBMIT, required for exported pools; readers under the reaper lock or SINGLE_READER).
  *Why:* A10
- **§7.4 (state row) — Accepted.** 03 §3.2 `mtl_session_reconfigure` row (CREATED/STOPPED) and
  §3.6 (reconfigure to move `flow.port`). *Why:* semantics of the pool re-derivation are (05 §5)
- **§7.4 — Accepted, modified.** 09 §7.3: `mtl_session_reconfigure(s, const struct
  mtl_reconfigure_params*)` in CREATED/STOPPED keeps identity (handle, name, SSRC, stats,
  timeline, group, CQ/EQ, RX rules and joins) and re-derives requirements. Library pools are
  re-created; attached pools are re-validated (`POOL_INCOMPATIBLE`). **Modified:** a params struct
  `{pool_count, media_config, legs}` instead of `(s, const void*)`, because 8.1 routes `flow.port`
  changes through reconfigure and `pool.count` must change in STOPPED. *Why:* Today's sinks refuse
  a second CAPS (`gst_mtl_st20p_tx.c:452-455`, `gst_mtl_st30p_tx.c:457-461`; C5's `:133-137`
  points at prototypes)
- **§7.5 — Accepted, modified.** 07 §3.4: subscription masks (`MTL_EQ_SUB_*` bits PORT, TIME,
  INSTANCE, SESSION, EPOCH_TICK, USER), `mtl_eq_create(cfg.mask)`, `mtl_eq_subscribe(eq, obj,
  mask)`, own copy per EQ, shared EQ Phase 1, getters for timing warning and retirement (07 §3.2),
  `OVERFLOW.producer_class`, USER ring. *Why:* tasklet-class events fan out at *read* time (each
  EQ keeps a cursor over per-source pending state), so tasklet cost is independent of subscribers;
  library-class events fan out at post time (deviation D6)
- **§7.6 — Accepted.** 03 §3.3: create/start never wait for ARP/IGMP; `WAITING_NEIGHBOUR` leg flow
  state; units `DROPPED/NO_NEIGHBOUR` per leg (`leg_reason`), DROPPED overall when every leg is
  unresolved; 07 §3.2 `FLOW_STATE` + getter; `MTL_FLOW_USER_MAC` bypass; IGMP report counter.
  *Why:* verified ARP at create (`st_tx_video_session.c:926`) and 500 ms sleeps
  (`mt_arp.c:171-199`)
- **§7.7 — Accepted, modified.** 09 §1.4 fields: `rx_unit_samples` (0 = `floor(10 ms·Fs/S)·S`,
  matching today's `st30_calculate_framebuff_size`, `st_fmt.c:1264-1281`), `buffer_capacity_bytes`
  (audio 10 ms, fastmeta 64 KiB), ANC `max_udw_bytes` (64 KiB = 255 × 255) + `max_packets` (255);
  granted values in `mtl_session_info`. **Modified:** ANC uses `max_udw_bytes` as its capacity (no
  separate `buffer_capacity_bytes`); the fastmeta per-item limit of 2044 B
  (`include/st41_api.h:94`) is stated. *Why:* plugins size at 10 ms: `gst_mtl_st30p_rx.c:228-230`,
  `mtl_st30p_rx.c:126-127`
- **§7.8 (merge-table part) — Accepted.** 03 §7.2: `lcores` ignored-on-second (reported), queue
  counts mergeable at first open, migrate flags ignored. *Why:* C5 cites `mtl-output.c:331-333`
  for per-source `lcores`/queues; they are at `mtl-input.c:330-332` and `mtl-output.c:139-141`
- **§7.8 — Accepted, modified.** 09 §8.2 OBS input and output recipes, and the merge-table route
  for lcores and queue counts. **Modified:** the MEDIA_CLK misuse is *latent*, not wrong on the
  wire: OBS sets neither USER_PACING nor USER_TIMESTAMP (`mtl-output.c:154-168`), so st20p
  discards the timestamp (`st20_pipeline_tx.c:231-234`) and output is effectively AUTO. *Why:*
  `mtl-output.c:224-225`; `mtl-output.c:134-141`, `mtl-input.c:325-332`
- **§7.9 auto-detect — Accepted.** `mtl_anc_config.detect = MTL_DETECT_OFF` (11 §6.3).
- **§7.9 ST40 test knobs — Accepted.** `MTL_FAULT_TX_MUTATE` + `enum mtl_tx_mutation` in
  `mtl_debug.h` (patterns of `include/st40_api.h:90-96`). *Why:* debug API, not lost
- **§7.9 `pts-pacing-offset` — Accepted.** `timing.media_time_offset_ns`.
- **§7.9 ST22 `pack_type` — Accepted.** `mtl_cvideo_config.pack_type`. *Why:* listed and kept
- **§7.9 CLOCK_TAI without ptp4l — Accepted.** `MTL_TIME_SOURCE_SYSTEM_TAI` labelled ESTIMATED
  with a timing warning (reason `TIME_ESTIMATED`); behaviour change noted (11 §6.2).
- **§7.9 instance flags — Accepted.** `MTL_INSTANCE_*` flags via the merge table (11 §6.2, §6.3).
  *Why:* A11
- **§7.9 `AVFMT_FLAG_NONBLOCK` — Accepted.** scoped to the demuxer; `write_packet` uses a timeout
  (11 §6.2).
- **§7.9 st30p muxer — Accepted.** pts rescale + `MTL_SUBMIT_DISCONTINUITY` (11 §6.2, §6.3).
- **§7.9 b1 (ST40 DISABLE_AUTO_DETECT) — Accepted.** 09 §8.1 → `mtl_anc_config.detect =
  MTL_DETECT_OFF`; 09 §1.4 `detect`. *Why:* `gst_mtl_st40p_rx.c:515`
- **§7.9 b2 (ST40 test knobs) — Accepted.** debug API `MTL_FAULT_TX_MUTATE` (header request).
  *Why:* `gst_mtl_st40p_tx_test.h`, `include/st40_api.h:103-108`
- **§7.9 b3 (`pts-pacing-offset`) — Accepted.** → `timing.media_time_offset_ns` (TAI, ns). *Why:*
  `gst_mtl_st20p_tx.c:210-217`
- **§7.9 b4 (ST22 `pack_type`) — Accepted.** listed: `mtl_cvideo_config.pack_type` CODESTREAM
  only. *Why:* `mtl_st22p_tx.c:81`
- **§7.9 b5 (FFmpeg CLOCK_TAI) — Accepted.** SYSTEM_TAI accepted, labelled ESTIMATED,
  `timing_warning = TAI_OFFSET_UNSET` when the kernel TAI offset is 0; behaviour change noted.
  *Why:* `mtl_common.c:39-45`
- **§7.9 b6 (instance flags) — Accepted.** merge table (03 §7) dispositions; `ALLOW_DOWN_PORTS`
  moot. *Why:* `include/mtl_api.h:340-350`, `:497`
- **§7.9 b7 (`AVFMT_FLAG_NONBLOCK`) — Accepted.** scoped to the demuxer; `write_packet` →
  `mtl_tx_write(timeout)` (11 §5 sentence: ). *Why:* —
- **§7.9 b8 (st30p muxer pts) — Accepted.** pts rescale to 1/Fs + forward gap / DISCONTINUITY;
  demuxer pts from the unit instead of `frame_counter++`. *Why:* no `pts` use in `mtl_st30p_tx.c`;
  `mtl_st30p_rx.c:232`, `mtl_st20p_rx.c:301`
- **§7.10 — Accepted, modified.** both pairs kept: `-MTL_EAGAIN`/`-MTL_ETIMEDOUT` are documented
  as "same handling" (C2, 10 §1.4); `-MTL_EBADF` (handle) and `-MTL_ESTALE` (lease generation)
  point at different bugs; the `-MTL_ESHUTDOWN` overload is removed (A5). *Why:* C5 rates the
  per-call surface acceptable; the distinctions carry diagnostic value at no cost
- **§7.10 — Accepted.** 07 §5.1: an "application action" column; EAGAIN/ETIMEDOUT handled alike;
  EBADF/ESTALE both bugs, different lease part. *Why:* —

### §8 Lifecycle, operations, NMOS, observability

- **§8.1 / 8.2 (as commands) — Accepted.** 03 §3.2 rows `update_flows` (boundary command in
  ARMED/RUNNING, CP-applied in CREATED/STOPPED) and `set_leg_enabled` (immediate); 04 §3.2
  all-or-nothing flow command; 03 §3.5 RX `AT_TAI` activation. *Why:* flow/leg semantics and the
  link monitor are (09)
- **§8.1 — Accepted.** 09 §7.1: `mtl_session_update_flows(s, legs, n, const struct
  mtl_activation*)`, `NOW`, `AT_TAI`, `AT_MEDIA_INDEX` (TX). All-or-nothing: rules, joins, ARP
  requests, header templates and kernel-socket queues are prepared before commit. All legs switch
  on the same unit; RX `AT_TAI` accepts by RTP-derived media time, with the old rule removed
  after; `flow.port` only via reconfigure; pending updates replaceable; IS-05 mapping. *Why:* ARP
  is started, not awaited (consistent with 7.6 `WAITING_NEIGHBOUR`)
- **§8.2 — Accepted.** 09 §7.2: every leg reserved and never pruned; `mtl_session_set_leg_enabled`
  (CP boundary command, RUNNING legal); admin × oper × flow state per leg; a leg with its link
  down is skipped and its queue reset via the stalled-queue path; link monitor Phase 2 (LSC or 100
  ms poll, netlink for kernel backends) as a prerequisite of `LEG_STATE`/`PORT_LINK`; 09 §5
  constraint row rewritten. *Why:* Verified: link is checked only at start, `dev_detect_link`
  `dev/mt_dev.c:815-850`, called at `:2008-2020`, TODO at `:2018`; no LSC anywhere in `lib/src`.
  **C5 citations wrong at `545a266a`:** the prune function is `st_tx_video_session.c:4000-4045`
  (not `:3935-3975`), `mt_eth_link_dump` is `mt_util.c:409-422` (not `:391`); audio prune is at
  `st_tx_audio_session.c:2599`
- **§8.3 — Accepted.** 07 §5.1 split codes with values; 03 §3.2 ERROR/DESTROYING columns; 03 §3.4
  ERROR column (QUEUED → `FLUSHED/SESSION_ERROR`, IN_FLIGHT → `FAILED/<reason>` after references
  drop, RX RECEIVING discarded); 03 §3.6 entry actions and restart (`start` re-reserves or
  `-MTL_ENODEV`); 07 §5.4 `enum mtl_state_reason` in full (≈ 60 values, grouped by hundreds).
  *Why:* A5
- **§8.4 — Accepted.** 03 §3.5: rule + join at first start, kept across stop; ARMED discards units
  with media time < t (arrival time when media time is invalid); RUNNING from t; latest-only
  recipe (count 2, or 3 for a holding consumer). *Why:* the fallback to arrival time and the
  count-3 variant are additions (deviation D10)
- **§8.5 (instance part) — Accepted.** 03 §7.2 merge table over every `mtl_init_params` field and
  flag at `545a266a` (`include/mtl_api.h:564-761`, flags `:338-502`, port params `:542-558`);
  `mtl_port_open` moved to Phase 6 with `mtl_port_params` and its subsystem list. *Why:* two
  general rules added: zero fields match, only *set* flag bits are requests (deviation D8)
- **§8.6 — Accepted.** 07 §7: `MSG_NOSIGNAL`; create after loss → `-MTL_EAGAIN`/`MANAGER_LOST`
  unless `MTL_INSTANCE_MANAGER_OPTIONAL` (shm fallback, `mt_sch.c:732-740`); background reconnect
  with re-registration and lcore re-announce; cached `mtl_instance_get_status().manager`; no `err`
  log when unconfigured. *Why:* verified `mt_instance.c:17`, `:69`, `:255-273`; the `err` is in
  `mtl_is_manager_alive` (`:266`), while init only warns (`:201`) — C5 attributed `:201` to the
  `err`
- **§8.7 (event field) — Accepted.** 07 §3.1: `origin_name[64]` in every event (session name, port
  name, group/timeline name). *Why:* a `SESSION_RETIRED` reader cannot look up a retired handle
- **§8.7 identity, enumeration, exporter — Accepted.** 08 §7: 64 B name copied at create, unique
  per instance (`-MTL_EEXIST`), in info/stats/log prefix, events carry a name index (+
  `mtl_instance_name_of`), generated name when empty, `mtl_instance_list_sessions` in v1, exporter
  pattern; R-OPS-3; G-88.
- **§8.8 epochs worse than cumulative — Accepted.** 08 §2.3 cumulative only, `new_epoch` removed;
  windowed maxima (1 s / 60 s ring of buckets) + histograms (08 §2.6); G-42 rewritten; Q-OBS-1 =
  (a).
- **§8.9 (07 identity) — Accepted.** 07 §6: gauges from one scan; testable form is the transition
  identity. *Why:* G-43 rewrite is
- **§8.9 G-43 tautology — Accepted.** 08 §2.4 gauges by single-pass scan; per-writer
  `entries/exits` transition counters; G-43 = transition identity, exact at quiescent points on
  the null backend, bounded otherwise.
- **§8.10 fault matrix not injectable — Accepted.** 13 §6 matrix: every row mapped to a
  `mtl_debug_inject` point and an I-tier step; new `nicctl.sh vf_link` / `vf_reset` (today's
  subcommands verified at `script/nicctl.sh:146`); FORCE_ERROR, device-gone, DROP_PKTS rows; G-49
  reaches ERROR.
- **§8.11 capacity / admission — Accepted.** 08 §8 `struct mtl_port_capacity` +
  `mtl_port_get_capacity`, `MTL_QUERY_CHECK_CAPACITY` → `-MTL_ENOSPC` with `CAPACITY_*` reason;
  reservation object Phase 6; R-OPS-4; G-89. *Why:* limits re-cited correctly: `mt_sch.c:378`,
  `:1171` ("no free sch"), `mt_sch_add_quota` `:1060-1080` (C5 had `:343`, `:1025-1046`)
- **§8.12 bullet 1 — Accepted.** 04 §5.4: instance-wide sticky flag (no per-session writes, so no
  race with destroy), `mtl_instance_uninterrupt_all`, interrupt on DESTROYING is a no-op,
  precedence EBADF > ESHUTDOWN > EIO/ENODEV > ECANCELED. *Why:* —
- **§8.12 b2 log2 histograms, RX parser summary — Accepted.** 08 §2.6 linear layout via
  `hist.<name>.linear`; 08 §2.7 RX timing-parser summary in v1, opt-in.
- **§8.12 b3 test isolation — Accepted.** 13 §6.1: U never inits/tears down EAL per case (one
  `--no-huge --no-pci` init, as `tests/unit/common/ut_common.c:25-31`), one long-lived null
  instance; G-49/G-57 in one long-lived instance; NoCtx stays per process.
- **§8.12 b4 recovery coalescing — Accepted.** 08 §2.2 footnote:
  `recoveries_ok`/`recoveries_failed` are the counts of record.

### §9 Cross-cutting themes

- **§9 `-ESHUTDOWN` overloaded — Accepted (A5).** 01 R-CMP-6; 13 G-30, G-70; 00 point 3.
- **§9 one handle for base and lease — Accepted (A4).** 01 R-OBJ-5; 02 §3 lease row; 15 §5.
- **§9 sketch does not compile; `mtl_session_info` undefined — Accepted.** 08 §3.1 defines the
  `mtl_session_info` field list; 13 G-74 (`sketch/check.sh` in CI from Phase 0, 14 §1.3); R-ABI-5.
- **§9 instance boundary untouched — Accepted (A11).** 14 §1.3 Phase 0 instance params + libmtl
  soname; R-ABI-4; G-77; 15 §8.1.
- **§9 single-process timelines / default instance — Accepted.** R-TIME-13; 15 §8.1 (epoch
  timeline + index helper; merge table). *Why:* 06 owns the helper
- **§9 interlaced and sub-rectangle strides — Accepted.** G-84; R-MEM-5. *Why:* 05 owns the rule
- **§9 ST22 CBR-with-drop — Accepted.** G-66 rewritten (VBR_MAX default, CBR opt-in, `-MTL_ENOSPC`
  at submit); 14 §2 E10.
- **§9 RX neglected — Accepted.** R-TIME-12; 02 §2.2 RX slot entry points incl. `force_complete`;
  G-76, G-82; Phase 1 RX deadline hook (14 §3.1). *Why:* 06/04 own the model and hooks
- **§9 private EQ + single-reader instance EQ — Accepted.** shared EQ with subscription mask moved
  to Phase 1 (14 §3.1); R-OBJ-6; G-77. *Why:* 07 owns the EQ
- **§9 no NIC-less backend / time / faults — Accepted.** as §3.3 and §8.10 rows.
- **§9 framework recipes point at INDEX — Accepted.** 00 point 5; 13 G-78, G-85. *Why:* 11 owns
  the recipes
- **§9 defaults load-bearing — Accepted.** G-73 (defaults table CI check); R-ABI-5. *Why:* 09 owns
  the table

### §10 What the design gets right

- **§10 what the design gets right — Accepted (kept).** the model, reader-side results, generation
  handles, region/lease separation, RX lending, dry-run query and the research method are
  unchanged.

### §11 The ten changes that move the plan furthest

- **§11.1 fix the header first — Accepted.** G-74 in CI from Phase 0; 14 §1.3. *Why:* owns
  `sketch/`
- **§11.2 RX chapter — Accepted.** as §9 RX row.
- **§11.3 re-sequence — Accepted.** 14 §0–§2: Phase 0.5, engines first, slot interface built once.
- **§11.4 grid arithmetic — Accepted, modified.** G-68 (T0 on the grid for every k, first-field
  aligned); G-22 rewritten; G-94 preroll. *Why:* see see below on "+750/+751"
- **§11.5 framework recipes — Accepted.** G-78 export pool, G-85 CAPTURE NEAREST, G-95 waiters per
  target. *Why:* 11 owns the text
- **§11.6 stride-aware DIRECT, countable leases, read-only TX views — Accepted.** G-83, G-84;
  Phase 4 scope. *Why:* 05 owns the rules
- **§11.7 split the error vocabulary — Accepted.** G-30, G-49 (ERROR row), G-57, G-70.
- **§11.8 operator surface — Accepted.** 08 §7–§9; R-OPS-1…7; G-75, G-87…G-91; link monitor in
  Phase 2 (14 §3.2).
- **§11.9 test substrate first — Accepted.** 13 §9; Phase 0 design, Phase 1 code; §8 budgets.
- **§11.10 shrink decisions, validate with users — Accepted.** 00 ten decisions; Phase 0 exit
  review; 01 §7 / 14 §5.

## Part C — Where the answers went

| Artefact | What changed in revision 3 |
|---|---|
| [sketch/](../../sketch/README.md) | the normative header set (`mtl_unified.h`, `mtl_simple.h`, `mtl_debug.h`), twelve C99 examples and a C++17 twin; `check.sh` compiles them with gcc and clang, C99 and C++17, `-Wpadded -Werror`, and prints the surface: 217 exported functions |
| [00](../00-summary.md)–[15](../15-security-and-deployment.md) | every document at revision 3; new [15 security and deployment](../15-security-and-deployment.md) |
| [OPEN-QUESTIONS.md](../OPEN-QUESTIONS.md) | ten maintainer decisions (M1–M10) at the top; every other question answered as a proposed default; three new questions (Q-TIME-26, Q-TIME-27, Q-PLAN-1) |
| [DECISIONS.md](../DECISIONS.md) | 29 new entries (D-42…D-70), 20 revised, 3 superseded; each is "Awaiting M#" or "Proposed default" |
| [side-findings.md](../side-findings.md) | a Status column against open PR #1770, and new findings SF-40…SF-55 from this review, among them the MtlManager socket's trust model (SF-47) and a likely overflow in the GStreamer st20p sink's copy path |
| [13](../13-guarantees-and-tests.md) | G-68…G-99 added; G-08, G-22, G-42, G-43, G-66 rewritten; every guarantee marked P or BE; numeric performance budgets |
