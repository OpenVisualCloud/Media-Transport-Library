# Run status

| | |
|---|---|
| Status | The ledger of the autonomous implementation run ([autonomous-run.md](autonomous-run.md)). The orchestrator keeps it current and commits it by path while no lead runs (autonomous-run.md §5); a lead commits its own §3 and §4 rows, and a pending §2 row, with its first commit |
| Branch | `unified-api-impl` on `sakoram` |
| Base commit | (the orchestrator records `git rev-parse HEAD` here when R0 starts) |

On every resume: read [autonomous-run.md](autonomous-run.md), this file, the current milestone's
ledger and `git log --oneline -20`, then continue (autonomous-run.md §7).

## 1. The host

The guard check and the `status_report` of `mtl-system-admin` before R0, every later change to the
host, and the known-red list of L0 (autonomous-run.md §1): the legacy cases red or skipped at the
base commit.

| Date | Check | Result |
|---|---|---|
| 2026-10-16 | preparation: tests at the doc-set commit | `./build.sh` green after `build/` was moved back to the `/usr/local` prefix; `./build.sh unit` 959 passed, 1 skipped; KahawaiTest `St20p*` 44/44 on the E830 (`0000:15:01.0`/`.1`) and on the E810 (`0000:c9:01.0`/`.1`) |
| 2026-10-16 | preparation: host | patched ICE `Kahawai_2.6.7`; MtlManager of this tree running; the stale `st41_api.h` removed from `/usr/local/include/mtl` and `.local_install/mtl/include/mtl`; both install trees at the doc-set commit (`build-both.sh`) |

## 2. Milestones

States: todo, in progress, exit review, done, done with exceptions (autonomous-run.md §4). The agent column holds the ID of the running lead, for SendMessage on
resume. A milestone starts when the one in Needs is done or done with exceptions.

| Milestone | Ledger | Needs | State | Lead agent | Exit commit | Exit evidence |
|---|---|---|---|---|---|---|
| R0 (re-pin to the new base, autonomous-run.md §1) | this file | — | todo | | | |
| L0 (the legacy baseline, autonomous-run.md §1) | this file, §1 | R0 | todo | | | |
| MS1 | [ms1-status.md](ms1-status.md) | L0 | todo | | | |
| MS2a | ms2a-status.md | MS1 | todo | | | |
| MS2b | ms2b-status.md | MS2a | todo | | | |
| MS3 | ms3-status.md | MS2b | todo | | | |
| MS4a1 | ms4a1-status.md | MS3 | todo | | | |
| MS4a2 | ms4a2-status.md | MS3 (run after MS4a1, plan §1.2) | todo | | | |
| MS4b | ms4b-status.md | MS4a2 | todo | | | |
| MS5 | ms5-status.md | MS4b | todo | | | |
| MS6 | ms6-status.md | MS5 | todo | | | |
| MS7 | ms7-status.md | MS6 | todo | | | |
| Kubernetes track (EK1–EK19) | its rows in the ledgers of MS1–MS3 (autonomous-run.md §2) | beside MS1–MS3 | todo | | | |

## 3. Decisions taken in the run

Every question answered and every plan bug fixed by autonomous-run.md §4, for the maintainer to
confirm. One row each; the D-row holds the decision itself.

| Date | Milestone, task | Question or bug | D-row | Evidence (test, model, review file) |
|---|---|---|---|---|

## 4. Blocked outside the run

Items that need a person or a release outside this machine (autonomous-run.md §3), and what the
run prepared for each.

| Item | Needed by | Prepared | What the maintainer must do |
|---|---|---|---|

## 5. Final report

Written at the end of the run: what is implemented, the test evidence per milestone, the
decisions of §3 to confirm, the items of §4, and the known gaps.
