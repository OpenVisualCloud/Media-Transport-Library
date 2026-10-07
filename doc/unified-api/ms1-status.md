# MS1 status

| | |
|---|---|
| Status | The ledger of milestone MS1. The implementing session keeps it current: one row per task, updated when a task changes state |
| Reads with | [implementation-plan.md](implementation-plan.md) §5 (tasks, gates, process), [README.md](README.md) §1 |
| Work branch | `unified-api-ms1`, from the doc-set commit on `unified-api-design` (record the base commit here) |

On every resume: read [README.md](README.md) §1, this file and `git log --oneline -20`, then
continue with the first task that is not done.

## 0. Standing instructions

The maintainer's standing permission (2026-10-02): commit each task through the `mtl-commit` skill
once Gate 5 is APPROVE and Gate 6 has run where marked; one signed-off commit per commit of a task
(a series has several, implementation-plan.md §5.8), no AI attribution; never push, never open a
pull request. This overrides the skill's "only when asked" for MS1 tasks.

## 1. Tasks

States: todo, in progress, in review (`mtl-reviewer`), committed (waiting for the maintainer's
approval), done (approved), cut. A series is one row; its Commit cell lists every commit.

| Task | State | Branch | Commit | Gate evidence (Gates 2, 4, 5, 6) | Open questions |
|---|---|---|---|---|---|
| P0 | todo | | | | |
| M0 (docs, design branch) | todo | | | | |
| S0 | todo | | | | |
| T1 | todo | | | | |
| H1b (series, parts 0–3) | todo | | | | |
| E1 (series E1a–E1d) | todo | | | | |
| C0 | todo | | | | |
| C1a (series of 2) | todo | | | | |
| C1w | todo | | | | |
| C1b (series of 2) | todo | | | | |
| A1 (series of 2) | todo | | | | |
| C2 | todo | | | | |
| B1 | todo | | | | |
| B2 | todo | | | | |
| A2a | todo | | | | |
| A2c | todo | | | | |
| I1 | todo | | | | |
| R1 | todo | | | | |
| P1 | todo | | | | |
| A2b (stretch, committed in MS2a) | todo | | | | |
| B3 (stretch, committed in MS2a) | todo | | | | |
| X (stretch, committed in MS2a) | todo | | | | |

## 2. Gates

| Day | Gate | Result |
|---|---|---|
| 5 | C0 approved by the maintainer against the binding ops and the register; H1b committed and approved; E1 committed | |
| 10 | C1a, C1w, C1b and A1 committed and approved | |
| 15 | first unified frame on a VF (I1 cross TX) | |
| 17 | feature freeze: fixes only | |

## 3. Questions for the maintainer

Batch them; ask at a checkpoint, not between. Each question: the options, a recommendation, and
an answer line.

For checkpoint 1 (day 5): the issues of [decisions.md](decisions.md) §5 that MS1 implements,
decided in the design rounds of 2026-10-05 and 2026-10-06 (each row gives the resolution; the
maintainer confirms or changes it on the answer line), and the plan changes that need a
stop-and-ask:

| Question | Needed by | Answer |
|---|---|---|
| OI-63, OI-66: several threads on one session; the wake cost | C0, C1w | decided (2026-10-06): calls with a timeout sleep on the object's event word and remove their own count; probes arm nothing; no descriptor in MS1; queues and ex03 in MS2a; the flush wakes a bounded number per iteration; the notifier is the contingency (D-142, D-158–D-170); to confirm |
| OI-64: later-milestone declarations: the macro carries the milestone, the `error` attribute marks later functions; nodes per milestone, sealed at the exit and every release; the bridge in its own node (D-190, D-191) | H1b | recommended as designed (`MTL_API_*(n)`, `MTL_LEVEL`, `check_exports.sh --seal`, the tag rule); no `-z now`; the maintainer confirms |
| OI-70, the MS1 part: `mtl_interrupt(o, mode, uint64_t targets)`, `MTL_RETIRING`, a cookie without results is `-MTL_EINVAL` (`COOKIE_WITHOUT_RESULTS`) in every build | C0, C1w, A2a | the cookie rule is decided: the maintainer approved `-MTL_EINVAL` in every build (2026-10-05); the rest as designed (decisions.md §5.4); the interrupt codes are A's (contract.md §8.4) |
| OI-71: interlaced and PsF rasters above 30 frames per second | C2, B1 | resolved: `-MTL_EINVAL`, `FIELD_RATE` (233), when the reduced fps of an interlaced or PsF raster exceeds 30; `mtl_raster_from_legacy()` for ports (decisions.md §5.5); the maintainer confirms |
| OI-72: what a bridged instance reports and accepts | A1, E1 | resolved (decisions.md §5.1): the legacy clock of port P; MS1 reads the default and TSC clocks directly, others `-MTL_ENOTSUP`; C keys `-MTL_EBUSY`, R keys `-MTL_ENOTSUP`; `mtl_uninit` `-EBUSY` until the wrapper retired; a second wrap `-MTL_ENOTSUP` until MS2a (D-192). Stop-and-ask: D-144, now ending with the bridge (D-190) |
| OI-74: the definition of `latency_min_ns`, `latency_max_ns` | A2c | resolved (D-156; decisions.md §5.2); with two legs F stays the configured skew budget, never a measured one (PRF-8's proposal declined: a framework's latency must not change while it runs). D's point: RX max is (pool_count − 1) × U, against the accepted pool_count × U |
| OI-67, OI-69, MS1 parts: RX slots and tail, ex20, `enum_names`, `caps.pacing_req`, `log_level`, the `MTL_PORTS` grammar | H1b, A1, B2, A2c | resolved (D-150, D-151, D-153, D-154; the inline helpers of D-197, D-199 with the headers). D's points: `log.level` removed; +2 MS2 exports (`mtl_option_parse`, `mtl_port_parse`); V210 width check until E15; the new MUST R-FW-1 |
| ANC skip counts | H1b | decided (D-157): one total, `mtl_anc_decode_info.skipped` (`uint32_t`) and `mtl_rx_detail.anc_skipped`; each cause is the stats key `anc.pkts_skipped{cause}`; `enum mtl_anc_skip` is gone |
| C0: `st_core.h` against core.md §2, §2.1 (the binding ops and core calls, D-171), the grant and the grid of §3.2 (the core reads no essence) and the state register of core.md §4.1 (D-180) | C0 (checkpoint 1) | recommended as designed; the maintainer approves or changes the tables and the register with C0 |
| The data tables: the import's ranges and milestones for keys and values the import could not settle (H1b part 0, D-185) | day 5 | |
| Stop returns `-MTL_EIO` after a fault during its drain or flush, and the session is STOPPED (D-186) | C1a | |
| The reason for a key or value that does not apply: `MTL_REASON_NOT_APPLICABLE` = 234, one name for option keys and for values outside their essence, not `OPTION_NOT_APPLICABLE` (223 stays unused) | H1b | recommended; confirm the name |
| Plan: A2a split into A2a and A2c; SA1 folded into P1; CI1a in H1b, CI1b in P1; E1 in week 1; review units of commit series (D-107: H1b and E1 four commits, C1a, C1b, A1 two) | day 5 | plan §5.2: 4 review units a week (C0, M0 and the copies not counted: confirm, else they land in I1 and R1). Stop-and-ask: the A2a split (unsplit, over the commit cap at about 2.4 k lines) and the sample set |

OI-76, OI-77 and OI-78 are decided (decisions.md §5.5). In MS1, N on 1080p at 100, 120000/1001 and
120 and SD interlaced N carry `MTL_INFO_NON_COMPLIANT`.

## 4. Measurements

S0 and every later measurement that a gate or a budget of implementation-plan.md §8.4 uses: one row
per configuration and stack, medians of 3 interleaved runs, the A/A spread of the legacy runs in
brackets, the host record of §8.4 "Topology" above the table.

| Date | Load | Legs | Pacing | Stack | Visit avg TX / RX (ns) | Iteration p50 / p99 / p99.9 / p99.99 (µs) | Max (µs) | Lost frames TX / RX | M class | C |
|---|---|---|---|---|---|---|---|---|---|---|
