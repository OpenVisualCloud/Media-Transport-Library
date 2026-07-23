---
description: "Use when the user asks to run, collect, debug, or diagnose pytest cases under tests/validation/single/ (st20p, st22p, st30p, st40p, st41, ffmpeg, gstreamer, dma, ptp, rss_mode, rx_timing, udp, virtio_user, xdp, kernel_socket); running by marker (-m smoke/-m nightly); investigating logs under tests/validation/logs/. For host setup (build, hugepages, NFS, configs): call `.github/scripts/validation_setup.sh status`/`setup` (interactive, prompts for NFS/PF/EBU choices) or the `mtl-validation-setup` MCP tools directly — no dedicated agent."
name: "MTL Validation — Run Tests"
applyTo: "tests/validation/tests/single/**"
---

# Running MTL Validation pytest

Scope: `tests/validation/tests/single/` only. `dual/` needs two hosts, `invalid/` is for negative cases.

## Health check first

```bash
ls tests/validation/configs/{topology,test}_config.yaml \
   .local_install/mtl/bin/{MtlManager,RxTxApp} \
   tests/validation/venv/bin/python3 2>&1
findmnt -no SOURCE /mnt/media 2>/dev/null || echo NFS_MISSING
modinfo -n ice    # must be /lib/modules/<kver>/updates/... not /kernel/...
```

**Pytest needs `.local_install/mtl/bin/{MtlManager,RxTxApp}`, NOT `build/manager/MtlManager`
or `tests/tools/RxTxApp/build/RxTxApp`.** `tests/validation/mtl_engine/const.py` hardcodes
`PREFIX = ".local_install"` — every app path pytest invokes (RxTxApp, MtlManager, ffmpeg,
gstreamer) resolves under `.local_install/{mtl,ffmpeg,gstreamer}/...`, a separate tree from
the system-wide `build/` + `/usr/local` install gtest/KahawaiTest uses. Symptom when this is
missing: the `mtl_manager` fixture errors with "Failed to start MtlManager on host" even
though gtest works fine and `build/manager/MtlManager` exists.

A probe of pytest-specific setup stages without modifying the host:

```bash
CHECK_ONLY=1 bash .github/scripts/setup_validation.sh
```

Broad host setup — **including the `.local_install` build** — is `setup_environment.sh`
pointed at the `.local_install` prefix instead of the system-wide one:

```bash
MTL_INSTALL_PREFIX=$PWD/.local_install/mtl SETUP_ENVIRONMENT=1 \
  SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 \
  bash .github/scripts/setup_environment.sh
```

`setup_validation.sh` (used above for the health-check probe) then handles only the
pytest-custom stages (NFS, localhost SSH to root, validation venv, configs) on top of that
build.

Anything missing or wrong → re-run the relevant script above, or `CHECK_ONLY=1 bash
.github/scripts/setup_validation.sh` to see which stage is failing. Same for runtime
failures that the table below tags as **(setup)**.

**If `/mnt/media` is not mounted, you MUST tell the setup subagent to ask the user for
`NFS_SOURCE`** — do not let it skip NFS silently. Almost every `tests/single/` test depends
on files under `/mnt/media`. Cold setup takes 7–10 min; warm < 5 s. Do not retry the script
during its first run.

Optional debugging tools the failure rows below assume are present: `sudo apt install -y gdb`.

## Hard rules

- Invoke as `sudo -E ./venv/bin/python3 -m pytest …` from `tests/validation/`. System python lacks `pytest_mfd_config` etc.
- Always pass `--topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml`.
- Tests run as **root over SSH-to-localhost** even on a single host.
- Never edit `conftest.py`, `common/`, or `mtl_engine/` to "fix" a test — fix the env/config.

## Selectors

```bash
cd tests/validation
PY="sudo -E ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml"

$PY -m smoke -v                                              # marker
$PY tests/single/st20p/fps/test_fps.py -m smoke --tb=short -v # proven first pass: p29/ParkJoy_1080p
$PY tests/single/st20p -v                                    # folder
$PY tests/single/st40p -k multicast -v                       # substring
$PY "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]" -v   # exact (quote brackets)
$PY <selector> --collect-only -q                             # dry run
```

Markers (from `pytest.ini`): `smoke` (smallest), `nightly` (bulk single-host), `performance` (long, hardware-bound), `verified`, `refactored`, `ptp`.

## Backend per category

| Category | Backend | Build artifact |
|---|---|---|
| `st20p`/`st22p`/`st30p`/`st40p`/`st41`/`dma`/`ptp`/`rss_mode`/`rx_timing`/`udp`/`virtio_user`/`xdp`/`kernel_socket` | RxTxApp | `tests/tools/RxTxApp/build/RxTxApp` |
| `ffmpeg/` | in-repo FFmpeg + MTL muxer | `ecosystem/ffmpeg_plugin/FFmpeg-release-*/ffmpeg` (system ffmpeg unused) |
| `gstreamer/` | in-repo GStreamer plugin | `ecosystem/gstreamer_plugin/builddir/libgstmtl_*.so` |
| `performance/` | RxTxApp capacity sweep | RxTxApp; very long |

## Logs

`tests/validation/logs/<UTC>/` (root-owned), with `logs/latest` symlink. Per-test `.log` files plus
MtlManager + RxTxApp/ffmpeg/gst output and rendered JSON configs.

```bash
sudo grep -E "EAL|hugepage|VF|RxTxApp|RemoteProcess|Traceback|err:" \
  tests/validation/logs/latest/*.log | head -40
```

## Failure → fix

| Symptom | Fix |
|---|---|
| `No module named pytest` / `pytest_mfd_config` | You used `sudo python3`. Re-run with `sudo -E ./venv/bin/python3`. |
| `unrecognized arguments: --topology_config` | Run from `tests/validation/` (local `conftest.py` registers the plugin). |
| Test hung > test_time + ~30 s | Stale process. `sudo pkill -9 RxTxApp MtlManager ffmpeg gst-launch-1.0` and retry. |
| `EBU server configuration not found` (test still PASSED) | Data path passed; compliance verdict was skipped. Re-run setup with `ebu_ip`/`ebu_user`/`ebu_password`/`capture_pci_device` (ask the user first) via `validation_setup.sh setup --ebu-ip=... --ebu-user=...` or the MCP tools, so `setup_validation.sh` populates `ebu_server`/`capture_cfg` in `test_config.yaml`. |
| `netsniff-ng: command not found` | `sudo apt install -y netsniff-ng`. |
| `build_dpdk.sh: line ...: unzip: command not found` | **(setup)** Fixed: `unzip` now in base apt deps. Re-run setup. |
| `no element "mtl_st20p_tx"` (gstreamer) | **(setup)** Plugin not built. |
| `RemoteProcessInvalidState: Process is finished` (ffmpeg) | **(setup)** In-repo ffmpeg/libav not installed. |
| RxTxApp exit `127` with `error while loading shared libraries: librte_*.so.26` | **(setup)** DPDK is installed but the dynamic linker cache is stale. Re-run `setup_validation.sh` (it refreshes `ldconfig`) or run `sudo ldconfig`, then confirm `ldd .local_install/mtl/bin/RxTxApp` has no `not found`. |
| `RuntimeError: Failed to start MtlManager on host` (pytest `mtl_manager` fixture, setup error not a test failure) | **(setup)** `.local_install/mtl/bin/MtlManager` missing — the host only has the system-wide gtest build. Run `setup_environment.sh` with `MTL_INSTALL_PREFIX=.local_install/mtl` (see above); `build_mtl`/`dpdk_build` target `build/` + `/usr/local` instead, which pytest doesn't use. |
| `Media file not present on <host>: /mnt/media/...` (SKIPPED) | **(setup)** NFS empty/unmounted. Setup subagent must ASK user for `NFS_SOURCE`; never skip silently. |
| `cp: cannot stat /mnt/media/...` | **(setup)** Same — NFS not populated. |
| `mount: bad option ... mount.<type> helper` | **(setup)** `nfs-common` missing. |
| `SSHConnection.__init__() missing … 'password'` | **(setup)** Topology needs `password: ''`. |
| `Incorrect format … PCIDevice` / `not found on host` | **(setup)** `pci_device` must be `vendor:device`, not BDF. |
| `TypeError: 'ExtraInfoModel' object is not subscriptable` | **(setup)** Stale `extra_info.custom_interface`. |
| `capture_cfg.sniff_pci_device=<BDF> not found` | **(setup)** Same — vendor:device not BDF. `setup_validation.sh`'s `stage_configs` patches this automatically when regenerated; if it's still a raw BDF, the config predates that fix — regenerate via `setup_validation_pytest`/`setup_validation_full`. |
| EAL hugepage / VF binding error in logs | **(setup)** Hugepages exhausted or VFs not on `vfio-pci`. |
| RxTxApp `Segmentation fault` inside `iavf_tm_node_add` (after `dev_if_init_pacing(0), try rl as drv support TM`) | **(setup)** Stock kernel ice loaded instead of the MTL out-of-tree patched ice (`versions.env::ICE_VER`). Re-run `setup_validation.sh` — the ice stage version-checks and reloads automatically. |
| RxTxApp `Segmentation fault` anywhere else | **NOT setup.** Capture `gdb -batch -ex 'bt full' .local_install/mtl/bin/RxTxApp /tmp/core.*` (or `coredumpctl gdb RxTxApp`) and report upstream as a real MTL/DPDK bug. Do **not** add a workaround. |
| `Permission denied (publickey)` to `root@127.0.0.1` | **(setup)** Pubkey not in `/root/.ssh/authorized_keys`. |

**(setup)** = re-run `setup_environment.sh` (build stage) and/or `setup_validation.sh`
(pytest-custom stage) — both are idempotent, safe to re-run on an already-prepared host.

If you hit something not in this table: read `logs/latest/*.log` (sudo), match against `mtl_engine/` source, fix the env, **add a row here**.

## Reporting

Selector + counts (`X passed, Y failed, Z skipped`); for each FAIL/ERROR: 1-line root cause + offending log line + path to `logs/latest/`.

## See also

[validation_quickstart.md](../../doc/validation_quickstart.md) · [validation_framework.md](../../doc/validation_framework.md) · [tests/validation/README.md](../../tests/validation/README.md)
