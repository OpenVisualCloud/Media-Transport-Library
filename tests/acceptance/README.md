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

## Further reading

- [Setup and run](../../doc/acceptance_quickstart.md)
- [Architecture](../../doc/acceptance-design.md) — modules, adapters,
  fixtures, oracles, clock model
- [Build guide](../../doc/build.md)
