# MTL Validation Framework

Pytest suite covering ST 2110-20/22/30/40/41 transport, the FFmpeg and
GStreamer plugins, and the DMA, kernel-socket, XDP, and virtio-user
backends — plus performance sweeps and EBU LIST compliance verdicts.

## Getting started

```bash
# From the repository root: one-command host setup
./.github/scripts/acceptance_setup.sh status   # read-only probe
./.github/scripts/acceptance_setup.sh setup    # interactive setup

# Then, from this directory:
sudo -E ./venv/bin/python3 -m pytest \
    --topology_config=configs/topology_config.yaml \
    --test_config=configs/test_config.yaml \
    -m smoke -v
```

Must run as root, from this directory, with the venv interpreter. Full
instructions, configuration reference, and troubleshooting:
[doc/acceptance_quickstart.md](../../doc/acceptance_quickstart.md).

CI uses the real-footage corpus mounted at `/mnt/media`. Media generation is
an optional, manual diagnostic task; neither setup nor pytest invokes it, and
it is not an automatic fallback for a missing NFS mount.

To create synthetic transport-format assets in a separate directory:

```bash
./venv/bin/pip install -r tools/requirements-assets.txt
./venv/bin/python3 tools/gen_acceptance_assets.py --out /tmp/mtl-media-generated
```

The generator does not overwrite existing files unless `--force` is passed.
Point `media_path` at the generated directory only for an explicit local
diagnostic run. Do not generate assets directly into the CI NFS mount.

## Layout

| Path | Contains |
|---|---|
| `tests/single/` | Single-host suites, one per protocol or backend |
| `tests/dual/` | Two-host suites (`-m dual`) |
| `mtl_engine/` | Application adapters, media registry, compliance, integrity |
| `common/` | Host control: NIC/VF setup, hugepages, integrity runners, reports |
| `configs/` | `topology_config.yaml`, `test_config.yaml`, and their generator |
| `create_pcap_file/` | Packet capture for compliance |

## Selecting tests

Suite markers: `smoke`, `nightly`, `performance`, `base_performance`, `dual`,
`ptp`. Descriptive markers: `verified`, `refactored`, `tx_side`, `rx_side`,
`tx_and_rx`, `allow_wide_compliance` — see
[doc/acceptance_quickstart.md § Markers](../../doc/acceptance_quickstart.md#markers)
and `pytest.ini` for the authoritative list.

The two performance markers select no `tests/single/` test. Select the
`tests/single/performance/` modules by path.

## rx_timing and CPU isolation

The `tests/single/rx_timing/` tests run RxTxApp in an exclusive CPU partition
(`cpu_isolation="require"`) and fail when the host cannot provide one. Besides
the wrapper's requirements in
[tests/tools/isolate/README.md](../tools/isolate/README.md), with the first
interface as port P, they need passwordless `sudo -n` for the test account.

## IPMX

The `tests/single/ipmx/` cases check IPMX senders with the VSF TR-10 TP-1
section 13 scripts. They skip unless all of these are in place:

- MTL in `.local_install` built with `-Denable_ipmx=true` (`ipmx=true` in
  its `mtl.pc`).
- A clone of the VSF IPMX test scripts (`nmos-testing-ipmx`), named by
  `ipmx.vsf_path` in `test_config.yaml` (see
  [configs/examples/test_config.yaml](configs/examples/test_config.yaml), or
  `gen_config.py --ipmx_vsf_path`). The scripts are not copied into this
  repository.
- A Python 3 interpreter for the scripts (`ipmx.vsf_python`, default
  `python3`; scapy is optional), plus tcpdump, reordercap, editcap and tshark
  on the host.

The JPEG XS cases (`test_ipmx_st22p.py`) also need:

- The SVT-JPEG-XS MTL plugin, enabled in `.local_install/conf/kahawai.json`,
  and its `mtl-plugin/` directory as `ipmx.svt_plugin_dir`: the cases load
  its `ipmx_1080p.json` / `ipmx_2160p.json`, switched to Sublev4bpp for the
  4 bpp points. The RGB 4:4:4 8-bit (`GBRPLANAR8`) and 10-bit
  (`GBRPLANAR10LE`, IDs `rgb10_*`) points use the same files.
- MTL scheduler CPUs that no other thread uses. The SVT encoder and decoder
  threads float over every CPU of the process; on a scheduler CPU they delay
  the TX frame start by milliseconds, which fails TR-10-9 section 11.2. Keep
  them off with a cpuset (`isolcpus`, or a cgroup holding the MTL lcores
  apart) and name those CPUs in `ipmx.lcores` (RxTxApp `--lcores`).
  `cpu_isolation="require"` does not help here: it puts the SVT threads into
  the same small partition.

The capture runs on the kernel netdev of the RX VF's PF, with hardware
timestamps, so the TX and RX ports must reach each other over the cable (VFs
of two PFs); the cases skip otherwise. The validators read 2 s of frames
(TR-10-9 section 11.2), or `ipmx.max_frames` if larger; a 2160p raw case takes
about 45 minutes. The capture and the SDP of each case stay in
`/tmp/ipmx_acceptance/<case>/`.

## Further reading

- [Setup and run](../../doc/acceptance_quickstart.md)
- [Architecture](../../doc/acceptance-design.md) — modules, adapters,
  fixtures, oracles, clock model
- [Test strategy](docs/test-strategy.md) — matrix depth, capability policy,
  path assertions, and oracles
- [Build guide](../../doc/build.md)
