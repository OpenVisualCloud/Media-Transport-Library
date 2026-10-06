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
once Gate 5 is APPROVE and Gate 6 has run where marked; one signed-off commit per task, no AI
attribution; never push, never open a pull request. This overrides the skill's "only when asked"
for MS1 tasks.

## 1. Tasks

States: todo, in progress, in review (`mtl-reviewer`), committed (waiting for the maintainer's
approval), done (approved), cut.

| Task | State | Branch | Commit | Gate evidence (Gates 2, 4, 5, 6) | Open questions |
|---|---|---|---|---|---|
| P0 | todo | | | | |
| S0 | todo | | | | |
| T1 | todo | | | | |
| H1b | todo | | | | |
| E1 | todo | | | | |
| C0 | todo | | | | |
| C1a | todo | | | | |
| C1w | todo | | | | |
| C1b | todo | | | | |
| A1 | todo | | | | |
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
| 5 | C0 approved by the maintainer; H1b committed and approved; E1 committed | |
| 10 | C1a, C1w, C1b and A1 committed and approved | |
| 15 | first unified frame on a VF (I1 cross TX) | |
| 17 | feature freeze: fixes only | |

## 3. Questions for the maintainer

Batch them; ask at a checkpoint, not between. Each question: the options, a recommendation, and
an answer line.

For checkpoint 1 (day 5): the issues of [decisions.md](decisions.md) §5 that MS1 implements,
decided in the design round of 2026-10-05 (each row gives the resolution; the maintainer confirms
or changes it on the answer line), and the plan changes that need a stop-and-ask:

| Question | Needed by | Answer |
|---|---|---|
| OI-63, OI-66: several threads on one session; the wake cost | C0, C1w | resolved: WT calls sleep on a per-object futex word, never on the wait handle; the handle is level, reset by misses, its mask fixed by the first request; interrupts walk the table; the flush wakes at most one object with a syscall per iteration (D-141, D-142, decisions.md §5.2); to confirm |
| OI-64: later-milestone declarations: the call-class macro carries the milestone and the `error` attribute marks later functions; per-milestone nodes, frozen when closed (decisions.md §5.4) | H1b | recommended as designed (B v3: `MTL_API_*(n)`, `MTL_LEVEL`, `exports.MSn.list`, `check_frozen_lists.sh`); the maintainer confirms |
| OI-70, the MS1 part: `mtl_interrupt(o, mode, uint64_t targets)`, `MTL_RETIRING`, a cookie without results is `-MTL_EINVAL` (`COOKIE_WITHOUT_RESULTS`) in every build | C0, C1w, A2a | the cookie rule is decided: the maintainer approved `-MTL_EINVAL` in every build (2026-10-05); the rest as designed (decisions.md §5.4); the interrupt codes are A's (contract.md §8.4) |
| OI-71: interlaced and PsF rasters above 30 frames per second | C2, B1 | resolved: `-MTL_EINVAL`, `FIELD_RATE` (233), when the reduced fps of an interlaced or PsF raster exceeds 30; `mtl_raster_from_legacy()` for ports (decisions.md §5.5); the maintainer confirms |
| OI-72: what a bridged instance reports and accepts | A1, E1 | resolved (decisions.md §5.1): the legacy clock of port P; MS1 reads the default and TSC clocks directly (UTC), others `-MTL_ENOTSUP`; C keys `-MTL_EBUSY`, R keys `-MTL_ENOTSUP`; `mtl_uninit` `-EBUSY` until the wrapper retired. Stop-and-ask: D-144, the legacy `ptp_get_time_fn` on tasklets as the one R6 exception |
| OI-74: the definition of `latency_min_ns`, `latency_max_ns` | A2c | resolved: RX min = the link offset, else S + U + F + W; RX max = (pool_count − 1) × U; TX min = max(0, `min_submit_lead_ns`); TX max = the horizon; `convert_ns`; `MTL_INFO_LATENCY_INFEASIBLE` (D-156; decisions.md §5.2); D's point for the maintainer: RX max is (pool_count − 1) × U, against the accepted pool_count × U |
| OI-67, OI-69, MS1 parts: RX slot layout and tail, ex05, `enum_names` pairs, `caps.pacing_req`, `log_level`, the `MTL_PORTS` grammar | H1b, A1, B2, A2c | resolved (D-150, D-151, D-153, D-154). D's points: `log.level` removed; +2 MS2 exports (`mtl_option_parse`, `mtl_port_parse`); V210 at widths not a multiple of 48 `-MTL_ENOTSUP` until E15; the new MUST R-FW-1 |
| ANC skip counts | H1b | decided (D-157): one total, `mtl_anc_decode_info.skipped` (`uint32_t`) and `mtl_rx_detail.anc_skipped`; each cause is the stats key `anc.pkts_skipped{cause}`; `enum mtl_anc_skip` is gone (frozen names 972 → 965, from `check.sh`) |
| Plan: A2a split into A2a and A2c; SA1 folded into P1 with only `tx_video`, `rx_video`, `legacy_bridge`; CI1a in H1b, CI1b in P1; E1 in week 1; C1w new | day 5 | implementation-plan.md §5.2: 4 reviews a week (C0 and the two copies not counted: confirm, else the copies land inside I1 and R1), every must-stay item kept. Stop-and-ask: the A2a split and the sample set (else A2a ≈ 2.4 k lines) |
| Plan: C1w's pre-decided split (C1h, the wait handle, to MS2a with ex03) | day 10 | only if C1w is over 1.5 k at Gate 4; it removes ex03 from A2a's exit, retags `mtl_get_wait_handle` to MS2 and changes exit criterion 8: stop-and-ask then |

OI-76, OI-77 and OI-78 are decided (decisions.md §5.5). In MS1, N on 1080p at 100, 120000/1001 and
120 and SD interlaced N carry `MTL_INFO_NON_COMPLIANT`.

## 4. Measurements

S0 baseline (RxTxApp `--tasklet_time`, avg and max per pacing class), and every later measurement
that a gate or a budget of implementation-plan.md §8.4 uses.
