from __future__ import annotations

import csv
import importlib.util
import json
from pathlib import Path
from unittest.mock import patch

import pytest

from mxlperf.common import ROOT, load_config
from mxlperf.rdt import (
    EXPECTED_HELPER_VERSION,
    VALID_PROFILES,
    _helper,
    collect_rdt,
    is_rdt_enabled,
    kubernetes_groups,
    rdt_capabilities,
    validate_rdt_config,
)


def _host_helper():
    spec = importlib.util.spec_from_file_location("mxl_rdt_host", ROOT / "scripts/mxl-rdt-host.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Kernel prints resctrl schemata with leading whitespace.
INDENTED_SCHEMATA = "    MB:0=100;1=100\n    L2:0=ffff;1=ffff\n    L3:0=ffff;1=ffff\n"


def test_indented_schemata_resources_are_found() -> None:
    helper = _host_helper()
    assert helper.resource_ids(INDENTED_SCHEMATA, "L3") == ["0", "1"]
    assert helper.resource_ids(INDENTED_SCHEMATA, "MB") == ["0", "1"]
    assert helper.schemata_line("L3", "fff0", INDENTED_SCHEMATA) == "L3:0=fff0;1=fff0"


def test_missing_resource_reports_available_resources() -> None:
    helper = _host_helper()
    with pytest.raises(RuntimeError, match=r"available: L2, L3, MB"):
        helper.schemata_line("L4", "ff", INDENTED_SCHEMATA)


def test_stress_ng_worker_names_are_matched() -> None:
    helper = _host_helper()
    # stress-ng renames workers; matching only the parent captured no traffic.
    assert helper.comm_matches("stress-ng", "stress-ng")
    assert helper.comm_matches("stress-ng-stream", "stress-ng")
    assert helper.comm_matches("stress-ng-cache", "stress-ng")
    assert helper.comm_matches("ffmpeg", "ffmpeg")
    assert not helper.comm_matches("stress-nglike", "stress-ng")
    assert not helper.comm_matches("ffmpegx", "ffmpeg")


def test_unassociated_group_fails_collection(tmp_path: Path) -> None:
    samples = [
        ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"],
        ["10", "encoder", "mon_L3_00", "100", "1000", "2000"],
        ["11", "encoder", "mon_L3_00", "200", "3000", "5000"],
        ["10", "noise", "mon_L3_00", "0", "0", "0"],
        ["11", "noise", "mon_L3_00", "0", "0", "0"],
    ]
    with pytest.raises(RuntimeError, match="RDT group noise measured zero"):
        _collect(tmp_path, samples)


def _helper_with_l3(tmp_path: Path, cbm_mask: str, min_cbm_bits: str):
    helper = _host_helper()
    info = tmp_path / "info/L3"
    info.mkdir(parents=True)
    (info / "cbm_mask").write_text(cbm_mask + "\n")
    (info / "min_cbm_bits").write_text(min_cbm_bits + "\n")
    helper.RESCTRL = tmp_path
    return helper


def test_cat_guarded_masks_split_llc_ways(tmp_path: Path) -> None:
    # The reference platform exposes 16 LLC ways; guarded keeps 12 for workload, 4 for noise.
    helper = _helper_with_l3(tmp_path, "ffff", "1")
    workload, noise = helper.l3_masks(0.75)
    assert (workload, noise) == ("fff0", "f")
    assert bin(int(workload, 16)).count("1") == 12
    assert bin(int(noise, 16)).count("1") == 4
    assert int(workload, 16) & int(noise, 16) == 0


def test_cat_strong_masks_squeeze_noise_further(tmp_path: Path) -> None:
    helper = _helper_with_l3(tmp_path, "ffff", "1")
    workload, noise = helper.l3_masks(0.875)
    assert (workload, noise) == ("fffc", "3")
    assert int(workload, 16) | int(noise, 16) == 0xFFFF


def test_cat_16_1_shares_one_way_instead_of_partitioning(tmp_path: Path) -> None:
    # cat-16-1 is the only overlapping profile: FFmpeg keeps all 16 ways and the
    # neighbour is confined to way 0, which FFmpeg may also use. That measured
    # best on the reference worker, better than any clean partition.
    helper = _helper_with_l3(tmp_path, "ffff", "1")
    workload, noise = helper.l3_masks_shared_one_way()
    assert (workload, noise) == ("ffff", "1")
    assert int(workload, 16) & int(noise, 16) == 1


def test_cat_masks_respect_minimum_cbm_bits(tmp_path: Path) -> None:
    helper = _helper_with_l3(tmp_path, "ffff", "4")
    workload, noise = helper.l3_masks(0.875)
    assert bin(int(noise, 16)).count("1") == 4
    assert bin(int(workload, 16)).count("1") == 12


def test_cat_fails_when_llc_ways_insufficient(tmp_path: Path) -> None:
    helper = _helper_with_l3(tmp_path, "3", "2")
    with pytest.raises(RuntimeError, match="insufficient LLC ways"):
        helper.l3_masks(0.75)


@pytest.mark.parametrize("profile", ["cat-guarded+mba-40", "cat-strong+mba-20", "cat-16-1+mba-20"])
def test_combined_profiles_accept_any_noise_profile(profile: str) -> None:
    for noise in ("host-a", "pod-a", "pod-b", "pod-c"):
        cfg = load_config("pinned", [f"RDT_CONTROL_PROFILE={profile}"], noise)
        validate_rdt_config(cfg)


def test_combined_profile_still_requires_noise() -> None:
    cfg = load_config("pinned", ["RDT_CONTROL_PROFILE=cat-guarded+mba-40"])
    with pytest.raises(ValueError, match="requires a noisy neighbor"):
        validate_rdt_config(cfg)


def test_combined_profile_sets_both_schemata_resources() -> None:
    helper = _host_helper()
    assert "cat-guarded+mba-40" in helper.VALID_PROFILE
    parts = "cat-guarded+mba-40".split("+")
    assert parts[0] in helper.CAT_PARTITION_FRACTION
    assert helper.schemata_line("L3", "fff0", INDENTED_SCHEMATA) == "L3:0=fff0;1=fff0"
    assert helper.schemata_line("MB", "40", INDENTED_SCHEMATA) == "MB:0=40;1=40"


def test_rdt_disabled_by_default() -> None:
    cfg = load_config("baseline", [])
    assert not is_rdt_enabled(cfg)
    validate_rdt_config(cfg)


def test_monitor_only_works_for_existing_scenario() -> None:
    cfg = load_config("numa-pool", ["RDT_MONITOR=1"])
    assert is_rdt_enabled(cfg)
    validate_rdt_config(cfg)


@pytest.mark.parametrize("noise", ["host-a", "pod-a", "pod-b", "pod-c"])
@pytest.mark.parametrize("profile", ["cat-guarded", "cat-strong", "cat-16-1", "mba-20", "cat-16-1+mba-20"])
def test_any_lever_allowed_with_any_noise(profile: str, noise: str) -> None:
    # CAT limits streaming cache pollution and MBA slows LLC-thrashing fill rate,
    # so cross-pairings are valid experiments rather than configuration errors.
    validate_rdt_config(load_config("pinned", [f"RDT_CONTROL_PROFILE={profile}"], noise))


def test_control_still_requires_noise() -> None:
    for profile in ("cat-guarded", "mba-40", "cat-strong+mba-20"):
        cfg = load_config("pinned", [f"RDT_CONTROL_PROFILE={profile}"])
        with pytest.raises(ValueError, match="requires a noisy neighbor"):
            validate_rdt_config(cfg)


def _pod_list(*uids: str) -> str:
    return json.dumps({"items": [{"metadata": {"uid": uid}} for uid in uids]})


def test_encoder_and_decoder_get_separate_groups() -> None:
    calls: list[list[str]] = []

    def fake_run(command, **_):
        calls.append(command)
        role = next(part for part in command if part.startswith("app="))
        return _pod_list("enc-1", "enc-2") if "role=encoder" in role else _pod_list("dec-1")

    with patch("mxlperf.rdt.run", side_effect=fake_run):
        groups = kubernetes_groups("mxl", "mxl-bench", {})

    assert set(groups) == {"encoder", "decoder"}
    assert groups["encoder"]["pod_uids"] == ["enc-1", "enc-2"]
    assert groups["decoder"]["pod_uids"] == ["dec-1"]
    assert all(group["comm"] == "ffmpeg" for group in groups.values())


def test_focus_session_narrows_selectors_to_one_instance() -> None:
    selectors: list[str] = []

    def fake_run(command, **_):
        selector = next(part for part in command if part.startswith("app="))
        selectors.append(selector)
        return _pod_list("uid-1")

    with patch("mxlperf.rdt.run", side_effect=fake_run):
        groups = kubernetes_groups("mxl", "mxl-bench", {"RDT_FOCUS_SESSION": "s01"})

    assert selectors == ["app=mxl-bench,role=encoder,session=s01", "app=mxl-bench,role=decoder,session=s01"]
    assert set(groups) == {"encoder", "decoder"}


def test_missing_workload_pods_names_focus_target() -> None:
    with patch("mxlperf.rdt.run", return_value=_pod_list()):
        with pytest.raises(RuntimeError, match="session s07"):
            kubernetes_groups("mxl", "mxl-bench", {"RDT_FOCUS_SESSION": "s07"})


@pytest.mark.parametrize("profile", ["mba-80", "mba-60", "mba-40", "mba-20", "mba-10"])
def test_mba_steps_accepted_with_host_bandwidth_noise(profile: str) -> None:
    cfg = load_config("pinned", [f"RDT_CONTROL_PROFILE={profile}"], "host-a")
    validate_rdt_config(cfg)


def test_unsupported_mba_step_is_rejected() -> None:
    cfg = load_config("pinned", ["RDT_CONTROL_PROFILE=mba-5"], "host-a")
    with pytest.raises(ValueError, match="unsupported RDT_CONTROL_PROFILE"):
        validate_rdt_config(cfg)


def test_controller_and_helper_versions_match() -> None:
    assert _host_helper().HELPER_VERSION == EXPECTED_HELPER_VERSION


def test_full_cat_mba_matrix_is_available() -> None:
    helper = _host_helper()
    expected = {f"cat-{strength}+mba-{level}"
                for strength in ("guarded", "strong", "16-1")
                for level in ("80", "60", "40", "20", "10")}
    assert expected <= VALID_PROFILES
    assert expected <= helper.VALID_PROFILE
    assert VALID_PROFILES == helper.VALID_PROFILE
    assert len(VALID_PROFILES) == 1 + 3 + 5 + 15


def test_outdated_worker_helper_is_rejected() -> None:
    stale = json.dumps({"helper_version": EXPECTED_HELPER_VERSION - 1, "supported_profiles": []})
    with patch("mxlperf.rdt._helper", return_value=stale):
        with pytest.raises(RuntimeError, match="update-rdt-helper.sh"):
            rdt_capabilities({})


def test_helper_without_requested_profile_is_rejected() -> None:
    current = json.dumps({
        "helper_version": EXPECTED_HELPER_VERSION,
        "supported_profiles": ["none", "cat-guarded"],
    })
    with patch("mxlperf.rdt._helper", return_value=current):
        with pytest.raises(RuntimeError, match="does not support control profile mba-10"):
            rdt_capabilities({"RDT_CONTROL_PROFILE": "mba-10"})


@pytest.mark.parametrize(
    ("target", "expected"),
    [
        ("root@192.0.2.10", ["/usr/local/sbin/mxl-rdt-host", "capabilities"]),
        ("operator@192.0.2.10", ["sudo", "-n", "/usr/local/sbin/mxl-rdt-host", "capabilities"]),
    ],
)
def test_rdt_helper_selects_privilege_for_login(target: str, expected: list[str]) -> None:
    with patch("mxlperf.rdt._target", return_value=target), patch(
        "mxlperf.rdt._ssh", return_value="{}"
    ) as ssh_mock:
        _helper({}, "capabilities")

    assert ssh_mock.call_args.args[1] == expected


def test_rdt_sample_aggregation(tmp_path: Path) -> None:
    samples = [
        ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"],
        ["10", "workload", "mon_L3_00", "100", "1000", "1400"],
        ["12", "workload", "mon_L3_00", "300", "3000", "4200"],
    ]

    def fake_copy(_cfg: dict[str, str], remote: str, local: Path) -> None:
        if remote.endswith("samples.csv"):
            with local.open("w", newline="") as handle:
                csv.writer(handle).writerows(samples)
        else:
            local.write_text("{}\n")

    with patch("mxlperf.rdt._copy", side_effect=fake_copy):
        rows = collect_rdt({}, tmp_path)

    summary = json.loads((tmp_path / "rdt-summary.json").read_text())
    assert summary[0]["llc_occupancy_bytes_avg"] == 200
    assert summary[0]["mbm_total_bytes_per_second_avg"] == 1400
    assert {row["metric"] for row in rows} == {
        "rdt_llc_occupancy_bytes",
        "rdt_mbm_local_bytes_per_second",
        "rdt_mbm_total_bytes_per_second",
        "rdt_mbm_remote_bytes_per_second",
    }


def _collect(tmp_path: Path, samples: list[list[str]]) -> list[dict[str, object]]:
    def fake_copy(_cfg: dict[str, str], remote: str, local: Path) -> None:
        if remote.endswith("samples.csv"):
            with local.open("w", newline="") as handle:
                csv.writer(handle).writerows(samples)
        else:
            local.write_text("{}\n")

    with patch("mxlperf.rdt._copy", side_effect=fake_copy):
        return collect_rdt({}, tmp_path)


def test_derived_remote_rate_never_reports_counter_reset(tmp_path: Path) -> None:
    # Local traffic growing faster than total is normal; remote is derived, not a hardware counter.
    _collect(
        tmp_path,
        [
            ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"],
            ["10", "workload", "mon_L3_00", "100", "1000", "2000"],
            ["11", "workload", "mon_L3_00", "100", "3000", "3500"],
            ["12", "workload", "mon_L3_00", "100", "3200", "6000"],
        ],
    )
    summary = json.loads((tmp_path / "rdt-summary.json").read_text())
    assert summary[0]["mbm_remote_bytes_per_second_min"] == 0.0
    assert summary[0]["mbm_remote_bytes_per_second_max"] == 2300.0


def test_unavailable_counters_are_skipped(tmp_path: Path) -> None:
    _collect(
        tmp_path,
        [
            ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"],
            ["10", "workload", "mon_L3_00", "100", "", ""],
            ["11", "workload", "mon_L3_00", "200", "1000", "2000"],
            ["12", "workload", "mon_L3_00", "300", "2000", "4000"],
        ],
    )
    summary = json.loads((tmp_path / "rdt-summary.json").read_text())
    assert summary[0]["sample_count"] == 2
    assert summary[0]["mbm_total_bytes_per_second_avg"] == 2000.0


def test_real_counter_reset_still_fails(tmp_path: Path) -> None:
    with pytest.raises(RuntimeError, match="counter reset"):
        _collect(
            tmp_path,
            [
                ["timestamp", "group", "domain", "llc_occupancy_bytes", "mbm_local_bytes", "mbm_total_bytes"],
                ["10", "workload", "mon_L3_00", "100", "5000", "9000"],
                ["11", "workload", "mon_L3_00", "100", "10", "20"],
            ],
        )
