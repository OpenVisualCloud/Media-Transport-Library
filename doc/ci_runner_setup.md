# Self-hosted runner setup for MTL CI

MTL's hardware jobs run on self-hosted bare-metal runners that own the NIC they
test. This page describes the contract between those hosts and the workflows.

## Jobs install nothing

A CI job never installs onto a runner. `apt`, `pip` and `venv` all mutate a host
that other jobs share, they race with those jobs, and they hide drift in the host
image: a runner that is missing a package looks healthy for months because every
job silently repairs it, until one job repairs it differently.

So the host image carries the dependencies and the job only checks for them:

| Job step                            | What it does                                    |
| ----------------------------------- | ----------------------------------------------- |
| `task ci:pytest-setup -- verify`    | Requires `tests/acceptance/.venv`, prints the pytest version |
| `task ci:validation -- verify-dependencies` | Requires the apt build packages           |
| `task ci:validation -- verify-pipenv`       | Requires an existing pipenv environment   |

A missing dependency fails the job with the command that provisions it. The
provisioning commands still exist and are meant to be run by hand on the host:

```bash
task ci:pytest-setup -- install         # tests/acceptance/.venv
task ci:validation -- install-dependencies
task ci:validation -- install-pipenv
```

## Host configuration instead of repository secrets

Lab facts live on the runner, in `/etc/mtl-ci/runner.env`, not in GitHub
secrets. See [`.github/ci-local/runner.env.example`](../.github/ci-local/runner.env.example)
for the keys and their defaults. Point `MTL_CI_RUNNER_ENV` elsewhere to test the
mechanism locally.

The shadow/SUT host pair used by `perf-pytest.yml` and the optional shadow sync
in `custom-pytest.yml` reads `/etc/mtl-ci/shadow-host`, a file defining `IP` and
`USER`.

Only credentials for genuinely external services remain repository secrets:
`COVERITY_EMAIL`, `COVERITY_TOKEN`, and the built-in `GITHUB_TOKEN`.

## NIC labels

A runner label is a claim about hardware, so it is verified rather than trusted:
`task ci:pytest-setup -- pci` resolves the label against `lspci` and fails with
the list of Intel network devices actually present when the card is not there.

| Label  | Device IDs (8086:)        | Notes                                     |
| ------ | ------------------------- | ----------------------------------------- |
| `e810` | 1592, 1593, 159b          | ICE                                       |
| `e830` | 12d2, 12d3                | ICE                                       |
| `e825` | 579d, 579e                | ICE                                       |
| `e835` | 1249, 124a                | ICE                                       |
| `i225` | 15f2, 15f3, 15f8, 0d9f, 3100 | IGC, 2.5 GbE, no SR-IOV                |
| `i226` | 125b, 125c, 125d, 3102    | IGC, 2.5 GbE, no SR-IOV                   |

The i225/i226 cards have no virtual functions, so tests on them bind the PF
(`--interface_type PF`), and 2.5 Gbps of link means only the `low_bandwidth`
subset of the suite fits: ST 2110-22, ST 2110-20 at up to 1080p29, ST 2110-30 and
ST 2110-40. The `smoke-tests-bare-metal` workflow runs that subset in its own
`run-smoke-tests-low-bandwidth` job, concurrently with the E8xx legs.
