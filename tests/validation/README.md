# MTL Validation Framework

Pytest suite covering ST 2110-20/22/30/40/41 transport, the FFmpeg and
GStreamer plugins, and the DMA, kernel-socket, XDP, and virtio-user
backends — plus performance sweeps and EBU LIST compliance verdicts.

## Getting started

```bash
# From the repository root: one-command host setup
./.github/scripts/validation_setup.sh status   # read-only probe
./.github/scripts/validation_setup.sh setup    # interactive setup

# Then, from this directory:
sudo -E ./venv/bin/python3 -m pytest \
    --topology_config=configs/topology_config.yaml \
    --test_config=configs/test_config.yaml \
    -m smoke -v
```

Must run as root, from this directory, with the venv interpreter. Full
instructions, configuration reference, and troubleshooting:
[doc/validation_quickstart.md](../../doc/validation_quickstart.md).

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
[doc/validation_quickstart.md § Markers](../../doc/validation_quickstart.md#markers)
and `pytest.ini` for the authoritative list.

## Further reading

- [Setup and run](../../doc/validation_quickstart.md)
- [Architecture](../../doc/validation-design.md) — modules, adapters,
  fixtures, oracles, clock model
- [Build guide](../../doc/build.md)
