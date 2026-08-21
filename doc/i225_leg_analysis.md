# Why the i225 smoke leg was red, and what it actually failed on

Written 2026-08-21, the day the leg first reached a runner. It is here because
the leg spent two days looking broken while nothing about it was, and the way to
tell those apart is not obvious from the Actions UI.

## Summary

The `i225` leg of `smoke-tests-bare-metal` had never run. Not "run and failed" —
never been handed to a machine. It was created on every push, sat `queued`, and
was cancelled by the next push. On 2026-08-21 at 09:40 UTC, once **mtl-runner-12**
came online advertising `self-hosted, Linux, X64, dpdk, i225`, the leg was
dispatched for the first time and failed 12 seconds in, on a host prerequisite:
that machine's `python3` has no `ensurepip`, so the acceptance virtualenv could
not be built.

Neither of those is a fault in the leg, the suite, or the low-bandwidth subset.
The design was separately confirmed to be right by running the same job on real
hardware (below).

## What the evidence was

Every i225 job record, read from the jobs API rather than the UI:

| run | i225 leg | `runner_name` | outcome |
|---|---|---|---|
| 32418031077 … 32422101417 | never created | — | run cancelled, or the gate expired, before the matrix expanded |
| 32423288237 | queued 65 min | *empty* | cancelled by the next push |
| 32430388914 | queued 62 min | *empty* | cancelled by the next push |
| 32434827867 | queued 23 min | *empty* | cancelled by the next push |
| 32441346791 | queued 4 h 48 m, then ran | `mtl-runner-12` | **failure** in 12 s |

`runner_name` is the tell, and it is the fact this analysis turns on. A job that
is queued because the fleet is busy and a job that is queued because *no machine
advertises its label* are the same grey dot in the Actions UI, and both were
reported to us as "the i225 leg is failing". They are distinguishable in exactly
one place: a job that has been handed to a host has a `runner_name`, and a job
that has not, has an empty one. Every i225 leg before 09:40 had an empty one.

`.github/scripts/ci/watch-run.sh` exists because of this. It polls the API,
prints `queued Nm, no runner yet` for the fleet-availability case, and turns a
finished run into the job, the failed step and the error line. `task ci:watch-run`
and the `ci_watch_run` MCP tool are the two front doors to it.

## What the first real run failed on

```text
run-smoke-tests (i225, smoke-low-bandwidth, 45, 35, 100, 1, true)  [mtl-runner-12]
  failed step: preparation: Ensure the acceptance virtualenv
  This host's python3 cannot create virtualenvs (no ensurepip).
```

`python3-venv` was not installed on the new host. This is a documented host
prerequisite — "Host image prerequisites the jobs check but cannot fix" in
[`ci_runner_setup.md`](ci_runner_setup.md) named it before the run happened — and
the failure is the check for it working as intended: 12 seconds, one sentence, the
package to install. It is not a diagnosis anyone had to work for.

Two things follow, and both were done:

- The step no longer has to be a dead end. `virtualenv` and `uv` build the same
  virtualenv without `ensurepip`, because they carry their own pip, so
  `pytest-setup.sh` uses whichever of the three the host has and only names the
  apt package when it has none. Using a tool the host already has is not a job
  repairing host state; nothing is installed either way.
- `python3-venv` still belongs in the host image, and on a machine with none of
  the three the leg still stops there. That is the fleet owner's one-liner:
  `sudo apt-get install -y python3-venv`.

**Everything before that step passed on the new host**, which is the useful part
of an otherwise wasted run: checkout, `validate-host` — so the `i225` label
resolved to a card actually present at that PCI address, the datapath was chosen
for a two-port card, and the build artifacts restored — and the process cleanup.
The parts specific to a NIC nobody had ever run in CI were fine.

## What the leg's own design is worth

The leg was proven correct on real hardware before it ever ran in CI, on
`mtldev4`, which has the I225-LM at `a7:00.0` and is not a registered runner:

```console
.github/ci-local/run-job.sh smoke-tests --nic i225 --runner host
```

Two runs on 2026-08-20 (at `ef194788` and `3580d366`) both ended `11 passed,
3 skipped, 1181 deselected` in about 22 minutes, `result: PASS`. So the suite
selection (`smoke-low-bandwidth`), `test_time=100`, `no_capture=1` and the
45-minute job / 35-minute run caps are right, and were right while the leg was
red.

## The three legs beside it were red for a different reason

Worth separating, because they were the actual red X on the run and were being
read as the i225 result. All three E8xx legs — `mtl-runner-3`, `-10`, `-11` — got
hosts, ran for 65 seconds and failed at `preparation: Verify the compliance
analyser`:

```text
EBU_IP is unset in /etc/mtl-ci/runner.env; tests will run without a compliance verdict.
```

That check was mine, and it was wrong: its own comment said the condition was
"not fatal on its own" and it exited 1 anyway, so a check added to stop a *late*
failure inside a test became an *earlier* failure of its own, on every capture leg
of a fleet where no host has EBU LIST deployed yet. It now distinguishes absence
from misconfiguration: no `EBU_IP` announces the missing verdict on the run
summary and lets the leg run, an `EBU_IP` that is set but unusable still fails,
and a host that has provisioned an analyser sets
`MTL_CI_REQUIRE_COMPLIANCE=1` so that losing it is a failure again.

The i225 leg never hit this one — it skips the step, because
`matrix.no_capture != '1'` gates it — but it is the reason the run it belongs to
was red.

## Reading list for the next person

- Which host carries a label: `runner_name` in the jobs API of the last run that
  used it. There is no other way; the NIC-labelled runners are registered at the
  organisation level, so `gh api repos/.../actions/runners` never lists them.
- A queued bare-metal job: `task ci:watch-run -- --pr <n>`, and believe
  `no runner yet`.
- A leg you cannot get a runner for at all: `.github/ci-local/run-job.sh <job>
  --nic <label> --runner host` on a machine that has the card.
- What a host still needs: `task ci:pytest-setup -- verify`,
  `task ci:media-assets -- verify`, `task ci:ebu-list -- status`,
  `task ebpf:check`.
