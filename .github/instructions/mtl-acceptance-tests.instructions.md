---
description: "Use when the user asks to RUN, collect, debug, or diagnose pytest cases under tests/acceptance/tests/single/ (st20p, st22p, st30p, st40p, st41, ffmpeg, gstreamer, dma, ptp, rss_mode, rx_timing, udp, virtio_user, xdp, kernel_socket); running by marker (-m smoke/-m nightly); investigating logs under tests/acceptance/logs/. For AUTHORING new cases see mtl-acceptance-authoring.instructions.md. For host setup (build, hugepages, NFS, configs): call `.github/scripts/acceptance_setup.sh status`/`setup` (interactive, prompts for NFS/PF/EBU choices) or the `mtl-acceptance-setup` MCP tools directly — no dedicated agent."
name: "MTL Acceptance Tests — Run Tests"
applyTo: "tests/acceptance/tests/single/**"
---

# Running MTL Validation pytest

Scope: `tests/acceptance/tests/single/` only. `dual/` needs two hosts.

## Health check first

```bash
./.github/scripts/acceptance_setup.sh status    # read-only; names the broken stage
```

This covers kernel/CPU, hugepages, governor, ICE driver, NIC PFs,
`.local_install` build state, NFS media, venv, and configs. Repair with
`setup --base-only` (build/driver/hugepages) or `setup --pytest-only`
(NFS, SSH, venv, configs); both are idempotent. Rows tagged **(setup)** below
are all fixed this way. Full reference:
[acceptance_quickstart.md](../../doc/acceptance_quickstart.md).

**Pytest needs `.local_install/mtl/bin/{MtlManager,RxTxApp}`, NOT `build/manager/MtlManager`
or `tests/tools/RxTxApp/build/RxTxApp`.** `tests/acceptance/mtl_engine/const.py` hardcodes
`PREFIX = ".local_install"` — every app path pytest invokes (RxTxApp, MtlManager, ffmpeg,
gstreamer) resolves under `.local_install/{mtl,ffmpeg,gstreamer}/...`, a separate tree from
the system-wide `build/` + `/usr/local` install gtest/KahawaiTest uses. Symptom when this is
missing: the `mtl_manager` fixture errors with "Failed to start MtlManager on host" even
though gtest works fine and `build/manager/MtlManager` exists.

**If `/mnt/media` is not mounted, you MUST ask the user for the NFS source** before running
setup — do not skip NFS silently. Almost every `tests/single/` test depends on files under
`/mnt/media`. Cold setup takes 7–10 min; warm < 5 s. Do not retry the script during its
first run.

Some failure rows below assume `gdb` is installed (`sudo apt install -y gdb`).

## Hard rules

- Invoke as `sudo -E ./venv/bin/python3 -m pytest …` from `tests/acceptance/`. System python lacks `pytest_mfd_config` etc.
- Always pass `--topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml`.
- Tests run as **root over SSH-to-localhost** even on a single host.
- Never edit `conftest.py`, `common/`, or `mtl_engine/` to "fix" a test — fix the env/config.

## Selectors

```bash
cd tests/acceptance
PY="sudo -E ./venv/bin/python3 -m pytest --topology_config=configs/topology_config.yaml --test_config=configs/test_config.yaml"

$PY -m smoke -v                                              # marker
$PY tests/single/st20p/fps/test_fps.py -m smoke --tb=short -v # proven first pass: p29/ParkJoy_1080p
$PY tests/single/st20p -v                                    # folder
$PY tests/single/st40p -k multicast -v                       # substring
$PY "tests/single/st20p/fps/test_fps.py::test_fps[|fps = p60|-ParkJoy_1080p]" -v   # exact (quote brackets)
$PY <selector> --collect-only -q                             # dry run
```

Selection markers: `smoke` (smallest), `nightly` (bulk single-host), `performance` (long,
hardware-bound). The full marker set and its authoring rules live in
[mtl-acceptance-authoring.instructions.md](mtl-acceptance-authoring.instructions.md#markers).

## Backend per category

| Category | Backend | Build artifact |
|---|---|---|
| `st20p`/`st22p`/`st30p`/`st40p`/`st41`/`dma`/`ptp`/`rss_mode`/`rx_timing`/`udp`/`virtio_user`/`xdp`/`kernel_socket` | RxTxApp | `.local_install/mtl/bin/RxTxApp` |
| `ffmpeg/`, or any `application="ffmpeg"` param | in-repo FFmpeg + MTL muxer | `ecosystem/ffmpeg_plugin/FFmpeg-release-*/ffmpeg` (system ffmpeg unused) |
| `gstreamer/`, or any `application="gstreamer"` param | in-repo GStreamer plugin | `ecosystem/gstreamer_plugin/builddir/libgstmtl_*.so` |
| `performance/` | RxTxApp capacity sweep | RxTxApp; very long |

## Logs

`tests/acceptance/logs/<UTC>/` (root-owned), with `logs/latest` symlink. Per-test `.log` files plus
MtlManager + RxTxApp/ffmpeg/gst output and rendered JSON configs.

```bash
sudo grep -E "EAL|hugepage|VF|RxTxApp|RemoteProcess|Traceback|err:" \
  tests/acceptance/logs/latest/*.log | head -40
```

## Failure → fix

| Symptom | Fix |
|---|---|
| `No module named pytest` / `pytest_mfd_config` | You used `sudo python3`. Re-run with `sudo -E ./venv/bin/python3`. |
| `unrecognized arguments: --topology_config` | Run from `tests/acceptance/` (local `conftest.py` registers the plugin). |
| Test hung > test_time + ~30 s | Stale process. `sudo pkill -9 RxTxApp MtlManager ffmpeg gst-launch-1.0` and retry. |
| `EBU server configuration not found` (test still PASSED) | Data path passed; compliance verdict was skipped. Re-run setup with `--ebu-ip=`/`--ebu-user=`/`--ebu-password=`/`--capture-pci-device=` (ask the user first) so `ebu_server`/`capture_cfg` get populated in `test_config.yaml`. |
| `netsniff-ng: command not found` | `sudo apt install -y netsniff-ng`. |
| `build_dpdk.sh: line ...: unzip: command not found` | **(setup)** Fixed: `unzip` now in base apt deps. Re-run setup. |
| `no element "mtl_st20p_tx"` (gstreamer) | **(setup)** Plugin not built. |
| `RemoteProcessInvalidState: Process is finished` (ffmpeg) | **(setup)** In-repo ffmpeg/libav not installed. |
| RxTxApp exit `127` with `error while loading shared libraries: librte_*.so.26` | **(setup)** DPDK is installed but the dynamic linker cache is stale. Re-run setup (it refreshes `ldconfig`) or run `sudo ldconfig`, then confirm `ldd .local_install/mtl/bin/RxTxApp` has no `not found`. |
| `RuntimeError: Failed to start MtlManager on host` (pytest `mtl_manager` fixture, setup error not a test failure) | **(setup)** `.local_install/mtl/bin/MtlManager` missing — the host only has the system-wide gtest build. Run `acceptance_setup.sh setup --base-only`; the `build_mtl`/`dpdk_build` MCP tools target `build/` + `/usr/local` instead, which pytest doesn't use. |
| `Media file not present on <host>: /mnt/media/...` (SKIPPED) | **(setup)** NFS empty/unmounted. ASK the user for the NFS source; never skip silently. |
| `cp: cannot stat /mnt/media/...` | **(setup)** Same — NFS not populated. |
| `mount: bad option ... mount.<type> helper` | **(setup)** `nfs-common` missing. |
| `SSHConnection.__init__() missing … 'password'` | **(setup)** Topology needs `password: ''`. |
| `Incorrect format … PCIDevice` / `not found on host` | **(setup)** `pci_device` must be `vendor:device`, not BDF. |
| `TypeError: 'ExtraInfoModel' object is not subscriptable` | **(setup)** Stale `extra_info.custom_interface`. |
| `capture_cfg.sniff_pci_device=<BDF> not found` | **(setup)** Same — vendor:device not BDF. Config generation patches this automatically; if it's still a raw BDF the config predates that fix, so regenerate it with `acceptance_setup.sh setup --pytest-only`. |
| EAL hugepage / VF binding error in logs | **(setup)** Hugepages exhausted or VFs not on `vfio-pci`. |
| RxTxApp `Segmentation fault` inside `iavf_tm_node_add` (after `dev_if_init_pacing(0), try rl as drv support TM`) | **(setup)** Stock kernel ice loaded instead of the MTL out-of-tree patched ice (`versions.env::ICE_VER`). Re-run setup — the ice stage version-checks and reloads automatically. |
| RxTxApp `Segmentation fault` anywhere else | **NOT setup.** Capture `gdb -batch -ex 'bt full' .local_install/mtl/bin/RxTxApp /tmp/core.*` (or `coredumpctl gdb RxTxApp`) and report upstream as a real MTL/DPDK bug. Do **not** add a workaround. |
| `Permission denied (publickey)` to `root@127.0.0.1` | **(setup)** Pubkey not in `/root/.ssh/authorized_keys`. |

**(setup)** = re-run `.github/scripts/acceptance_setup.sh setup` — `--base-only` for
build/driver/hugepage problems, `--pytest-only` for NFS/SSH/venv/config problems. Both are
idempotent and safe to re-run on an already-prepared host.

If you hit something not in this table: read `logs/latest/*.log` (sudo), match against `mtl_engine/` source, fix the env, **add a row here**.

## Reporting

Selector + counts (`X passed, Y failed, Z skipped`); for each FAIL/ERROR: 1-line root cause + offending log line + path to `logs/latest/`.

## See also

Authoring new cases: [mtl-acceptance-authoring.instructions.md](mtl-acceptance-authoring.instructions.md) ·
Architecture: [acceptance-design.md](../../doc/acceptance-design.md) ·
Setup, configuration, troubleshooting: [acceptance_quickstart.md](../../doc/acceptance_quickstart.md) ·
[tests/acceptance/README.md](../../tests/acceptance/README.md)
