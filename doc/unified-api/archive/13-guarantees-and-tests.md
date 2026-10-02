# 13 — Normative guarantees and their contract tests

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Basis | review §14's 30 guarantees `[R00 §14]`, extended with threading, observability and timing guarantees from research notes 03, 05, 06, 12 and reviews C1–C5 |
| Rule | a behaviour that has no test is not promised (01 §6); every guarantee is marked **P** or **BE** (r3) |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

Items changed in revision 3 are marked **r3**. IDs are stable: a guarantee that no longer
applies is retired in place, never renumbered.

## Tiers and evidence

| Tier | Where | Needs |
|---|---|---|
| **B** build / lint | CI | headers, `sketch/check.sh`, `readelf`, `nm` |
| **U** unit | `tests/unit/unified/` | nothing: L2 core against a scripted fake L1, and **r3** the null backend (`null:<n>`) with the test time source, on one long-lived instance per test binary (§9) |
| **UB** unit, real engine boundary | `tests/unit/unified/` | the real engine code driven without a NIC. **r3:** UB tests drive the L2 ↔ engine slot interface ([02 §2.2](02-architecture.md)), not file-local statics; catches callback-boundary bugs like PR #1610's lost timing metadata `[R01 R7]` |
| **I** integration | `KahawaiTest`-style, real VFs | NIC, hugepages, root; **r3** `gtest.sh` fault steps (§6) |
| **A** acceptance | `tests/acceptance/` pytest | E2E, capture tooling |
| **M** measurement | lab with HW timestamps / EBU LIST | accuracy claims per NIC × pacing |

**Evidence (r3).** Review C5 found 37 of 67 guarantees resting on UB, the most brittle
tier: at `545a266a`, 24 unit-test files `#include` production `.c` files and 60 commits
touched them in 2026 (C5 counted 20 and 43; the point stands) `[C5 §1.8]`. Two changes
answer it:

- guarantees that are properties of L2 alone move to **U over the null backend**
  (G-04, G-30, G-49, G-57, G-64, most new ones), which needs no engine internals;
- guarantees that are properties of the engine boundary stay **UB**, because that is the
  boundary where PR #1610 failed, but UB now targets the slot interface, a stable internal
  API, so harness churn follows interface changes rather than every refactor.

The **Ev.** column says how firm each promise is:

- **P** — promised. B/U/UB evidence exists, or the I-tier evidence exists or is funded in
  the phase named in [14](14-implementation-roadmap.md). A red P test blocks its phase
  exit;
- **BE** — best effort until the named evidence exists. A BE guarantee never blocks a
  phase exit, and the doc says what would make it P.

## 0. Requirement → guarantee map

Every MUST and SHOULD in [01 §5](01-goals-and-requirements.md) has at least one guarantee,
except R-LIFE-5, deferred with Q-LIFE-2 (the six MUST gaps found by review C4 are closed
by G-45…G-51 `[C4 §2]`; the r3 requirements are closed by G-68…G-99, and G-100…G-106 add
coverage):

| Requirement | Guarantees | Requirement | Guarantees |
|---|---|---|---|
| R-OBJ-1 | G-45 | R-CMP-1 | G-01, G-02 |
| R-OBJ-2 | G-46 | R-CMP-2 | G-04, G-05 |
| R-OBJ-3 | G-12, G-48 | R-CMP-3 | G-28, G-57 |
| R-OBJ-4 | G-47 | R-CMP-4 | G-06, G-100 |
| R-OBJ-5 | G-07, G-29, G-71 | R-CMP-5 | G-41, G-58 |
| R-OBJ-6 | G-52, G-95 | R-CMP-6 | G-30, G-38, G-57, G-70 |
| R-MEM-1 | G-15 | R-CMP-7 | G-52, G-69 |
| R-MEM-2, R-MEM-3 | G-10, G-11, G-96, G-101, G-102 | R-OBS-1 | G-40, G-42 |
| R-MEM-4 | G-16 | R-OBS-2, R-OBS-4 | G-43, G-55 |
| R-MEM-5 | G-13, G-14, G-84 | R-OBS-3 | G-41, G-54, G-91 |
| R-MEM-6 | G-35, G-36, G-103 | R-OBS-5 | G-44, G-88 |
| R-MEM-7 | G-15 | R-THR-1 | G-38, G-79 |
| R-MEM-8 | G-56, G-60, G-83 | R-THR-2 | G-39 |
| R-TIME-1 | G-17 | R-THR-3 | G-50 |
| R-TIME-2 | G-18, G-59 | R-THR-4 | G-14, G-39 |
| R-TIME-3 | G-19, G-21, G-22 | R-THR-5 | G-39, §8 waker budgets |
| R-TIME-4 | G-20 | R-LIFE-1 | G-35, G-49, G-80 |
| R-TIME-5 | G-26, G-28, G-61, G-66, G-85, G-86 | R-LIFE-2 | G-31 |
| R-TIME-6 | G-54, G-62 | R-LIFE-3 | G-30, G-64 |
| R-TIME-7 | G-23, G-24, G-63, G-68, G-94, G-104 | R-LIFE-4 | G-29, G-37, G-65, G-77, G-81 |
| R-TIME-8 | G-21 | R-LIFE-5 | deferred (Q-LIFE-2) |
| R-TIME-9 | G-53 | R-CAP-1 | G-13, G-14, G-34 |
| R-TIME-10 | G-25, G-67, G-82 | R-ABI-1, R-ABI-2 | G-33, G-51, G-72 |
| R-TIME-12, R-TIME-13 | G-76, G-105 | R-ABI-3 | G-51 (experimental DSO check) |
| R-OPS-1 | G-75 | R-ABI-4 | G-77 (merge table part) |
| R-OPS-2 | G-91 | R-ABI-5 | G-73, G-74 |
| R-OPS-3 | G-88 | R-TEST-1, R-TEST-2 | G-92 |
| R-OPS-4 | G-89 | R-TEST-3, R-TEST-4 | G-93, §6 |
| R-OPS-5 | G-87 | R-USE-1, R-USE-2 | G-97, G-72 |
| R-OPS-6 | G-98 | R-MIG-1 | G-99, G-106 |
| R-OPS-7 | G-90 | R-PERF-1 | §8 |
| | | R-SEC-1 | G-93, G-96, [15](15-security-and-deployment.md) |

The export-pool lifecycle (G-78) serves R-CMP-1 and R-MEM-8 for framework pools.

## 1. Ownership and progress

| ID | Guarantee | Tier | Ev. | How |
|---|---|---|---|---|
| G-01 | Every accepted TX submission has exactly one terminal outcome: a result, or a counter increment when the completion mode suppresses it | U, UB, I | P | count outcomes vs accepted over randomised submit/release/withdraw/discard/stop/destroy sequences (r3 names); include fault injection (§6) |
| G-02 | A rejected call leaves ownership with the caller and produces no outcome | U | P | submit with each invalid argument class; lease stays APP_WRITABLE |
| G-03 | An accepted unit progresses without any further API call — including the last unit before the app goes idle (idle descriptor cleanup) | UB | P | submit one unit, never call anything else, observe the result; PR #1610's backlog stalled `[R01 R5]`; chain-mode results today never arrive while idle `[C1 #2]` |
| G-04 | Results cannot be lost: with the CQ never read, acquire eventually reports `blocked_on = RESULTS`; then every result is readable | U | P | null backend |
| G-05 | A pool slot is not re-acquirable before its outcome is recorded | U, UB | P | race acquire against completion in a stress loop; against the real st20p (which stores FREE before calling back today `[C1 #1]`) |
| G-06 | "Reusable" means no reader or writer remains | UB, I | P | poison the buffer right after the result; capture on the wire shows no poisoned packet; DMA engine drained |
| G-07 | Foreign, stale, duplicate and wrong-session handles/leases fail without corrupting valid state. **r3:** a lease of another session fails with `-MTL_EBADF`, deterministically, on its session index (A4 encoding); an old lease of the right session fails with `-MTL_ESTALE`; neither changes any state | U | P | fuzz handle values; double submit/release; release on another session |
| G-08 | **r3.** Transmit order is **submit** order — the order of `mtl_tx_submit` calls — never slot order and never acquire order | UB | P | note G-08 |
| G-09 | Results for a session are published in submission order, whatever context completed them. **r3:** a unit that completes before an older in-flight unit frees its slot at completion, without waiting for the older unit; its result waits for its predecessors; every result carries the `seq` assigned at submit | UB | P | completions forced from another thread and out of order |

Notes for the rows above (the test method; "Also:" marks further clauses of the guarantee):

- **G-08:** Acquire A, B; submit B then A before pick-up → B transmits first. Today st20p
  numbers frames at `get_frame` (`st20_pipeline_tx.c:806-808`) and the builder picks the
  lowest number (`tx_st20p_newest_available`, `:62-77`, SF-44), so it would send A first; the
  slot interface assigns `seq` at submit `[C5 §4.11]`. Also the A B D C trace of `[R01 R6]`

## 2. Memory

| ID | Guarantee | Tier | Ev. | How |
|---|---|---|---|---|
| G-10 | A region outlives every buffer, conversion, packet and DMA reference | UB, I | P | destroy attempts at every stage of a unit's life |
| G-11 | `mtl_mem_destroy` returns `-MTL_EBUSY` while referenced | U | P | |
| G-12 | No access outside declared plane spans or published rows | UB | P | guard pages around spans (`mprotect`); progressive publish with unpublished rows poisoned |
| G-13 | `REQUIRE_DIRECT` never silently copies or converts | UB, I | P | force every downgrade condition (no multi-seg, pool below `min_count_direct`, ST22, PA mode, CONVERT) → query/create/start fails with a reason (CONVERT + REQUIRE_DIRECT → `-MTL_ENOTSUP`, reason `DIRECT_IMPOSSIBLE`, at query and at create alike) |
| G-14 | The selected data path is queryable and matches what happens | UB | P | compare `get_info` path with the per-unit `pkts_dma` / `pkts_copied_partial` counters |
| G-15 | Library-pool and imported buffers obey the same timing and outcome accounting | UB | P | identical submissions through both, diff results (review §11) |
| G-16 | Region import maps every device on the session's path, or fails | I | BE | 2022-7 session on two ports + DMA engine; IOMMU fault monitor (DMAR/AMD-Vi lines in `dmesg`). P once the Phase 4 I-tier job scans for IOMMU faults |
| G-60 | `acquire_buffer` leases exactly the named attached buffer; `BY_INDEX` RX places unit *k* in slot *k mod N* | U | P | |
| G-83 | **r3.** An RX lease held by N TX submissions (`mtl_tx_submission.hold`) returns to FREE exactly once, when its hold count reaches 0 **and** the app has released it, in any order; MTL never writes a slot that is HELD or APP_READING | U, UB | P | note G-83 |
| G-84 | **r3.** A layout with `stride ≥ row_bytes` (sub-rectangle, interleaved field) over a DIRECT-capable region is DIRECT for ST20 TX and RX: the stride reaches the engine as `ops.linesize` and no whole-unit copy happens | UB | P | note G-84 |
| G-96 | **r3.** An import whose `va` or `length` is not page-aligned (hugepage-aligned for hugetlbfs) fails with `-MTL_EINVAL`, reason `UNALIGNED`, and maps nothing; an aligned import maps exactly `[va, va+length)` | U, I | P | note G-96 |
| G-100 | **r3.** MTL never writes a TX buffer: after `mtl_tx_submit` its bytes are unchanged and it stays mapped until it is acquired again; a buffer held with `mtl_buffer_hold` is never returned by an any-free `mtl_tx_acquire` | U, UB | P | hash the buffer before submit and after the result on the DIRECT, COPY and CONVERT paths `[C5 §6.7]` |
| G-101 | **r3.** Attach fails with `-MTL_EINVAL`, reason `ACCESS_MISMATCH`, when an RX session gets a region without `MTL_MEM_WRITE` or a TX session one without `MTL_MEM_READ` | U | P | every direction × access combination `[C5 §6.10]` |
| G-102 | **r3.** An import beyond the region budget fails with `-MTL_ENOSPC`, reason `REGION_BUDGET`, never with an opaque DPDK error; `regions_used` and `regions_free` in `mtl_instance_get_mem_status(mt, numa, &st)` are exact | UB | P | import until the budget is exhausted, destroy one region, import again `[C5 §6.4]` |
| G-103 | **r3.** A `pool.count` above `mtl_buffer_requirements.max_count` (8 for video and cvideo until E11) fails at query and at create with `-MTL_ERANGE`, reason `POOL_COUNT_MAX`, and never at start | U | P | `max_count` and `max_count + 1` per essence `[C5 §6.14]` |

Notes for the rows above (the test method; "Also:" marks further clauses of the guarantee):

- **G-83:** One RX unit → four TX sessions with attached buffers over the same region;
  release the RX lease first, then complete the TX units in random order; the slot is never
  reused early and never leaks `[C5 §6.2]`. Also: the N TX units may be in flight together;
  a submission with a plane outside the held RX buffer fails with `-MTL_EINVAL`, reason
  `HOLD_MISMATCH`.
- **G-84:** Split-forward quarter frames and 2 × row_bytes field buffers with
  `REQUIRE_DIRECT`: no copy counted (`pkts_copied_partial = 0` where the engine does not need
  one) `[C5 §6.1]`. Also: only a packet that crosses row padding (non-GPM_SL packing) or a
  PA-mode page boundary is copied, each counted in `pkts_copied_partial`; attached buffers
  of one pool with different strides fail with `-MTL_EINVAL`, reason `STRIDE_MISMATCH`.
- **G-96:** U: every misalignment class; I: IOVA table of the device shows no page outside
  the range ([15 §2](15-security-and-deployment.md)) `[C5 §6.5]`. Also: an import spanning
  VMAs with different backings fails with `-MTL_EINVAL`, reason `MIXED_BACKING`.

## 3. Time and synchronisation

| ID | Guarantee | Tier | Ev. | How |
|---|---|---|---|---|
| G-17 | Every timestamp field names its clock, unit and validity; zero is a valid value when marked valid; every time in a CQ record is TAI or invalid (r3) | U | P | schema test over every output struct |
| G-18 | Requested, resolved, scheduled, enqueued and observed times are separate fields | U | P | |
| G-19 | RTP = `floor(M × R) mod 2^32` exactly, for every essence, with no cumulative drift | U, UB | P | note G-19 |
| G-20 | Default video media time is the frame epoch; ST20 and ST40 of the same frame carry equal RTP | UB | P | today they differ by +54.4…+55.7 ticks at 1080p59.94, depending on the granted VRX0 (**[inferred, computed]**, 06 §1 T2) `[R05 F7]` |
| G-21 | Audio RTP identifies each packet's first sample; submission boundaries do not change packet RTP or payload | UB | P | random submission sizes (incl. 1024, 800/801, 1601/1602) vs one large submission → identical packets |
| G-22 | **r3, rewritten.** Interlaced: every field is a unit with its own media time, `M(second field) = M(first field) + TFIELD` exactly, and its RTP is `floor(M(field) × 90000) mod 2^32` — the one rule of G-19, with no fixed field offset. Launch includes `TLINE/2` for the second field; indices count fields | UB | P | note G-22 |
| G-23 | A group starts all members at one instant or none | U, I | P | fail one member's validation → nothing starts |
| G-24 | Under DROP and bounded SEND_LATE, dropping or delaying a unit of one session does not shift later units of it or of other sessions | UB | P | inject lateness into video only; audio RTP and launch unchanged; bounded SEND_LATE never overlaps the next slot `[C2 #4]` |
| G-25 | Arrival time is never substituted for media time (RX) | UB | P | RX results with shifted arrival: `media` unchanged |
| G-26 | Invalid exact launch requests fail explicitly and never fall back to another pacing mode | UB | P | today they silently fall back (`st_tx_video_session.c:1796-1805`) `[R05 F3]` |
| G-27 | The TX launch schedule matches ST 2110-21 for the granted sender type within the advertised accuracy profile | M | BE | note G-27 |
| G-28 | A late unit produces the configured policy's outcome and a result with margins | UB | P | delay submission across the deadline for each policy and source kind; the media mode sets the default policy (AUTO → RESLOT; INDEX/TAI → DROP) |
| G-53 | Submitting before `hint.submit_deadline_tai_ns` yields `ON_TIME` in the hinted slot. **r3:** `hint.next_media_index` is exactly the smallest index that passes the admission test, and a unit submitted before `M − min_submit_lead_ns` (`get_info`) is never `DROPPED/TOO_LATE` for pick-up reasons | U, UB | P | |
| G-59 | No packet leaves before its unit's media time; `NOT_BEFORE t` never sends a packet before t | UB, M | P (UB) / BE (M) | wide sender with maximum pre-fill `[C2 #14, #15]`; the UB part checks scheduled times, the M part the wire |
| G-61 | ST40 sessions emit the empty keep-alive packet for every frame/field without ANC; ST41 at least every 500 ms | UB | P | submit no ANC for 10 frames |
| G-62 | A clock step applies the timeline's step policy and emits `TIME_STEP`; no session drops "forever" after a forward step; **r3:** the published time base never steps except at a declared `TIME_STEP` (its frame-start error is the S7 budget of §8) | U, UB | P | `mtl_debug_inject(PTP_STEP(+37 s))` or `mtl_time_test_advance` (today's UTC→PHC switch) `[C2 #10]` |
| G-63 | A lazily anchored timeline is never resolved in the past: index 0 of an `AT_START` group is never late because of session-creation time | U | P | create sessions slowly (test clock advanced between creates) and start |
| G-66 | **r3, rewritten.** cvideo (ST 2110-22) follows `rate_mode`: `CBR` (default, as ST 2110-22 requires) pads every unit (frame, or interlaced field) to one packet count, byte count and TRS; `VBR_MAX` (opt-in, flagged non-compliant) paces at the codestream ceiling's TRS and sends only the codestream's packets. An oversize `valid_bytes` fails the submit with `-MTL_ENOSPC` | UB | P | note G-66 |
| G-67 | RX `media` honours `rx_rtp_offset`; SENDER streams mark `media` invalid; a DIRECT stream with a large offset raises `RX_TIMEBASE_SUSPECT` | UB | P | AES67 sender with a non-zero offset `[C2 #11]` |
| G-68 | **r3.** For every start index k and grid member, the resolved `T0 = ceil((now + lead + preroll − k·period) / G) · G` makes `T0 · R` an integer for every member rate R, is a whole number of frame periods for every video, ANC and grid fastmeta member (so a first-field instant for interlaced members), and puts unit k at `T0 + k·period ≥ now + lead + preroll` | U | P | note G-68 |
| G-85 | **r3.** CAPTURE with the default NEAREST snap: each submission lands in the nearest slot; a collision with an already-submitted slot gives that unit `DROPPED/DUPLICATE_SLOT`; a skipped slot follows the underrun policy; the next on-time submission is always `ON_TIME` — no permanent drop | U, UB | P | note G-85 |
| G-86 | **r3.** An audio forward gap that is not packet-aligned is padded with silence to the packet boundary; the packet grid is kept; the stream resumes at the next packet boundary ≥ the gap end; `samples_dropped` reports the skipped samples. Only an explicit DISCONTINUITY re-phases | UB | P | note G-86 |
| G-94 | **r3.** `preroll_ns` extends the lead of the T0 resolution; the horizon is measured from the resolved start instant S; if any ARMED-queued unit lies beyond `S + horizon_ns`, session or group start fails atomically with `-MTL_ERANGE`, reason `BEYOND_HORIZON`, and no member starts | U | P | note G-94 |
| G-104 | **r3.** In a group whose video member has slot delay L_v, ANC unit k carries RTP = RTP_v(k) and every packet of it lies inside the CTM/LLTM window of frame `E⁻¹(M(k)) + L_v` (06 §5.5); so does the empty keep-alive packet when video unit k is dropped | UB | P | CAPTURE group with L_v = 1 and 2; drop video units `[C5 §5.13]` |
| G-105 | **r3.** Two processes that derive k with `mtl_rational_index_at` from the same grid-aligned `t_start` on the epoch timeline emit RTP identical to one process running the same members as a group on an `AT_START` timeline resolved at that instant | U | P | both set-ups on the null backend, packet headers diffed (06 §10.7) `[C5 §5.6]` |
| G-106 | **r3.** Phase 0.5 helper = engine: for identical members, anchor and indices, `st_timeline_unit().rtp` equals the unified engine's wire RTP, and legacy `USER_TIMESTAMP` fed `legacy_ts_ns` puts exactly that RTP on the wire (06 §15) | U, UB | P | the §7 oracle against the helper (U); legacy ST20/ST30/ST40 sessions' packet headers (UB) |

Notes for the rows above (the test method; "Also:" marks further clauses of the guarantee):

- **G-19:** Exact oracle (128-bit rational) at every rate incl. 1001 families; ranges: 10^7
  units at today's TAI, **across the 2^32 wrap**¹, 44.1 kHz with 1001 rates, ST41 at 90 kHz
  with rational unit rates (r3), negative indices (audio priming), TAI past 2^61 ns `[C2
  #28]`; compare the engine's packet headers (UB). Negative indices are checked in the
  oracle at every rate, and in the engine only on EPOCH and AT_TAI timelines with M(k) ≥ S
  (r3)
- **G-22:** The engine's packet headers against the oracle, which computes `floor(M·90000)`
  per field directly, for every interlaced format. With T0 on the frame grid (G-68) the first
  field's `M·90000` is an integer, so at 1080i59.94 (TFRAME·90000 = 3003) the increments are
  +1501 (first → second field) and +1502 (second → next first); at 1080i50 +1800 each, at
  1080i60 +1500 each. C5's
  "+750/+751" is the same rule for a TFIELD·90000 of 750.75 (119.88 fields/s), which no ST
  2110-20 format uses; the rewrite matters because the rule, not a fixed offset, is normative
  `[C5 §5.3, R12 §3.1]`
- **G-27:** EBU LIST / timing parser with HW timestamps, per NIC × pacing class. **r3:** a
  **Phase 2 exit** criterion, not a Phase 1 regression gate — the lab drives RxTxApp and the
  new API has no RxTxApp path before Phase 1 ends `[C5 §1.8]`; P for each NIC × pacing class
  once measured
- **G-66:** Variable codestream sizes in both modes; an oversize codestream `[C2 #9, C5
  §5.12]`. In both modes the oversize rejection is synchronous at submit, never a silent drop.
  Also: its reason is `CODESTREAM_OVERSIZE` and the lease stays the app's; `VBR_MAX` sets
  `MTL_INFO_NON_COMPLIANT_2110_22` in `get_info`.
- **G-68:** Every rate family × randomised k; at 59.94 with k = 3, 7 and 13 the revision-2
  formula gives a half-integer `T0·90000` and this one gives integers `[C5 §5.1]`. The oracle
  runs k = 0…100 over the eight configurations of 06 §3.1. Interlaced
  members contribute TFRAME (not TFIELD) to the grid, which is what makes T0 a first-field
  instant. The same holds for `MTL_START_AT_MEDIA_INDEX` on a single session.
- **G-85:** A 60.0 Hz source into a 59.94 session for a simulated 24 h on the test clock,
  and any source rate within ±1 % of the session rate: the fraction of units sent is
  ≥ 1 − the rate error and every sent unit's RTP is on the `N·TFRAME` grid; duplicate
  indices from a framework sink `[C5 §5.8]`. No relock is needed after collisions or skips.
  LOCKED_PHASE (opt-in) relocks after 3 consecutive `OFF_GRID` with `off_grid_policy`
  RELOCK (the default; REANCHOR and DROP are the alternatives). In INDEX mode a repeated
  index gives `DROPPED/DUPLICATE_INDEX` and a smaller one `DROPPED/BEHIND`, never a
  synchronous error.
- **G-86:** Drop one video frame's audio (800.8 samples at 59.94) repeatedly; RTP of every
  later packet stays on the original grid `[C5 §5.14]`. Also: for any sequence of forward
  gaps and overlaps without DISCONTINUITY, packet p covers samples `[p·S, (p+1)·S)` since T0
  (S samples per packet), has RTP `floor(T0·Fs) + p·S`, and no submitted sample outside an
  overlap is dropped.

- **G-94:** Preroll a full pool, then start with a horizon one unit too short `[C5 §5.2]`.
  Also: queued units with M < S are `FLUSHED/BEFORE_START`.

¹ 13.26 h at 90 kHz, 27.05 h at 44.1 kHz.

## 4. Lifecycle, ABI and header

| ID | Guarantee | Tier | Ev. | How |
|---|---|---|---|---|
| G-29 | Destroy cannot race an active call or a completion into freed memory | U, UB | P | ASan/TSan stress: N threads calling DP/WT functions while another destroys |
| G-30 | **r3.** Stop, interrupt and destroy wake blocked calls immediately with the documented codes: `-MTL_ESHUTDOWN` (app-initiated stop or destroy), `-MTL_ECANCELED` (interrupt), `-MTL_EIO` (the session entered ERROR) | U | P | blocked acquire/dequeue/cq/eq waits return within a bound (today `wake_block` does not return early `[R13 §1.4]`, SF-16, fix in open PR #1770) |
| G-31 | Discard, drain and flush leave every accepted unit with a terminal outcome | U, UB | P | `mtl_session_discard_queued`, `MTL_STOP_DRAIN`, `MTL_STOP_FLUSH` (r3 names) |
| G-32 | Creation failure leaves no live session, mapping, flow, scheduler quota or handle | U, I | P | inject a failure at each create step; resource counters and `mtl_port_get_capacity` return to baseline |
| G-33 | **r3.** `struct_size` is input only and never rewritten; the library never reads or writes beyond the caller's size; it rejects 0, too-small sizes and non-zero bytes beyond the size it knows (`-MTL_EINVAL`, reason `NONZERO_TAIL`) | U | P | note G-33 |
| G-34 | Every advertised capability passes its conformance tests | I | BE | capability-driven test matrix; P once the Phase 2 capability job runs per NIC in CI |
| G-35 | A session cannot start until its pool is complete and validated | U | P | |
| G-36 | Attach, detach and memory registration never happen in the packet path. **r3:** an attach to a session on a device the region is not yet mapped to maps it before the attach returns (lazy mapping is a CP step); no map or unmap runs on the data path of a RUNNING session | UB | P | instrument: no allocation / map calls while RUNNING |
| G-37 | After destroy completes: no callback, no access to imported memory, handles invalid | UB | P | unmap the region right after `SESSION_RETIRED`; no fault |
| G-49 | Every call returns the code in the 03 §3.2 table for every state, including ERROR and **r3** RETIRED: on a retired handle `mtl_session_get_state` returns `MTL_STATE_RETIRED` until its table slot is reused, and every other call `-MTL_EBADF` | U | P | note G-49 |
| G-51 | **r3.** Unknown flag bits return `-MTL_EINVAL`. libmtl has a SONAME and a version script from Phase 0; the unified API ships as `libmtl_unified.so.0.<rev>` with every symbol in node `MTL_UNIFIED_EXPERIMENTAL` and its internal imports from libmtl in `MTL_INTERNAL`; no other symbols are exported | U, B | P | `readelf -d`, `nm -D --defined-only`, version-node listing |
| G-64 | **r3.** Interrupt is sticky until uninterrupt: a WT call started after `mtl_session_interrupt` returns `-MTL_ECANCELED` at once; `mtl_session_uninterrupt` restores normal waits; interrupting one session or queue never wakes another's waiters | U | P | note G-64 |
| G-65 | Destroy with outstanding leases defers: `mtl_tx_release`/`mtl_rx_release` still work on those leases; memory is freed only after the last returns; `SESSION_RETIRED` is posted | U | P | `[C3 P2-4]` |
| G-70 | **r3.** Each `MTL_E*` code has exactly the one meaning of the 07 §5 table; RX dequeue in CREATED, ARMED or STOPPED with nothing READY returns `-MTL_EAGAIN` (timeout 0) or `-MTL_ETIMEDOUT`, never `-MTL_ESHUTDOWN`; every failing call sets `mtl_last_error` on the calling thread with its `call_seq`, and a successful call does not modify it | U | P | note G-70 |
| G-71 | **r3.** ID 0 is the null handle of every type: every call rejects it with `-MTL_EBADF`; in a config struct it means "the documented default"; no live object ever has ID 0 | U | P | pass `MTL_<TYPE>_NULL` to every function, table-driven over every prototype of the header; `mtl_<type>_is_null` and `mtl_<type>_eq` agree with the inline twins `[C5 §2.8]` |
| G-72 | **r3.** Array readers (`mtl_tx_reap`, `mtl_cq_read`, `mtl_eq_read`) write `min(record_size, native)` bytes per record at pitch `record_size`, set each record's `hdr.size` to the bytes written, never touch a caller-set stride, and return a count ≥ 0 (0 only when `timeout == 0`; `-MTL_ETIMEDOUT` otherwise) | U | P | note G-72 |
| G-73 | **r3.** Zero-default rule: no field of any input struct has a non-zero default, and every `*_init()` writes `struct_size` and zero bytes only; the defaults table in 09 lists every field whose zero means something other than zero, and matches the header | B, U | P | note G-73 |
| G-74 | **r3.** The header set and every example of 10 compile with `gcc -std=c99 -Wall -Wextra -Wpadded -Werror` and `g++ -std=c++17 -Wall -Wextra -Werror`; every public struct passes its size-check typedef; every hole is a named `reserved` field; `MTL_CQ_ENTRY_SIZE` is 256 and every CQ record kind is ≤ 240 B; every symbol named in any document is declared | B | P | note G-74 |
| G-77 | **r3.** `mtl_instance_release` only drops a reference; the instance lives until the last DESTROYING session retires; `SESSION_RETIRED` reaches the session EQ and every EQ subscribed with `MTL_EQ_SUB_SESSION`; the session EQ stays readable in DESTROYING | U | P | note G-77 |
| G-78 | **r3.** Export pool: a framework pool built on `mtl_tx_acquire` / `mtl_tx_submit` / `mtl_tx_release` recycles every wrapper exactly once — `release_buffer` of an unsubmitted buffer releases the lease, a submitted one returns only through its `TX_RESULT`; the session forces `MTL_COMPLETE_ALL`; `set_active(FALSE)` = `stop(FLUSH)` delivers every result | U | P | note G-78 |
| G-80 | **r3.** Every command is acked: immediate commands (stop, flush/discard, detach, interrupt) within one tasklet iteration even with no unit and no packet; the CP applies a command itself for a detached session; an ack not received within the instance's ack timeout (default 100 ms) puts the session in ERROR with reason `CMD_TIMEOUT` — no call waits forever | U, UB | P | note G-80 |
| G-81 | **r3.** Destroy on a stalled queue completes: after the bounded idle-cleanup wait a worker resets the queue (dedicated) or quarantines it (shared); mbuf free callbacks then run on that worker; `SESSION_RETIRED` is posted only after; if the reset fails the port escalates to `PORT_RESET` handling | UB, I | P (UB) / BE (I) | note G-81 |
| G-87 | **r3.** `mtl_session_reconfigure` in STOPPED keeps handle, name, flows, SSRC, stats counters, timeline, group, CQ and EQ bindings; library pools are re-created; any validation failure changes nothing | U | P | note G-87 |
| G-98 | **r3.** MtlManager loss never kills or stalls the process: no SIGPIPE (manager sends use `MSG_NOSIGNAL`); running sessions continue; a create needing the manager returns `-MTL_EAGAIN` with reason `MANAGER_LOST` (r3's shm fallback flag is removed, D-92); after reconnection the instance re-registers and re-announces its lcores | U, I | P | note G-98 |

Notes for the rows above (the test method; "Also:" marks further clauses of the guarantee):

- **G-33:** Compile tests against an "old" header copy and a "new" one;
  `mtl_struct_known_size` agrees and is non-zero for every `enum mtl_struct_kind` value.
  Also: a non-zero `reserved` field of an input struct returns `-MTL_EINVAL`.
- **G-49:** Table-driven state × call matrix on the null backend; ERROR is reached with
  `mtl_debug_inject(FORCE_ERROR(reason))` (r3; revision 2 could not force it `[C5 §8.10]`)
- **G-64:** GStreamer `unlock`/`unlock_stop` sequence `[C3 P2-1, C5 §8.12]`. Also:
  `mtl_instance_uninterrupt_all` undoes `mtl_instance_interrupt_all` but leaves a session
  or queue interrupted on its own still interrupted, and interrupt on a DESTROYING session
  is a no-op (destroy wins).
- **G-70:** Per code, the triggering call and state; the P1 forgotten-`start` case `[C5 §3.2,
  §8.3]`. Also: the record holds `code`, `reason` and `call_seq`, and `detail` stays empty
  for DP failures.
- **G-72:** A caller with a larger and a smaller record size than the library's; init hoisted
  out of the loop `[C5 §2.6]`. Also: any `record_size` from the 48 B record header up is
  accepted (`-MTL_EINVAL` below it); nothing is written beyond `max × record_size`; in EQ
  records the written size is `mtl_event.size`.
- **G-73:** Header lint in CI: every field with a documented default is in the table and
  vice versa `[C5 §2.4]`; U: each `*_init()` output compared with a zeroed struct. Also: the
  lint checks both 09 §9.1 (zero replaced by another value) and §9.2 (zero-valued
  enumerators that name a mode).
- **G-74:** `sketch/check.sh` in CI from Phase 0, moving to `tests/unit/unified/` in Phase 1;
  a compile-only Windows CI job `[C5 §2.1–2.3, §2.7]`. Also: the same with `-pedantic`
  and with clang; 10 shows each example verbatim.
- **G-77:** Framework teardown order: release the instance before the last lease returns `[C5
  §7.1, §8.5]`. Also: `mtl_instance_release` never fails because objects are still live,
  and process exit without a release is supported. A second `acquire_default` with an
  invariant mismatch fails with `-MTL_EINVAL`, reason `INSTANCE_PARAM_MISMATCH`; a zero
  field and a clear flag bit always match; fields ignored on a second acquire (`lcores`
  among them, 03 §7.2) are reported in `mtl_instance_get_info` (null backend).
- **G-78:** GstBufferPool state machine replayed on the null backend, including release before
  result `[C5 §7.2]`. Also: `mtl_tx_release` on a submitted lease returns `-MTL_ESTALE` and
  changes nothing.
- **G-80:** TX waiting 1 s for launch: stop is immediate and the waiting unit is `FLUSHED`; RX
  with no packets; with `MTL_FLAG_TASKLET_SLEEP` the ack takes at most one iteration plus
  the sleep wake-up; a stalled scheduler (debug inject) `[C5 §4.2]`
- **G-81:** `mtl_debug_inject(TX_QUEUE_HANG)` then destroy; I part P once a hang can be forced
  on a VF `[C5 §4.3]`. Also: after `SESSION_RETIRED` (or `MTL_DESTROY_FORCE` returning) no
  descriptor references imported memory, and `mtl_mem_destroy` returns 0.
- **G-87:** 1080p → 720p → 1080p; counters keep counting `[C5 §7.4]`. Failures: attached
  pools that no longer fit (`-MTL_EINVAL`, reason `RECONFIGURE_INCOMPATIBLE`) and a unit
  period that no longer fits the group grid (`GRID_MISMATCH`).
- **G-98:** `mtl_debug_inject(MANAGER_LOST)`; I: `gtest.sh` kills and restarts MtlManager (§6)
  `[C5 §8.6]`

## 5. Threading, API surface, observability and operations

| ID | Guarantee | Tier | Ev. | How |
|---|---|---|---|---|
| G-38 | No public function executes on a library busy-loop thread; from one, every call outside the inline-safe DP subset and the AS calls returns `-MTL_EDEADLK` (r3) | UB | P | call each from a user tasklet, the RX packet lcore and the TAP lcore |
| G-39 | On the DPDK PMD backend the tasklet side of every hand-off makes no syscall, takes no lock an app thread can hold, and never allocates²; **r3:** measured per thread mode, with the W2 wake-up as the one syscall exception (note G-39) | UB | P | note G-39 |
| G-40 | Reading stats never blocks or delays the tasklet | UB | P | the reader takes no lock the tasklet takes (lock instrumentation); the latency effect is the §8 budget |
| G-41 | Every *state* event kind has a state getter; after an EQ overflow, getters reflect the true state (notices such as `EPOCH_TICK`, `USER`, `OVERFLOW` are exempt) | U | P | overflow the EQ, then compare |
| G-42 | **r3, rewritten.** Every counter is monotonic non-decreasing for the life of the session — across stop/start, `update_flows`, `reconfigure` and recovery — and no library call resets it; two independent readers computing deltas over overlapping intervals get consistent results (there are no stats epochs) | U | P | note G-42 |
| G-43 | **r3, rewritten.** Queue gauges satisfy a **transition identity**: for each lease state s, `entries[s] − exits[s] = gauge[s]`, where entries and exits are the per-writer transition counters and the gauge is a single-pass scan of the slot state words | U, UB | P | note G-43 |
| G-44 | In release builds no log line is formatted or printed on a busy-loop thread at any level; tasklet logs go through the binary log ring | UB | P | log hook asserts on busy-loop threads `[C1 #14]` |
| G-45 | The same verb sequence passes G-01…G-09 for every essence × direction | U, UB | P | parameterise the suites over the five create functions (video, cvideo, audio, anc, fastmeta) |
| G-46 | A TX verb on an RX session (and vice versa) returns `-MTL_EINVAL` and changes nothing | U | P | |
| G-47 | Every result and delivery returns the submission's (TX) or buffer's (RX) `user_cookie` verbatim; a TX result with no submission cookie carries the buffer's; a non-zero cookie in completion NONE fails the submit with `-MTL_EINVAL`, reason `COOKIE_WITHOUT_RESULTS` (r3) | U | P | random cookies |
| G-48 | A buffer's planes never change between uses; per-use fields exist only in the submission/result | U | P | hash `mtl_buffer_get_view` before and after 10^4 uses |
| G-50 | **r3.** Every exported function carries exactly one `MTL_API_{CP,DP,DPC,WT,AS}` annotation, and debug builds enforce it: a thread-local class is set at every entry point, and allocation, mutex lock, sleep and `info()`/`warn()` assert the class is not DP, DPC or AS | B, U | P | note G-50 |
| G-52 | Try-wait + wait object never loses a wake-up, for private and shared CQs, EQs and session wait targets; the library drains the wait object | U | P | litmus-style two-thread test with forced interleavings of 04 §5.1 `[C1 #3]` |
| G-54 | `mtl_time_get_status` clears LOCKED on sync loss and posts `TIME_STATE` | UB | P | `mtl_debug_inject(PTP_LOST)`; pins SF-20 |
| G-55 | `queued_media_ns` equals the sum of queued units' durations; each histogram's `count` equals the number of published results | U | P | |
| G-56 | RX leases released out of order, from other threads, return to FREE; the overflow policy acts only on READY slots | U | P | |
| G-57 | Each status, reason (`enum mtl_state_reason`) and 07 §5 error code is produced by its documented trigger | U, UB | P | table-driven trigger list on the null backend with `mtl_debug_inject` |
| G-58 | EQ coalescing never loses the first or last state of a transition and never blocks a producer; `USER` posts never evict library notices (own ring, r3); **r3:** every EQ subscribed to a source receives each state event of that source (coalesced), independently of other subscribers | U | P | tasklet producer + reader stress `[C1 #8]` |
| G-69 | **r3.** `*_trywait()` returns 1 = something is ready (do not block), 0 = armed and safe to block on the wait object, < 0 = error; it never returns `-MTL_EAGAIN`; after 0, the wait object becomes readable no later than the next target becoming ready | U | P | both outcomes with forced interleavings; the libfabric `fi_trywait` inversion `[C5 §2.10]` |
| G-79 | **r3.** From a busy-loop thread (user tasklet, RX packet lcore), the inline-safe DP subset (`mtl_tx_acquire`/`mtl_rx_dequeue`/`mtl_tx_reap` with timeout 0, `mtl_tx_submit`, `mtl_tx_release`, `mtl_rx_release`, getters) never blocks: it only trylocks the reaper lock (`-MTL_EAGAIN` if contended), never drains or writes a wait object and makes no syscall | UB | P | note G-79 |
| G-82 | **r3.** An RX unit whose due time (06 §11.7) passes is force-completed within one scheduler iteration and delivered or discarded per `rx_incomplete`, with no further packet; `stop(DRAIN)` on RX delivers the partial unit with its completeness; with no due time the waker never sleeps longer than 1 ms | UB | P | note G-82 |
| G-88 | **r3.** A session name is copied at create (64 B), unique per instance (`-MTL_EEXIST` on a clash with a live or DESTROYING session), and appears in `get_info`, the stats header, the log prefix and every event (`origin_name`); `mtl_instance_list_sessions` returns every live session exactly once | U | P | note G-88 |
| G-89 | **r3.** If `MTL_QUERY_CHECK_CAPACITY` succeeds and nothing else changes the host, the create succeeds; if it fails, the reason names the limiting resource; `mtl_port_get_capacity` returns to its baseline after every destroy | U, I | P | fill a scheduler to its quota and its session limit on the null backend (U); RL queues on a VF (I) `[C5 §8.11]` |
| G-75 | **r3.** `mtl_session_update_flows` is all-or-nothing across legs: ARP/IGMP are prepared and rules and queues reserved before commit, and on failure nothing changes. TX `AT_MEDIA_INDEX k`: unit k is the first on the new flows on every leg. RX `AT_TAI t`: the new rule accepts the units whose RTP-derived media time ≥ t; the old rule is removed after | U, I | P | note G-75 |
| G-76 | **r3.** RX start and timing: join and flow rule at the first start, kept until destroy or `update_flows`; ARMED = joined and discarding units whose media time < t; RUNNING from t; RX sessions on EPOCH or AT_TAI timelines report `media_index` = the exact inverse of the TX rule (06 §11.2); an RX group arms every member together | U, UB | P | note G-76 |
| G-90 | **r3.** Create and start never wait for ARP or IGMP: an unresolved leg is `WAITING_NEIGHBOUR`, its units are `DROPPED/NO_NEIGHBOUR` on that leg (DROPPED when every leg is unresolved), resolution resumes sending without an API call, and `FLOW_STATE` reports resolved / unresolved / joined / join-failed per leg with a getter | U, I | P | note G-90 |
| G-91 | **r3.** Resources are reserved for every configured leg regardless of link, and a configured leg is never pruned; `mtl_session_set_leg_enabled` is a boundary command: disabling or re-enabling a leg takes effect at the next unit boundary; admin and oper state per leg are in the status and `LEG_STATE` | U, I | P | note G-91 |
| G-92 | **r3.** The null backend completes every unit at its scheduled launch instant read from the instance clock; with the test time source, a run is deterministic (same inputs → byte-identical CQ records) | U | P | run the §6 and §3 suites twice and diff `[C5 §3.3]` |
| G-93 | **r3.** Every fault of §6 is injectable with `mtl_debug_inject` in builds with `-Denable_debug_api=true`; in a release build every `mtl_debug.h` function returns `-MTL_ENOTSUP` and no fault or test-clock code is linked | U, B | P | U: each fault reaches its outcome; B: release stubs return `-MTL_ENOTSUP`, `nm` shows no fault internals ([15 §6](15-security-and-deployment.md)) |
| G-95 | **r3.** Waiters are per wait target (buffers, results, RX ready, events), each with its own armed bit: a thread blocked in `mtl_tx_acquire` and another blocked in `mtl_tx_reap` on the same session both wake for their own target and never steal each other's wake-up | U | P | the P1 two-thread layout `[C5 §3.6]` |
| G-97 | **r3.** The L4 simple layer: `mtl_simple_tx_open` returns a started session with a library pool and completion NONE; the 10-line loop of 10 §14 and every example in `sketch/examples/` compile (C99 and the C++17 twin) and run on the null backend | B, U | P | `sketch/check.sh` plus a CI job running the examples against `null:1` `[C5 §3.1]` |
| G-99 | **r3.** Engines first: with every legacy opt-in flag off, the legacy wire output of each E-change is identical to the pre-change baseline; with the flag on it matches the 13 §7 oracle | UB, I | P | per E-change, a pcap diff against the baseline in the legacy KahawaiTest + acceptance gate ([14 §2](14-implementation-roadmap.md)) `[C5 §1.4]` |

Notes for the rows above (the test method; "Also:" marks further clauses of the guarantee):

- **G-75:** Move both legs to a new multicast pair at one TAI instant; make one leg fail
  (no ARP answer) and check nothing changed; at the I tier capture both legs and check no
  unit mixes old and new flows `[C5 §8.1]`. Also: no unit goes to a mix of old and new
  destinations; a change of `flow.port` fails with `-MTL_EINVAL`, reason
  `PORT_CHANGE_NEEDS_RECONFIGURE`.
- **G-76:** Start at `AT_TAI t` with packets flowing before t; stop and start again without
  an IGMP leave; the latest-only recipe (`pool.count = 2`, `MTL_RX_RECLAIM_OLDEST_READY`);
  video, audio and ANC receivers aligned with `mtl_rx_align` `[C5 §5.7, §8.4]`. The index
  inverse is checked against the §7 oracle for a sender on the grid at every rate (1001
  families, fields, floor-aligned 44.1 kHz) across the 2^32 wrap, and for a sender with a
  phase φ < P, which maps to the slot it falls in. Also: every member of an RX group
  started at t delivers its first unit with media ≥ t and discards every unit with
  media < t.
- **G-39:** Run tasklets under syscall counting and a `malloc`/`rte_malloc` hook during a
  stress run; the idle-sleep path, W2 sessions and the kernel-socket/AF_XDP backend syscalls
  (reported via `backend_syscalls_on_tasklet`) are excluded; measured per `TASKLET_THREAD`
  mode (r3) `[C1 #7, #15]`. The W2 exception is one non-blocking eventfd `write()`, only
  when a waiter is armed: in lcore mode it is a designer recommendation for unit periods
  below 1 ms that needs maintainer decision M6 (Q-THR-2); in `MTL_FLAG_TASKLET_THREAD` mode,
  whose threads are not pinned, it is the default (04 §5.2, §7.1).
- **G-42:** Two reader threads with different periods over a randomised session life `[C5 §8.8]`
- **G-43:** Exact at every quiescent point (test clock paused, app threads parked), sampled ≥
  10^4 times during a stress run on the null backend; between quiescent points only a bound is
  checked: the absolute value of `entries − exits − gauge` is at most the number of completing
  contexts. Revision 2's `slots = Σ states` was a tautology for a scan and unimplementable for
  per-writer sums `[C5 §8.9]`
- **G-50:** Header lint; the enforcement asserts under the U suite; a signal-safety test
  raises a signal inside every CP call under a `malloc`/`pthread_mutex_lock` interposer `[C5
  §2.15]`
- **G-79:** The subset called from a user tasklet under contention from an app thread `[C5 §4.1]`
- **G-82:** Feed half a frame and stop sending `[C5 §4.9]`. The due time is the first-packet
  arrival on the earliest leg + unit period + `rx_flush_offset_ns`, capped at
  `presentation_tai_ns` when a link offset is set.
- **G-90:** A destination that never answers ARP; create returns within its CP bound `[C5 §7.6]`
- **G-91:** Null backend (U); I: `nicctl.sh vf_link … down/up` while RUNNING (Phase 2, §6)
  `[C5 §8.2]`. Also: a down leg does not starve the pool (units complete with that leg
  marked not sent) and resumes at the next unit boundary after its link returns, with no
  app action.
- **G-88:** Create/destroy churn with a concurrent lister; free the config's name buffer right
  after create `[C5 §8.7]`

² No `malloc`/`rte_malloc*`/pool or ring create.

**Bindings.** When the SWIG (Python) binding for the new header lands, its test suite
includes a GIL check: a WT call blocked in one Python thread (for example `mtl_rx_dequeue`
with a 1 s timeout) must not stop a second Python thread from running, because the
interface releases the GIL around every WT call (11 §4) `[C3 B-3]`. **r3:** it runs on the
null backend (tier U) against the installed library, which revision 2 could not do
because `pymtl` wraps the installed `.so` `[C5 §3.3]`.

## 6. Fault injection matrix (r3)

Every G-01/G-31/G-37 test runs under these faults. Revision 2 listed faults that nothing
could inject or orchestrate `[C5 §8.10]`; each row now names its U/UB injection point
(`mtl_debug_inject(object, fault, params)`, [10](10-api-sketch.md) `mtl_debug.h`) and its
I-tier step in `.github/scripts/gtest.sh`. Today `nicctl.sh` offers `bind_kernel`,
`create_vf`, `create_kvf`, `create_tvf`, `disable_vf`, `bind_pmd`, `list` and nothing that
pulls a link or resets a VF (`script/nicctl.sh:146`); the two new subcommands are Phase 1
work for link and Phase 2 for reset.

| Fault | U/UB injection | I-tier step | Expected | Ev. |
|---|---|---|---|---|
| link down on one 2022-7 leg | `LEG_DOWN(session, leg)`; `LEG_UP` restores | new `nicctl.sh vf_link <vf_bdf> down\|up` (PF-side `ip link set <pf> vf <n> state disable\|enable`) | `LEG_STATE` event (oper down); units keep flowing on the other leg; results `ON_TIME` | P (U/UB); P at I from Phase 2 |
| link down on the only leg | `LEG_DOWN(session, leg)` | as above | results `DROPPED/LINK_DOWN`; session stays RUNNING (Q-LIFE-7) | P |
| TX queue hang → recovery | `TX_QUEUE_HANG(session, leg)`: the transmitter's burst returns 0 until cleared | none: a real hang cannot be forced | `SESSION_RECOVERY` begin/end; in-flight units `DROPPED/RECOVERY`, never `ON_TIME`; recovery on a worker; `sh_info` untouched (SF-41) | P (UB) |
| VF reset / port restart | `PORT_RESET(port)` | new `nicctl.sh vf_reset <vf_bdf>` (`/sys/bus/pci/devices/<bdf>/reset`) | `PORT_RESET`; outcome per Q-LIFE-10 | P (UB); BE at I until Phase 5 |
| device gone | `PORT_RESET(port, unrecoverable)` | `nicctl.sh disable_vf` | session ERROR with `-MTL_ENODEV`; `start` fails with `-MTL_ENODEV` until resources can be re-reserved | P (U) |
| PTP loss / clock step | `PTP_LOST`, `PTP_STEP(Δ)`, or `mtl_time_test_advance` | stop `ptp4l` / `phc_ctl <if> adj Δ` on the lab host | `TIME_STATE` / `TIME_STEP`; step policy per timeline (G-62) | P (U/UB); BE at I |
| forced ERROR | `FORCE_ERROR(reason)` | — | ERROR row of 03 §3.4: QUEUED → `FLUSHED/SESSION_ERROR`, IN_FLIGHT → `FAILED/<reason>` once its device references are gone, RX RECEIVING discarded and counted; waiters get `-MTL_EIO` | P (U) |
| packet loss patterns | `DROP_PKTS(pattern)` per leg (generalises today's `MTL_SIMULATE_PACKET_DROPS`, TX video only, `lib/meson.build:91-96`) | — | RX missing ranges, 2022-7 reconstruction, `units_used_redundancy` | P (UB) |
| app stops reading results | none needed | — | `blocked_on = RESULTS`; `BACKPRESSURE` event; no loss | P |
| app holds all RX buffers | none needed | — | `units_missed_pool_full`, `BACKPRESSURE`; latest-wins reclaims READY units only | P |
| destroy mid-unit, with completions on another thread; destroy with leases held | none needed | — | G-01, G-37, G-65 | P |
| MtlManager dies | `MANAGER_LOST` | `gtest.sh` kills MtlManager, runs creates, restarts it | `MANAGER_LOST`; running sessions continue; G-98 | P |
| scheduler overload | not injectable | load test | `SCHED_OVERLOAD` | BE |

### 6.1 Test isolation (r3)

- The U tier never initialises or tears down EAL per case and never calls
  `mtl_init`/`mtl_uninit` per case. The test binary initialises EAL once
  (`--no-huge --no-pci`, as `tests/unit/common/ut_common.c:25-31` already does) and opens
  one long-lived null-backend instance as a fixture; cases create and destroy sessions in
  it `[C5 §8.12]`.
- The G-49 and G-57 tables run inside that one long-lived instance (or one long-lived
  VF instance at the I tier), because the last `mtl_uninit` of a default instance happens
  inside the library and a clean re-init per case is impossible today (#1341).
- `NoCtxTest.*` stays one process per case (`tests/integration_tests/noctx/run.sh`).
- The debug APIs fault state is per object and cleared by the fixture after each case;
  a case that leaves a fault set fails.

## 7. Timing oracle and the contract document

Adopt the contract document's oracle *method* (`doc/user-pacing-timestamp-contract.md`,
Level 3 and Appendix A — an untracked draft today): compute every expected value from
the anchor with exact arithmetic, check every packet (not only packet 0), never infer
anchors from captured output. The Phase 0.5 `st_timeline_*` helper is unit-tested against
this oracle ([14 §1.6](14-implementation-roadmap.md)). This design **replaces** several
of the contract's *selection and state-transition rules*; the contract needs an update
before the engines track changes the wire `[C2 #13]`. **r3** oracle cases beyond G-19: the T0
formula for k = 0…100 over the eight configurations of 06 §3.1 (G-68); the RX index inverse
(G-76); and the Phase 0.5 round trip, where `legacy_ts_ns` fed to legacy `USER_TIMESTAMP`
reproduces the helper's RTP on the wire (G-106):

| Contract says | This design | Question |
|---|---|---|
| RTP `floor` | same | Q-TIME-15 (legacy keeps rounding unless the opt-in flag is set) |
| user pacing = nearest *slot* (N·T + packet-0 offset), may be before the request | media time primary; `NOT_BEFORE` never sends early; legacy USER_PACING maps to TAI media snapped to the *epoch* — migrated apps' TX moves by ≈ TROFFSET − VRX0·TRS | Q-TIME-21 |
| later audio buffers must equal the grid point or be rejected | contiguous unless DISCONTINUITY; a forward gap pads to the packet boundary and keeps the grid (G-86); CAPTURE absorbs ±one packet by default | Q-TIME-6 |
| 1 s horizon | configurable, default 1 s, measured from the resolved start instant (G-94) | Q-TIME-3 |
| ns rounding ties choose the later ns | undecided: the code rounds ties down (`st_muldiv_u64_round_closest`); pick one and pin it | Q-TIME-15 |
| too-early / insufficient-lead requests are REJECTED, consume no media unit, advance no RTP | late at pick-up → accepted, then DROPPED (RTP slot consumed) or RESLOT; only synchronously knowable cases are rejected at submit | Q-TIME-23 |
| the user timestamp must be a non-zero TAI value | zero is valid when flagged (G-17); the session mode, not the value, says what is meant | — |
| an exact ST40 request outside the LLTM/CTM window is rejected | same (06 §5.3) | — |
| audio packet-0 TX target = the grid point = the RTP instant | TX = M + D_a (D_a small, from the pacing profile) | Q-TIME-11 |
| audio RTP anchored at the first TX request in user mode | RTP from the timeline sample index | — |
| `rtp_anchor_ticks` includes the delta; ST20/ST40 equal "with the same RTP clock offset" | offset forbidden in the new API; whole-unit `media_index_offset`, or **r3** `media_time_offset_ns` in TAI mode (ns precision, replaces `rtp_timestamp_delta_us`) | Q-TIME-5 |
| ST20 packet offsets = `packet_index × packet_interval`, no TLINE/2 | linear TRS for NL/W; `+TFRAME/2 + TLINE/2` for the second field; second-field RTP from its own media time (G-22) | Q-TIME-17 |
| packet-0 slot includes `− initial_virtual_receiver_packets × packet_interval` | same, stated as `scheduled_first = TVD − VRX0·TRS` | — |
| ST40 target = `st40_target_delay` inside the window | same (`anc_target_delay_ns`, now in `mtl_anc_config`) | Q-TIME-22 |
| `receive_timestamp` = 0 if packet 0 is missing | validity flags | — |
| `tx_queue_available_time` term in the scheduling cutoff | bounded SEND_LATE (`WOULD_OVERLAP`) | — |
| scope ST20/ST30/ST40 only | ST22 `rate_mode` and ST41 at 90 kHz included; the contract needs rules for them | — |

## 8. Performance budgets (r3: numbers)

Budgets are on tails, not means, because pacing is damaged by tails `[C1 #9]`. Revision 2
set every gate "by measurement in Phase 1", so Phase 1 could not fail its own gate
`[C5 §1.8]`. These are **initial budgets**. Spike S0 in Phase 0 measures today's baseline
with `MTL_FLAG_TASKLET_TIME_MEASURE` (`include/mtl_api.h:449`) on the reference machine
and confirms or revises them before Phase 1 starts; a revision is recorded in DECISIONS.

| Metric | Budget | Measured by |
|---|---|---|
| tasklet iteration time per scheduler, same load as the legacy pipeline | p99.99 ≤ legacy + 2 %; max ≤ legacy + 5 % | `MTL_FLAG_TASKLET_TIME_MEASURE`, I tier, 10 min runs |
| DP call cost without conversion (`acquire`, `submit`, `reap`, `dequeue`) | p50 ≤ 150 ns; p99 ≤ 1 µs | U microbenchmark on the null backend + I spot check; DPC calls reported separately |
| completion latency (last packet handed / last mbuf freed → result visible) | ≤ the `completion_latency_ns` reported in `get_info` + 10 % | UB/I |
| waker CPU | ≤ 2 % of one core at 100 armed sessions | S1, then I |
| W2 wake latency | p99 ≤ 10 µs | S1, then I |
| W3 (waker thread) wake latency | p99 ≤ the unit period / 10 for units ≥ 1 ms; below 1 ms W2 is recommended (M6) | S1 |
| sessions per scheduler at 1080p59.94 | unchanged versus legacy | I |
| `mtl_cq_read` on a shared CQ | cost proportional to the members with work, not to the member count | U microbenchmark on the null backend |
| ST 2110-21 narrow compliance under stress (stats reader in a tight loop, 64 armed waiters) | no regression versus legacy, per NIC × pacing class | M (G-27, Phase 2 exit) |
| frame-start error of the published time base versus a direct PHC read | reported by S7; budget set from it | S7 |

## 9. The test substrate (r3)

Review C5 found no NIC-less backend, no time injection and no fault injection, so the
bindings, the doc examples and half the U tier needed a NIC `[C5 §3.3, §8.10]`. The
substrate ships first (Phase 1; its design is a Phase 0 deliverable):

| Piece | Shape | Where specified |
|---|---|---|
| null backend | port spec `null:<n>`; units complete at their scheduled time from the instance clock; TX→RX loopback on the same port; no NIC, root or hugepages; experimental, in the library | [02 §2](02-architecture.md), [09](09-media-modes-and-backends.md) |
| test time source | `mtl_time_test_source(mt, base_tai_ns, rate)`, `mtl_time_test_advance(mt, ns)` in `mtl_debug.h`; the published time base honours it; test builds enable the debug API | [06 §2](06-timing-pacing-and-sync.md), [15 §6](15-security-and-deployment.md) for its gating |
| fault injection | `mtl_debug_inject(struct mtl_object, uint32_t fault, const struct mtl_fault_params*)` with `LEG_DOWN`, `LEG_UP`, `TX_QUEUE_HANG`, `PORT_RESET`, `PTP_STEP(Δ)`, `PTP_LOST`, `MANAGER_LOST`, `FORCE_ERROR(reason)`, `DROP_PKTS(pattern)`, `TX_MUTATE` (today's ST40 test knobs); only with `-Denable_debug_api=true` | [10](10-api-sketch.md), [15 §6](15-security-and-deployment.md) |
| I-tier orchestration | `nicctl.sh vf_link`, `nicctl.sh vf_reset`, MtlManager kill/restart steps in `gtest.sh` | §6 |
