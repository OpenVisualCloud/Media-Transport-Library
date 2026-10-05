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
| H1a | todo | | | | |
| H1b | todo | | | | |
| C0 | todo | | | | |
| C1a | todo | | | | |
| C1b | todo | | | | |
| A1 | todo | | | | |
| C2 | todo | | | | |
| E1 | todo | | | | |
| B1 | todo | | | | |
| B2 | todo | | | | |
| A2a | todo | | | | |
| CI1 | todo | | | | |
| I1 | todo | | | | |
| R1 | todo | | | | |
| P1 | todo | | | | |
| A2b (stretch) | todo | | | | |
| B3 (stretch) | todo | | | | |
| X (stretch) | todo | | | | |

## 2. Gates

| Day | Gate | Result |
|---|---|---|
| 5 | C0 approved by the maintainer; H1a and H1b committed and approved | |
| 10 | C1a, C1b and A1 committed and approved | |
| 15 | first unified frame on a VF (I1 cross TX) | |
| 17 | feature freeze: fixes only | |

## 3. Questions for the maintainer

Batch them; ask at a checkpoint, not between. Each question: the options, a recommendation, and
an answer line.

## 4. Measurements

S0 baseline (RxTxApp `--tasklet_time`, avg and max per pacing class), and every later measurement
that a gate or a budget of implementation-plan.md §8.4 uses.
