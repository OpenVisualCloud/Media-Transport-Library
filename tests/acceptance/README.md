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
`ptp`. Other markers describe a test rather than select it — see
[doc/acceptance_quickstart.md § Markers](../../doc/acceptance_quickstart.md#markers)
and `pytest.ini` for the authoritative list.

## Further reading

- [Setup and run](../../doc/acceptance_quickstart.md)
- [Architecture](../../doc/acceptance-design.md) — modules, adapters,
  fixtures, oracles, clock model
- [Test strategy](docs/test-strategy.md) — matrix depth, capability policy,
  path assertions, and oracles
- [Build guide](../../doc/build.md)
