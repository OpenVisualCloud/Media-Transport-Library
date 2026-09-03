# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Oracles for the generated ``test_config.yaml``.

Every case here guards a config value that a *healthy* run depends on, so a
regression shows up as a fake test failure rather than as a config diff:
an under-sized media ramdisk ENOSPCs mid-dump and the byte oracles read the
shortfall as an MTL delivery failure, and an absent ``capture_cfg`` is read by
the ``pcap_capture`` fixture as "this host does compliance".
"""

import importlib.util
import pathlib
from fractions import Fraction

import yaml
from mtl_engine.gstreamer import _MIN_GRADED_WALL_CLOCK_S
from mtl_engine.media_files import yuv_files

_GEN_CONFIG = (
    pathlib.Path(__file__).resolve().parents[1]
    / "acceptance"
    / "configs"
    / "gen_config.py"
)


def _load_gen_config():
    """Import gen_config.py by path -- ``configs/`` is not an importable package."""
    spec = importlib.util.spec_from_file_location("gen_config", _GEN_CONFIG)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gen_config = _load_gen_config()


def _config(**overrides) -> dict:
    params = {
        "session_id": 0,
        "mtl_path": "/opt/mtl",
        "pci_device": "8086:1592",
        "test_time": 30,
    }
    params.update(overrides)
    return yaml.safe_load(gen_config.gen_test_config(**params))


# ---------------------------------------------------------------- media ramdisk
def test_media_ramdisk_holds_a_full_length_rx_dump(monkeypatch):
    """A 30s run at the heaviest graded geometry writes ~70 GiB; 16 would ENOSPC."""
    monkeypatch.setattr(gen_config, "_usable_mem_gib", lambda: 512)

    size_gib = gen_config._media_ramdisk_gib(30)

    dump_gib = gen_config._PEAK_RX_BYTES_PER_S * 30 / (1 << 30)
    assert size_gib > dump_gib
    assert size_gib >= dump_gib + gen_config._MEDIA_ASSET_HEADROOM_GIB - 1


def test_media_ramdisk_scales_with_test_time(monkeypatch):
    monkeypatch.setattr(gen_config, "_usable_mem_gib", lambda: 512)

    assert gen_config._media_ramdisk_gib(60) > gen_config._media_ramdisk_gib(30)


def test_media_ramdisk_never_drops_below_the_floor(monkeypatch):
    """The floor is the fixed size this calculation replaced -- small hosts keep it."""
    monkeypatch.setattr(gen_config, "_usable_mem_gib", lambda: 4)

    assert gen_config._media_ramdisk_gib(30) == gen_config._MEDIA_RAMDISK_FLOOR_GIB


def test_media_ramdisk_is_clamped_to_half_of_usable_ram(monkeypatch, capsys):
    monkeypatch.setattr(gen_config, "_usable_mem_gib", lambda: 80)

    assert gen_config._media_ramdisk_gib(30) == 40
    # The clamp must be audible: it can still ENOSPC on the heaviest cases.
    assert "may hit ENOSPC" in capsys.readouterr().err


def test_usable_mem_excludes_hugepages(monkeypatch, tmp_path):
    """MemTotal counts hugetlb, which a tmpfs cannot use."""
    meminfo = tmp_path / "meminfo"
    meminfo.write_text(
        "MemTotal:       104857600 kB\nHugetlb:         52428800 kB\n",
    )
    real_open = open
    monkeypatch.setattr(
        gen_config,
        "open",
        lambda path, *a, **kw: real_open(meminfo if path == "/proc/meminfo" else path),
        raising=False,
    )

    assert gen_config._usable_mem_gib() == 50


def test_generated_config_carries_the_derived_ramdisk_size(monkeypatch):
    """The derived size has to reach the file, not just be computed.

    Pinned to a patched sizer so the assertion does not depend on how much RAM
    the machine running the unit tier happens to have.
    """
    asked = []

    def _sizer(test_time):
        asked.append(test_time)
        return 4242

    monkeypatch.setattr(gen_config, "_media_ramdisk_gib", _sizer)

    assert _config(test_time=45)["ramdisk"]["media"]["size_gib"] == 4242
    # And sized from the window this config actually configures, not from the
    # argparse default -- a config saying 45 with a mount sized for 30 ENOSPCs.
    assert asked == [45]


def test_media_ramdisk_is_sized_against_the_extended_window(monkeypatch):
    """A short test_time does not mean a short run.

    ``GStreamer._graded_wall_clock`` raises anything below
    ``_MIN_GRADED_WALL_CLOCK_S``, and the RX dump grows for the whole extended
    window -- sizing against the configured value would ENOSPC.
    """
    monkeypatch.setattr(gen_config, "_usable_mem_gib", lambda: 512)

    floor = gen_config._MIN_GSTREAMER_WALL_CLOCK_S
    assert gen_config._media_ramdisk_gib(5) == gen_config._media_ramdisk_gib(floor)


def test_ramdisk_sizing_floor_tracks_the_adapters_wall_clock_floor():
    """The generator cannot import the adapter, so pin the duplicated literal."""
    assert gen_config._MIN_GSTREAMER_WALL_CLOCK_S == _MIN_GRADED_WALL_CLOCK_S


def _rfc4175_bytes_per_s(entry: dict) -> float:
    """Write rate of one RFC4175 PG2BE10 session: 2.5 B/px x pixels x fps."""
    return entry["width"] * entry["height"] * 2.5 * float(Fraction(entry["fps"]))


# The entries `_PEAK_RX_BYTES_PER_S` deliberately does not cover, each with the
# reason it writes nothing the media ramdisk has to hold. The guard below
# maximises over the whole registry minus this set, so adding a heavier geometry
# fails until it is either covered by the constant or classified here -- which is
# the point: an uncovered writer ENOSPCs mid-dump and the byte oracles read the
# shortfall as an MTL delivery failure.
_NO_RX_DUMP_KEYS = {
    # Reached only by the legacy sweep in tests/single/gstreamer/video_resolution
    # (and its dual/ twin), which parametrizes over every yuv_files entry and
    # depends on short windows and on the run stopping before the mount fills;
    # 60s of i4320p119 is 596 GB, which no realistic host can hold at any cap.
    "i4320p119": "legacy gstreamer resolution sweep only",
    "i4320p60": "legacy gstreamer resolution sweep only",
    # Also selected by tests/single/performance/, but a capacity sweep writes
    # nothing to disk: add_perf_st20p_session_rx() takes no output URL, so the RX
    # session has no st20p_url to dump to.
    "i4320p59": "legacy gstreamer sweep + performance sweep, neither dumping",
    "i4320p50": "legacy gstreamer sweep + performance sweep, neither dumping",
}


def test_peak_rx_rate_covers_every_geometry_that_writes_a_dump():
    """``_PEAK_RX_BYTES_PER_S`` is a claim about the registry; hold it to it.

    Maximised over ``yuv_files`` itself rather than over a restated key list, so
    a heavy entry added to the registry -- or an exempt one that a writing case
    starts selecting -- fails here instead of silently under-sizing the mount.
    """
    writers = {
        key: entry for key, entry in yuv_files.items() if key not in _NO_RX_DUMP_KEYS
    }
    peak_key = max(writers, key=lambda key: _rfc4175_bytes_per_s(writers[key]))
    peak = _rfc4175_bytes_per_s(writers[peak_key])

    assert gen_config._PEAK_RX_BYTES_PER_S >= peak, (
        f"{peak_key} writes {peak / 1e9:.3f} GB/s, above the "
        f"{gen_config._PEAK_RX_BYTES_PER_S / 1e9:.3f} GB/s the ramdisk is sized "
        f"for; raise the constant or add {peak_key} to _NO_RX_DUMP_KEYS with the "
        f"reason it writes no dump"
    )
    # And it tracks them rather than being an unrelated round number.
    assert gen_config._PEAK_RX_BYTES_PER_S <= peak * 1.2


def test_exempt_geometries_are_still_in_the_registry():
    """A stale exemption would hide a real writer behind a key that moved."""
    assert not _NO_RX_DUMP_KEYS.keys() - yuv_files.keys()


# ----------------------------------------------------------------- capture_cfg
def test_no_sniff_nic_records_an_explicit_capture_opt_out():
    """An absent capture_cfg is read as "compliance required" by pcap_capture."""
    config = _config(no_capture=True)

    assert config["compliance"] is False
    assert config["capture_cfg"] == {"enable": False}


def test_no_capture_overrides_a_capture_capable_host():
    """``--no_capture`` is the operator's "do not grade compliance here".

    The case above reaches the same opt-out through a single-device pci_device,
    which yields no sniff device at all -- so it pins nothing about the flag. On
    a host that does have a second NIC *and* EBU credentials, the flag is the
    only thing standing between it and an armed capture, e.g. when that second
    port is needed for a redundant (ST2022-7) test.
    """
    config = _config(
        pci_device="8086:1592,8086:1592",
        capture_pci_device="8086:12d2",
        ebu_ip="10.0.0.9",
        ebu_user="user",
        ebu_password="secret",
        no_capture=True,
    )

    assert config["compliance"] is False
    assert config["capture_cfg"] == {"enable": False}
    assert "pcap_dir" not in config["ramdisk"]
    # The credentials are still written: they are what a later regeneration
    # without --no_capture carries forward.
    assert config["ebu_server"]["ebu_ip"] == "10.0.0.9"


def test_sniff_nic_without_ebu_credentials_does_not_arm_a_capture():
    """A pcap nothing can grade is what produced "ebu_server is not configured"."""
    config = _config(pci_device="8086:1592,8086:1592")

    assert config["compliance"] is False
    assert config["capture_cfg"] == {"enable": False}
    assert "pcap_dir" not in config["ramdisk"]


def test_sniff_nic_with_ebu_credentials_enables_compliance():
    config = _config(
        pci_device="8086:1592,8086:1592",
        ebu_ip="10.0.0.9",
        ebu_user="user",
        ebu_password="secret",
    )

    assert config["compliance"] is True
    assert config["capture_cfg"]["enable"] is True
    assert config["capture_cfg"]["sniff_pci_device"] == "8086:1592"
    assert config["ramdisk"]["pcap_dir"] == "/mnt/ramdisk/pcap"
    assert config["ebu_server"]["ebu_ip"] == "10.0.0.9"


def test_dedicated_capture_device_takes_precedence_over_the_legacy_fallback():
    config = _config(
        pci_device="8086:1592,8086:1592",
        capture_pci_device="8086:12d2",
        ebu_ip="10.0.0.9",
        ebu_user="user",
        ebu_password="secret",
    )

    assert config["capture_cfg"]["sniff_pci_device"] == "8086:12d2"
