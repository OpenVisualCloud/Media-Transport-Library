# MTL Acceptance Tests — Setup and Run

Everything needed to get `tests/acceptance/` pytest running, and to fix it
when it breaks. For *how the framework is built* see
[acceptance-design.md](acceptance-design.md); for MTL itself see
[build.md](build.md) and [run.md](run.md).

## The `.local_install` rule — read this first

Pytest resolves **every** binary it launches under `.local_install/`, a tree
separate from the `build/` + `/usr/local` install that gtest/KahawaiTest
uses. This is hardcoded in `tests/acceptance/mtl_engine/const.py`
(`PREFIX = ".local_install"`) and is the single most common reason a host
"with MTL built" still cannot run a test.

| Pytest launches | Resolved path |
|---|---|
| RxTxApp | `.local_install/mtl/bin/RxTxApp` |
| MtlManager | `.local_install/mtl/bin/MtlManager` |
| FFmpeg / ffprobe | `.local_install/ffmpeg/bin/` |
| GStreamer plugin | `.local_install/gstreamer/gstreamer-1.0/` |

A working `build/manager/MtlManager` or `tests/tools/RxTxApp/build/RxTxApp`
does **not** satisfy pytest. Symptom when the tree is missing: the
`mtl_manager` fixture errors with `Failed to start MtlManager on host` while
gtest runs fine.

## Prerequisites

- Python 3.9+ and **root access** — the framework connects over SSH to
  `root@` even on a single host, because NIC binding and hugepage setup
  need it. There is no unprivileged mode.
- An Intel E810/E830/E835 NIC. VFs are created automatically; do not create
  them by hand.
- Test media, on NFS or generated locally (see below).
- FFmpeg/GStreamer only if you run the integration suites — they are built
  into `.local_install`, not taken from the system packages.

## Recommended: automated setup

`.github/scripts/acceptance_setup.sh` is the single entry point. It composes
`acceptance_setup_base.sh` (apt, ICE driver, DPDK, MTL + plugins into
`.local_install`, hugepages, CPU governor) and `setup_acceptance.sh` (NFS
media, localhost root SSH, venv, generated configs). No agent or MCP client
is required.

```bash
./.github/scripts/acceptance_setup.sh status   # read-only probe, changes nothing
./.github/scripts/acceptance_setup.sh setup    # interactive: asks PF, NFS source, EBU
```

Non-interactive, e.g. for CI:

```bash
./.github/scripts/acceptance_setup.sh setup --auto \
    --pf-bdf=0000:c9:00.0 \
    --nfs-source=10.0.0.5:/mnt/NFS/mtl_assets/media
```

It prints a ready-to-run pytest command when it finishes. `-h` lists all
flags (`--base-only`, `--pytest-only`, `--check-only`, `--ffmpeg`/
`--no-ffmpeg`, `--gstreamer`/`--no-gstreamer`, `--ebu-ip`/`--ebu-user` for
compliance capture, `--nr-hugepages`, `--build-mode`). Both phases are
idempotent — re-running on a prepared host is safe and takes seconds.

Cold setup takes roughly 7–10 minutes. Do not interrupt and retry during the
first run.

## Manual setup

Only needed to debug or customise one stage; the script above automates all
of it.

### Python environment

```bash
cd tests/acceptance
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
pip install -r common/integrity/requirements.txt
```

### Configuration

`configs/gen_config.py` generates both files; hand-edit only to adjust.

`configs/topology_config.yaml` — hosts, connections, and NICs:

```yaml
hosts:
  - name: host
    instantiate: true
    role: sut                    # "sut" or "client" (dual-host only)
    network_interfaces:
      - pci_device: 8086:1592    # vendor:device, NOT a BDF
        interface_index: 0
    connections:
      - ip_address: 127.0.0.1
        connection_type: SSHConnection
        connection_options:
          port: 22
          username: root         # must be root
          password: ''           # required key even when using a key file
          key_path: /home/<you>/.ssh/id_rsa
```

`pci_device` takes the **vendor:device** ID (`lspci -nn | grep Ethernet`), not
the `0000:18:00.0` bus address. Supplying a BDF raises
`Incorrect format ... PCIDevice`.

`configs/test_config.yaml` — paths and per-run behaviour:

```yaml
build: /home/<you>/Media-Transport-Library/
mtl_path: /home/<you>/Media-Transport-Library/
media_path: /mnt/media
interface_type: VF               # VF | PF | VFxPF | 2VFxPF | 3VFxPF
capture_cfg:
  enable: false                  # true enables EBU LIST compliance capture
  pcap_dir: /mnt/ramdisk/pcap
  capture_time: 5
  sniff_pci_device: 8086:1592    # vendor:device of the capture NIC
  phc_sync: true                 # discipline the local PHC to system TAI
ebu_server:                      # omit to leave compliance disabled
  host: 10.0.0.9
  user: <user>
  password: <password>
ramdisk:
  media: {mountpoint: /mnt/ramdisk/media, size_gib: 32}
  pcap:  {mountpoint: /mnt/ramdisk/pcap,  size_gib: 768}
```

Capture needs a **second** NIC port in a different IOMMU group from the one
carrying traffic. Without `ebu_server`, tests still run — the compliance
verdict is simply skipped.

### Test media

Production hosts mount an NFS share at `media_path`. Tests **skip** rather
than fail when an asset is missing. To generate a local set instead:

```bash
cd tests/acceptance/common
./gen_frames.sh
```

This needs an FFmpeg build with text filters; `No such filter: 'drawtext'`
means the system FFmpeg is too minimal (`sudo apt install ffmpeg`).

The ST2110-40 GStreamer suites stage inputs and outputs on
`ramdisk.media.mountpoint` — mount and size that ramdisk or they spill
into `/tmp`.

## Running tests

Run from `tests/acceptance/`, as root, using the venv interpreter:

```bash
cd tests/acceptance
sudo -E ./venv/bin/python3 -m pytest \
    --topology_config=configs/topology_config.yaml \
    --test_config=configs/test_config.yaml \
    -m smoke -v
```

`sudo python3` uses the *system* interpreter and fails with
`No module named pytest`. The config arguments are registered by the local
`conftest.py`, so running from another directory fails with
`unrecognized arguments`.

### Selecting tests

```bash
$PY -m smoke -v                                   # by marker
$PY tests/single/st20p -v                         # by folder
$PY tests/single/st40p -k multicast -v            # by substring
$PY "tests/single/st20p/test_fps.py::test_st20p_fps[|fps = p60|-Penguin_1080p-|application = rxtxapp|]"
$PY <selector> --collect-only -q                  # dry run
```

Quote selectors containing `[` `]` or `|`.

### Markers

Suite markers:

| Marker | Use |
|---|---|
| `smoke` | Smallest set that proves the host works |
| `nightly` | Bulk single-host coverage |
| `performance` | Dual-host capacity sweeps, long and hardware-bound |
| `base_performance` | Added at collection to a test ID that holds `1080p` and `59fps` |
| `dual` | Requires two hosts |
| `ptp` | Uses MTL's internal PTP (`phc2sys` suppressed) |

Neither performance marker selects a `tests/single/` test. Select the
`tests/single/performance/` modules by path.

Descriptive markers: `verified` (see below),
`refactored` (uses the Application-based harness),
`tx_side`/`rx_side`/`tx_and_rx` (which side a test validates, and therefore
whether a compliance verdict is meaningful), and `allow_wide_compliance`
(accept ST 2110-21 *wide* instead of failing).

`tests/` contains `single/` and `dual/` only.

### Useful options

| Option | Effect |
|---|---|
| `--time N` | Seconds each test runs (default 15) |
| `--keep all\|failed\|none` | Retain result media files |
| `--dmesg clear\|keep` | Whether to clear the kernel ring before each test |
| `--media`, `--build`, `--nic`, `--dma` | Override the config values |
| `--num_sessions N`, `--sch_quota N` | Performance sweeps only |

### Output

Logs land in `tests/acceptance/logs/<UTC-timestamp>/` (root-owned) with a
`logs/latest` symlink — per-test logs plus MtlManager and application output
and the rendered JSON configs.

```bash
sudo grep -E "EAL|hugepage|VF|RxTxApp|Traceback|err:" logs/latest/*.log | head -40
```

An HTML run report:

```bash
$PY -m smoke --template=html/index.html --report=report.html
```

A performance report across timestamped log directories:

```bash
./venv/bin/python3 common/generate_report.py logs/
```

## Troubleshooting

Start with `./.github/scripts/acceptance_setup.sh status` — it checks kernel,
hugepages, governor, ICE driver, NIC PFs, `.local_install` build state, NFS,
venv, and configs in one read-only pass, and usually names the broken stage
directly. Re-run `setup --base-only` or `setup --pytest-only` to repair it.

| Symptom | Cause and fix |
|---|---|
| `No module named pytest` | Used `sudo python3`. Use `sudo -E ./venv/bin/python3`. |
| `unrecognized arguments: --topology_config` | Not in `tests/acceptance/`. |
| `Failed to start MtlManager on host` | `.local_install` tree missing — re-run setup. A system-wide `build/` install does not count. |
| `RxTxApp: command not found` | Same cause: build into `.local_install`, not `tests/tools/RxTxApp/build/`. |
| `error while loading shared libraries: librte_*.so` | Stale linker cache. `sudo ldconfig`, then check `ldd .local_install/mtl/bin/RxTxApp`. |
| `Media file not present ...` (SKIPPED) | NFS unmounted or empty. Re-run setup with the NFS source. |
| `Incorrect format ... PCIDevice` / `sniff_pci_device ... not found` | A BDF was used where `vendor:device` is required. |
| `SSHConnection.__init__() missing ... 'password'` | Add `password: ''` to the topology connection. |
| `Permission denied (publickey)` for `root@127.0.0.1` | Public key not in `/root/.ssh/authorized_keys`. |
| `ValueError: q must be exactly 160, 224, or 256 bits long` | DSA key. `ssh-keygen -t rsa -b 2048 -f ~/.ssh/id_rsa && ssh-copy-id root@localhost`. |
| `netsniff-ng: command not found` | `sudo apt install -y netsniff-ng`. |
| `EBU server configuration not found`, test still PASSED | Data path passed, compliance skipped. Configure `ebu_server` + `capture_cfg`. |
| Test hangs past `--time` + ~30 s | Stale process: `sudo pkill -9 RxTxApp MtlManager ffmpeg gst-launch-1.0`. |
| EAL hugepage or VF binding errors | Hugepages exhausted, or VFs not bound to `vfio-pci`. Re-run setup. |
| `RxTxApp` segfault in `iavf_tm_node_add` | Stock kernel `ice` loaded instead of the patched out-of-tree build. Re-run setup. |

A segfault anywhere *other* than `iavf_tm_node_add` is a real MTL/DPDK bug —
capture a backtrace (`coredumpctl gdb RxTxApp`) and report it rather than
working around it.

The exhaustive symptom table, kept in sync with the engine source, is in
[mtl-acceptance-tests.instructions.md](../.github/instructions/mtl-acceptance-tests.instructions.md).

## Verified test criteria

`@pytest.mark.verified` is a claim about evidence, not just a passing run. A
test may carry it only when all three hold:

1. **Telemetry matches inputs.** If the input is 60 fps, the logs and
   collected metrics report 60 fps within a documented tolerance.
   Configuration flags (e.g. `rss_enabled`) in the logs agree with the test
   inputs.
2. **The stated goal is actually checked.** The test carries a
   Doxygen-style preamble giving objective, steps, expected outcome, and
   metrics, and enables every check that is feasible for it.
3. **It passes in CI on a framework-provisioned host** — no manual setup —
   and the CI artifacts include the logs and metrics proving point 1.

## See also

- [acceptance-design.md](acceptance-design.md) — architecture: modules,
  adapters, fixtures, oracles, clock model
- Agent instructions for changing the framework:
  [running tests](../.github/instructions/mtl-acceptance-tests.instructions.md),
  [authoring tests](../.github/instructions/mtl-acceptance-authoring.instructions.md),
  [engine](../.github/instructions/mtl-acceptance-engine.instructions.md),
  [harness](../.github/instructions/mtl-acceptance-harness.instructions.md)
- [build.md](build.md), [run.md](run.md), [configuration_guide.md](configuration_guide.md)
