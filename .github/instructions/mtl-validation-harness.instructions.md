---
description: "Use when editing the validation harness — tests/validation/conftest.py fixtures, configs/ YAML schema and gen_config.py, common/ host control (nicctl, host_setup, integrity runners, platform info), create_pcap_file/ capture, and compliance/ EBU client. Covers fixture scoping rules, VF pool lifecycle, the mfd remote-execution contract, and cleanup discipline."
name: "MTL Validation — Harness & Host Control"
applyTo: "tests/validation/conftest.py,tests/validation/configs/**,tests/validation/common/**,tests/validation/create_pcap_file/**,tests/validation/compliance/**"
---

# Editing the validation harness

Architecture: [doc/validation-design.md](../../doc/validation-design.md) §4, §6, §7, §8.
`mfd` = the Intel `mfd-*` test library family that abstracts host access.

## The mfd contract — this is what makes tests remote-proof

Every command must go through the host object. One `subprocess` call or
hardcoded interface name silently breaks the moment the host is not local.

```python
host.connection.execute_command(cmd, shell=True, expected_return_codes=None)
host.connection.start_process(cmd, output_file=log_path)   # daemons only
host.connection.path(base, "bin", "RxTxApp")               # remote-safe join
host.network_interfaces[i].name / .pci_address
host.topology.role            # "sut" | "client"
host.vfs / host.vfs_r         # session VF pool
```

`connection.path()` is the correct primitive for a path that resolves on the
DUT, but most existing code still uses `os.path.join`, which is safe only
while every host is Linux. Prefer `connection.path()` in new code; do not
churn existing call sites for it.

Prefer `expected_return_codes=None` plus an explicit `return_code` check
over letting mfd raise, whenever a non-zero exit is a legitimate outcome.

## Fixture rules

- **Scope is a contract.** Anything touching shared hardware state (VF pool,
  ramdisk, MtlManager, ldconfig) is `session`. Anything a test can dirty
  (interfaces, media staging, capture, output files) is `function`. Do not
  widen a function-scoped fixture to session to "speed things up" — that is
  how cross-test contamination gets introduced.
- **Every fixture that creates state removes it** in its own teardown, and
  removes *only what it created*. `InterfaceSetup.cleanup()` is the model:
  it releases per-test VFs and rebinds PFs but leaves the session pool alone.
- **Teardown must not assert.** Assertions in teardown surface as ERROR, not
  FAILED, and mask the real result. The one deliberate exception is the
  compliance "never dispatched" safety net, which checks that a requested
  oracle actually ran.
- **Autouse fixtures are host hygiene only** (reaping stray daemons, killing
  stale VFIO holders, wiping hugepages, registering libs). No test policy.

## VF pool lifecycle

The session pool (`nic_port_list` → `host.vfs`, `host.vfs_r`) is created
once and reused, because repeated SR-IOV teardown is slow and can hang on a
held VFIO group. `Nicctl.create_vfs()` must therefore stay **idempotent** —
return the existing VFs when the requested count is already satisfied.

Per-test allocation goes through `InterfaceSetup`. When adding a new
interface flavour, add a method there rather than teaching tests to call
`Nicctl` directly.

**PF mode + capture requires distinct IOMMU groups.**
`InterfaceSetup._check_pf_not_capture_group()` compares the requested PF's
sysfs IOMMU group against the capture NIC's and calls `pytest.fail()` on a
match — there is no fallback to another PF, and it deliberately fails rather
than skips, because the test asserted the host has the hardware.

## Clock discipline

Reap `ptp4l`/`phc2sys` **by process name**, never by the handle returned
from `start_process`. The SSH/sudo wrapper is not the daemon; killing the
wrapper leaks a clock owner into later NIC reconfiguration and can trigger
use-after-free in the ice driver during SR-IOV teardown.

`capture_cfg.phc_sync` (local TAI discipline) and `@pytest.mark.ptp`
(slave-only `ptp4l`) must never both drive the same PHC — marked PTP tests
suppress the local helper automatically. Note `ptp_sync` early-returns when
`capture_cfg.enable` is falsy, so the marker is a no-op with capture off.

## Config schema changes

Adding a key to `test_config.yaml` or `topology_config.yaml` means updating,
in the same change:

1. the reader in `conftest.py`, with a default — an old config must still load;
2. `configs/gen_config.py`;
3. `configs/examples/`;
4. `configs/README.md`;
5. [doc/validation-design.md](../../doc/validation-design.md) §4.1 if the key
   is architecturally significant.

## Capture and compliance split

`create_pcap_file/netsniff.py` is **capture only** — process control, filter,
packet sizing, nanosecond pcap with hardware RX timestamps. It holds no
EBU/compliance state. The verdict lives in `mtl_engine/pcap_compliance.py`;
the HTTP client lives in `compliance/`.

## Before handing back

```bash
cd "$(git rev-parse --show-toplevel)/tests/validation" && \
  ./venv/bin/python3 -m pytest tests/single --collect-only -q | tail -1
cd "$(git rev-parse --show-toplevel)" && ./format-coding.sh
```

Collection needs no config flags and must not change count unintentionally.
A harness change is not proven by collection alone — run at least one real
test that exercises the fixture you touched.
