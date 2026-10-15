# MS1 status

| | |
|---|---|
| Status | The ledger of milestone MS1. The implementing session keeps it current: one row per task, updated when a task changes state |
| Reads with | [implementation-plan.md](implementation-plan.md) §5 (tasks, gates, process), [README.md](README.md) §1 |
| Work branch | `unified-api-ms1`, from the doc-set commit on `unified-api-design` |
| Base commit | (record the doc-set commit here when the work branch is made) |

On every resume: read [README.md](README.md) §1, this file and `git log --oneline -20`, then
continue with the first task that is not done.

## 0. Standing instructions

The maintainer's standing permission (2026-10-02):

- Commit each task through the `mtl-commit` skill once Gate 5 is APPROVE and Gate 6 has run where
  marked.
- Make one signed-off commit per commit of a task (a series has several, implementation-plan.md
  §5.8).
- Add no AI attribution.
- Never push. Never open a pull request.

This overrides the skill's "only when asked" for MS1 tasks.

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
| C1b (series of 3–4) | todo | | | | |
| A1 (series of 2) | todo | | | | |
| C2 (series C2a, C2b) | todo | | | | |
| B1 | todo | | | | |
| B2 | todo | | | | |
| A2a (series of 2) | todo | | | | |
| A2c (series of 2) | todo | | | | |
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

Batch them; ask at a checkpoint, not between. An open question gives the options, a
recommendation, and an answer line (§3.1).

### 3.1 Open questions

For checkpoint 1 (day 5), the plan changes and design points that need a stop-and-ask:

| Question | Options | Recommendation | Needed by | Answer |
|---|---|---|---|---|
| The data tables: the import's ranges and milestones for keys and values the import could not settle (H1b part 0, D-185) | | | day 5 | |
| Stop returns `-MTL_EIO` after a fault during its drain or flush, and the session is STOPPED (D-186): confirm? | | | C1a | |
| The reason for a key or value that does not apply: one name for option keys and for values outside their essence? | `MTL_REASON_NOT_APPLICABLE` = 234 for both | that name, not `OPTION_NOT_APPLICABLE` (223 stays unused); confirm the name | H1b | |
| D-144's exception now ends with the bridge (D-190): confirm? | | | A1, E1 | |
| Plan: A2a split into A2a and A2c; CI1a in H1b, CI1b in P1; E1 in week 1; review units of commit series (D-107: H1b and E1 four commits, C1b three or four, C1a, A1, C2, A2a and A2c two) | A2a split; A2a unsplit, over the commit cap at about 2.4 k lines | split | day 5 | |
| Plan: the MS1 sample set (SA1 folded into P1) | | | day 5 | |
| Plan §5.2: 4 review units a week, with C0, M0 and the copies not counted | not counted; counted, so they land in I1 and R1 | not counted: confirm | day 5 | |

### 3.2 Decided, to confirm

For checkpoint 1 (day 5): the issues of [decisions.md](decisions.md) §5 that MS1 implements,
decided in the design rounds, and the decisions the maintainer approved with the set of
D-208…D-244. Each stands unless the maintainer overrules it here at checkpoint 1, in the
Confirmed column.

| Item | Needed by | Decision | Confirmed |
|---|---|---|---|
| OI-63, OI-66: several threads on one session; the wake cost | C0, C1w | Decided 2026-10-06: D-142, D-158–D-170 | |
| OI-64: later-milestone declarations, nodes per milestone, the bridge in its own node (D-190, D-191) | H1b | Recommended as designed: OI-64 (`MTL_API_*(n)`, `MTL_LEVEL`, `check_exports.sh --seal`, the tag rule); no `-z now` | |
| OI-70, the MS1 part: `mtl_interrupt(o, mode, uint64_t targets)`, `MTL_RETIRING`, a cookie without results is `-MTL_EINVAL` (`COOKIE_WITHOUT_RESULTS`) in every build | C0, C1w, A2a | The cookie rule: approved by the maintainer (2026-10-05). The rest as designed: OI-70 (decisions.md §5.4); the interrupt codes are those of contract.md §8.4 | |
| OI-71: interlaced and PsF rasters above 30 frames per second | C2, B1 | Resolved: OI-71 (decisions.md §5.5) | |
| OI-72: what a bridged instance reports and accepts | A1, E1 | Resolved: OI-72 (decisions.md §5.1), D-192; one WARNING for a UTC clock (D-235) | |
| OI-74: the definition of `latency_min_ns`, `latency_max_ns` | A2c | Resolved: D-225, superseding D-156 (decisions.md §5.2). With two legs F is the configured skew budget, never a measured one: a framework's latency changes only when C, the caller's work per unit, grows. RX max is (pool_count − 1) × U, not pool_count × U | |
| OI-67, OI-69, MS1 parts: RX slots and tail, ex20, `enum_names`, `caps.pacing_req`, `log_level`, the `MTL_PORTS` grammar | H1b, A1, B2, A2c | Resolved: D-228, D-151, D-153, D-154; the inline helpers of D-197 with the headers; D-199 superseded by D-227. With them: `log.level` removed; +2 MS2 exports (`mtl_option_parse`, `mtl_port_parse`); the V210 width check until E15; the new MUST R-FW-1 | |
| ANC skip counts | H1b | Decided: D-157 (`mtl_anc_decode_info.skipped` is a `uint32_t`; `enum mtl_anc_skip` is gone) | |
| The TSQ idle cleanup in MS1 | E1b | Decided: D-213. E1b rewrites `mt_tsq_done_cleanup`, since MS1 offers `MTL_TXQ_SHARED` (U-075), so a video session on a shared queue needs its last frame completed | |
| Several submitting threads (`MTL_SESSION_MT_SUBMIT`) | C1b, A2a | Decided: D-208. Their acquires use a finite timeout | |
| C, the caller's work, held once in `latency_min_ns` | B2, A2c | Decided: D-225, superseding D-156 | |
| MEM3 is not a reap gate: an application-memory session always produces results, and an attached slot is acquirable when PUBLISHED and not held by D-209 | C1b | Decided: D-214 | |
| The copy margin: B = `pool_count` − R_copy (one slot of margin), not `pool_count` − R_copy + 1 | B2 | Decided: D-227 | |
| The repository rule on RX gaps changes in the rule files with the MS2a black-fill commit, not with the docs | MS2a | Decided: D-242; implementation-plan.md §6.1 quotes the text | |
| One (format, kind) per format name, before the A2c table | A2c | Decided: D-222 | |
| `mtl_release` with a null `s`; the entry's lease base | C1a, C1b, A2a | Decided: D-226 | |
| C0: `st_core.h` against core.md §2, §2.1 (the binding ops and core calls, D-171), the grant and the grid of core.md §3.2 (the core reads no essence) and the state register of core.md §4.1 (D-180) | C0 (checkpoint 1) | Recommended as designed; the maintainer approves or changes the tables and the register with C0 | |

## 4. Measurements

S0 and every later measurement that a gate or a budget of implementation-plan.md §8.4 uses. One
row per configuration and stack. Each value is the median of 3 interleaved runs. The A/A spread of
the legacy runs goes in brackets. The host record of plan §8.4 "Topology" goes above the table.

Key: M class is the compliance class the monitor M found; C is the capacity C(load, pacing class,
legs). Both are defined in implementation-plan.md §8.4.

| Date | Load | Legs | Pacing | Stack | Visit avg TX / RX (ns) | Iteration p50 / p99 / p99.9 / p99.99 (µs) | Max (µs) | Lost frames TX / RX | M class | C |
|---|---|---|---|---|---|---|---|---|---|---|
