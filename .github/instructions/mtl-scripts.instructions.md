---
name: 'MTL Scripts and CI'
description: 'Rules for user scripts in script/, for CI scripts, workflows and actions, and the table that says which layer configures which host setting. Read it before you add or change a script, a workflow step, or a test fixture that changes the host.'
applyTo: 'script/**,.github/scripts/**,.github/workflows/**,.github/actions/**,Taskfile.yml'
---

# MTL Scripts and CI

The repository has two kinds of script. Each kind has its own rules. Do not
mix them.

## User scripts: `script/`

A person runs these scripts by hand, so each one explains itself.

- Start with `#!/bin/bash`, the SPDX line, and `set -euo pipefail`.
- Set `script_name` and `script_folder`, then source `common.sh`.
- Use the `common.sh` helpers. Do not write a local copy of one:
  - `log_info`, `log_success`, `log_warning`, `log_error` for each message.
    No `echo ... >&2`.
  - `as_root` for each privileged command, `require_root` when the whole
    command needs root. No raw `sudo`.
  - `require_commands`, `require_pkg_config` to check prerequisites, with the
    package that supplies each one.
  - `register_plugin`, `origin_rpath`, `download_file` and the other build
    helpers.
- Give a `show_help()` that starts with `Usage: <script name>`. It names each
  command and option, the required packages, and the environment variables
  that the script reads. `-h` and `--help` print it and exit 0. An unknown
  option exits non-zero.
- Put the work in `main()`. End with the sourced guard, so that a test can
  source the script without running it:

  <!-- markdownlint-disable MD010 -->
  ```bash
  (return 0 2>/dev/null) && sourced=1 || sourced=0
  if [ "${sourced}" -eq 0 ]; then
  	main "$@"
  fi
  ```
  <!-- markdownlint-enable MD010 -->

- Work from any current directory. Find files from `script_folder` or
  `REPO_DIR`.

`.github/scripts/ci/lint/test-scripts.sh` enforces this structure for each
`script/*.sh` and for `.github/scripts/setup_environment.sh`. Copy the
structure of `script/dma_setup.sh` or `script/nicctl.sh` for a new script.

## CI scripts: `.github/scripts/ci/`, workflows, actions, `ci:` tasks

Only CI runs these. They must be as simple as possible.

- Keep `#!/usr/bin/env bash`, the SPDX line, and `set -euo pipefail`. Nothing
  more is required.
- No `-h`/`--help`, no help text, no argument checks, no option parsing. A
  `case` on `$1` ends with the one-line fallback
  `*) echo "Usage: $0 {a|b}" >&2; exit 2 ;;` and nothing more.
- No variables that exist only to point a test at a fixture. No fallback for
  a host that is not set up: a missing host setting fails the step.
- No long explanations or recovery hints in messages. One line that says what
  is wrong is enough; the documentation says how to fix it.
- A comment says why, in one or two lines. Do not explain what the code does.
- A workflow `run:` calls one `ci:` task of `Taskfile.yml`, so that a person
  runs the same command. A few lines of shell stay in the task; more go in a
  script in `.github/scripts/ci/<family>/` that the task calls. Do not add a
  script that only wraps one command.
- A CI step that needs a user script calls it (`script/dma_setup.sh reset`,
  `script/nicctl.sh`). Do not copy its logic into CI.
- When you fix a CI script, change only what is broken. Removing code is
  better than adding code.

## The other scripts in `.github/scripts/`

- `gtest.sh` is the test harness that CI runs. The CI rules apply to it, and
  it changes only what one test needs (the table below).
- `setup_environment.sh`, `acceptance_setup.sh`, `acceptance_setup_base.sh`
  and `setup_acceptance.sh` set up a host for a person. They are host-layer
  tools, and CI does not call them to change a runner. `setup_environment.sh`
  follows the user script rules; `ci:build-dependencies` calls it only to
  build the cached dependencies, with the host settings off.
- `lib/` holds the helpers of the acceptance setup scripts and of the
  `mtl-acceptance-setup` MCP server.

## Who configures what

Each host setting belongs to one layer. A layer does not do the work of
another layer.

| Layer | When | Who does it |
| --- | --- | --- |
| **Host** | Once, and again after each boot | Not CI. On the fleet: the runner watchdog. By hand: `.github/scripts/setup_environment.sh`, `.github/scripts/acceptance_setup.sh`, the `mtl-system-setup` MCP server. |
| **Test cycle** | At the start of each CI job, before the first test | CI: the `validate-host` action, and the `ci:` tasks that each test job runs before its tests. |
| **Test** | Before one test or suite, undone after it | The test harness: `.github/scripts/gtest.sh`, the pytest fixtures, `tests/integration_tests/noctx/run.sh`. |

| Setting | Layer | Where |
| --- | --- | --- |
| Packages, build tools, `python3-venv` | Host | watchdog, `setup_environment.sh`, `script/install_dependencies.sh` |
| Kernel release and headers | Host | watchdog |
| Kernel command line: `intel_iommu=on iommu=pt`, hugepage size | Host | watchdog, GRUB |
| CPU isolation on the command-line (`isolcpus`, `nohz_full`, `rcu_nocbs`), when a host needs it | Host | GRUB |
| Hugepage reservation per NUMA node, the `/dev/hugepages` mount | Host | watchdog, `acceptance_setup.sh`, `hugepages_set` MCP tool |
| CPU governor `performance` | Host | watchdog, `acceptance_setup.sh` |
| Turbo | Host | BIOS; the performance fixture warns when it is off |
| `vfio` and `vfio-pci` loaded at boot, `/dev/vfio` udev rule, memlock limit | Host | watchdog |
| `net.core.rmem_max` for the kernel socket datapath | Host | watchdog, `acceptance_setup.sh` |
| NFS media at `/mnt/media` | Host | watchdog, `acceptance_setup.sh` |
| SSH key and the login of the suite to the host itself | Host | watchdog, `acceptance_setup.sh` |
| `/etc/mtl-ci/runner.env`, `/etc/mtl-ci/shadow-host` | Host | watchdog |
| TAI offset, PTP, EBU LIST server | Host | watchdog |
| DSA or CBDMA enabled | Host | BIOS |
| Stop the processes and release the CPUs that an earlier job left | Test cycle | `task cleanup` |
| Restore and check the DPDK, MTL, plugin and ICE builds of this commit | Test cycle | `validate-host` |
| Unload the ICE driver, load the ICE module under test (this removes all VFs) | Test cycle | `ci:activate-ice` |
| Bind the DMA devices to `vfio-pci`, because the DPDK under test can change | Test cycle | `script/dma_setup.sh reset` |
| Job environment: `PATH`, `PKG_CONFIG_PATH`, `GST_PLUGIN_PATH`, `KAHAWAI_CFG_PATH` | Test cycle | `ci:configure-host -- environment` |
| Acceptance virtualenv cache, free disk space, generated test configuration | Test cycle | `ci:pytest-setup` steps of the test job |
| Same tree, ICE module and DMA binding on the shadow host | Test cycle | `ci:configure-host -- shadow-*` |
| Number of VFs on a PF, and their bind to `vfio-pci` | Test | `gtest.sh` `create_vfs`, `Nicctl.create_vfs` |
| A PF on `vfio-pci` for a PF test, or on its kernel driver for a kernel socket test | Test | pytest fixtures, `script/nicctl.sh` |
| Owner of the `/dev/vfio` nodes for the test account | Test | `Nicctl.grant_vfio_access` |
| C-state limit (`/dev/cpu_dma_latency`), for a test that needs it | Test | the fixture of that test (no test needs it yet) |
| Cores offline, for the turbo of a performance test | Test | `optimize_cpu_cores_for_turbo`, `restore_cpu_cores` |
| CPU isolation for NoCtx | Test | `tests/tools/isolate/isolate.sh` |
| MtlManager, `ptp4l`/`phc2sys`, interface addresses, packet capture | Test | pytest fixtures |

Rules that follow from the table:

- CI and the tests never set a host setting. When a missing host setting
  gives a misleading error (no hugepages: `Cannot get hugepage information`),
  check it and fail with one line that names the setting. Do not fix it.
- Do not add a workflow that installs onto a runner or changes its boot
  configuration. Fleet provisioning is outside this repository.
- A test-cycle step runs on each job. It must be safe to run again, and it
  must not depend on what the job before it did.
- A test step undoes its change when the test ends, also when the test fails:
  remove the VFs that it made, restore the CPUs, stop the daemons.
- Before you add a host change, find its row in the table. If it has no row,
  add one here in the same change.
