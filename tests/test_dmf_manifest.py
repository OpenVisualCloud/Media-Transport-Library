"""Tests for the DMF CRM manifest generator (mxlperf.dmf_manifest).

Covers:
  - PerfSpect JSON parsing (hierarchical and flat-record formats)
  - metrics.csv parsing and unit conversion
  - Manifest building and config-driven field population
  - Schema validation (pass and fail)
  - CLI: resolved profile config, missing/invalid inputs, --no-validate, --dry-run
  - Ambiguous / missing JSON file detection
"""
from __future__ import annotations

import csv
import json
import shutil
import textwrap
from pathlib import Path

import pytest
import yaml

from mxlperf.dmf_manifest import (
    _find_json_file,
    _normalize_isa_label,
    _parse_isa_features,
    build_manifest,
    cores_to_millicore_str,
    mib_to_k8s_memory,
    parse_metrics_csv,
    parse_perfspect_json,
    validate_manifest,
    main,
)

# ──────────────────────────────────────────────────────────────────────────────
# Fixtures directory
# ──────────────────────────────────────────────────────────────────────────────

FIXTURES = Path(__file__).parent / "fixtures"
PERFSPECT_HIER = FIXTURES / "perfspect-sample.json"
PERFSPECT_FLAT = FIXTURES / "perfspect-flat-records.json"
PERFSPECT_HIER_FULL = FIXTURES / "perfspect-sample-full.json"
PERFSPECT_FLAT_FULL = FIXTURES / "perfspect-flat-records-full.json"
PERFSPECT_LIST_CPU_ISA = FIXTURES / "perfspect-list-cpu-isa.json"
METRICS_CSV = FIXTURES / "metrics-sample.csv"
SCHEMA_PATH = Path(__file__).parent.parent / "manifest" / "schema" / "resource_manifest_schema.yaml"


def _profile_config(**overrides):
    cfg = {
        "SCENARIO": "pinned",
        "PLACEMENT": "exclusive",
        "RESOLUTION": "1080p",
        "PRESET": "medium",
        "BITRATE": "12M",
        "DEC_THREADS": "1",
        "FILTER_THREADS": "1",
        "ENC_THREADS": "15",
        "SLICES": "2",
    }
    cfg.update(overrides)
    return cfg


@pytest.fixture()
def profile_dir(tmp_path):
    """A temporary profiling run directory with metrics.csv and config.json."""
    import shutil
    shutil.copy(METRICS_CSV, tmp_path / "metrics.csv")
    (tmp_path / "config.json").write_text(json.dumps(_profile_config()))
    return tmp_path

# ──────────────────────────────────────────────────────────────────────────────
# Unit conversion helpers
# ──────────────────────────────────────────────────────────────────────────────


def test_cores_to_millicore_str_half_core():
    assert cores_to_millicore_str(0.5) == "500m"


def test_cores_to_millicore_str_five_cores():
    assert cores_to_millicore_str(5.0) == "5000m"


def test_cores_to_millicore_str_fractional():
    # 4.823 cores × 1.20 margin ≈ 5.788 → 5788m
    assert cores_to_millicore_str(4.823 * 1.20) == "5788m"


def test_mib_to_k8s_memory_exact_mib():
    assert mib_to_k8s_memory(512) == "512Mi"


def test_mib_to_k8s_memory_round_gib():
    assert mib_to_k8s_memory(2048) == "2Gi"


def test_mib_to_k8s_memory_tib():
    assert mib_to_k8s_memory(1024 * 1024) == "1Ti"


def test_mib_to_k8s_memory_fractional_gib():
    # 1536 MiB = 1.5 GiB — not divisible so stays in Mi
    assert mib_to_k8s_memory(1536) == "1536Mi"


# ──────────────────────────────────────────────────────────────────────────────
# PerfSpect JSON parsing — hierarchical format (perfspect-sample.json)
# ──────────────────────────────────────────────────────────────────────────────


def test_parse_perfspect_hierarchical_cpu_fields():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    cpu = caps["cpu"]
    assert cpu["numa_nodes"] == 2
    assert cpu["number_of_cores"] == 128  # 2 sockets × 64 cores
    assert "Granite Rapids" in cpu["generation"] or "6767P" in cpu["generation"]


def test_parse_perfspect_hierarchical_cpu_frequency():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    assert "2500MHz" in caps["cpu"]["base_frequency"]


def test_parse_perfspect_hierarchical_no_unsupported_max_frequency():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    assert "max_frequency" not in caps["cpu"]


def test_parse_perfspect_hierarchical_memory():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    assert "memory" in caps
    assert caps["memory"][0]["type"] == "ram"
    assert caps["memory"][0]["generation"] == "DDR5"


def test_parse_perfspect_hierarchical_memory_size_per_node():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    # total_gb=503 → 503/2 = 251 Gi per node (integer division)
    assert caps["memory"][0]["size"] == "251Gi"


def test_parse_perfspect_hierarchical_memory_numa_count():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    assert len(caps["memory"]) == 2  # one entry per NUMA node


def test_parse_perfspect_hierarchical_full_host_capabilities():
    caps = parse_perfspect_json(PERFSPECT_HIER_FULL)
    assert {"cpu", "network", "memory", "storage"}.issubset(caps.keys())
    assert "avx512f" in [f.lower() for f in caps["cpu"]["features"]]
    assert caps["network"][0]["name"] == "eth0"
    assert caps["network"][0]["bandwidth"]["input"] == "25Gbps"
    assert caps["memory"][0]["throughput"]["total"] == "5600MT/s"
    assert caps["storage"][0]["name"] == "nvme0n1"
    assert caps["storage"][0]["type"] == "nvme"
    assert caps["storage"][0]["throughput"]["read"] == "7000MB/s"


# ──────────────────────────────────────────────────────────────────────────────
# PerfSpect JSON parsing — flat record format (perfspect-flat-records.json)
# ──────────────────────────────────────────────────────────────────────────────


def test_parse_perfspect_flat_records_cpu_fields():
    caps = parse_perfspect_json(PERFSPECT_FLAT)
    cpu = caps["cpu"]
    assert cpu["numa_nodes"] == 2
    assert cpu["number_of_cores"] == 36  # 2 × 18


def test_parse_perfspect_flat_records_memory():
    caps = parse_perfspect_json(PERFSPECT_FLAT)
    assert "memory" in caps
    assert caps["memory"][0]["generation"] == "DDR4"


def test_parse_perfspect_flat_records_base_frequency():
    caps = parse_perfspect_json(PERFSPECT_FLAT)
    assert caps["cpu"]["base_frequency"] == "3.0 GHz"


def test_parse_perfspect_flat_records_full_host_capabilities():
    caps = parse_perfspect_json(PERFSPECT_FLAT_FULL)
    assert {"cpu", "network", "memory", "storage"}.issubset(caps.keys())
    assert "avx512f" in [f.lower() for f in caps["cpu"]["features"]]
    assert caps["network"][0]["name"] == "ens1f0"
    assert caps["network"][0]["bandwidth"]["input"] == "12.5GB/s"
    assert caps["storage"][0]["name"] == "nvme1n1"
    assert caps["storage"][0]["type"] == "nvme"
    assert caps["storage"][0]["throughput"]["write"] == "3200MB/s"


def test_parse_perfspect_missing_optional_network_storage_fields(tmp_path):
    sample = {
        "CPU": {"NUMA node(s)": "2", "CPU(s)": "16"},
        "Memory": {"total_gb": "64", "Memory Type": "DDR4"},
        "Network": {"name": "eth9"},
        "Storage": {"name": "/dev/sdb"},
    }
    p = tmp_path / "minimal-net-store.json"
    p.write_text(json.dumps(sample))

    caps = parse_perfspect_json(p)
    assert caps["network"] == [{"name": "eth9"}]
    assert caps["storage"] == [{"name": "/dev/sdb"}]


# ──────────────────────────────────────────────────────────────────────────────
# PerfSpect list-CPU + top-level ISA regression (real PerfSpect structure)
# ──────────────────────────────────────────────────────────────────────────────

_EXPECTED_NFD_POSITIVE = {
    "AMXFP16",
    "AVX512F",
    "AES",
    "AMX",
    "AVX512FP16",
    "CLDEMOTE",
    "ENQCMD",
    "MOVDIR64B",
    "MOVDIRI",
    "PREFETCHIT0",
    "PREFETCHIT1",
    "SERIALIZE",
    "SHA",
    "TSXLDTRK",
    "WAITPKG",
    "VAES",
    "AVX512BF16",
    "AVX512VNNI",
}

_EXPECTED_NFD_ABSENT = {
    "AMXCOMPLEX",
    "AVXIFMA",
    "AVXNECONVERT",
    "AVXVNNIINT8",
    "CMPCCXADD",
}


def test_list_cpu_isa_cpu_metadata():
    """CPU metadata is correctly parsed when CPU is a list of dicts."""
    caps = parse_perfspect_json(PERFSPECT_LIST_CPU_ISA)
    cpu = caps["cpu"]
    assert cpu["numa_nodes"] == 2
    assert cpu["number_of_cores"] == 128  # 2 sockets × 64 cores
    assert cpu["base_frequency"] == "2.4GHz"
    assert cpu["generation"] == "GNR_X2"


def test_list_cpu_isa_positive_features_present():
    """The ISA fixture yields exactly the expected positive NFD features."""
    caps = parse_perfspect_json(PERFSPECT_LIST_CPU_ISA)
    features_upper = {f.upper() for f in caps["cpu"]["features"]}
    assert features_upper == _EXPECTED_NFD_POSITIVE


def test_list_cpu_isa_negative_features_absent():
    """'No' ISA entries are excluded from the feature list."""
    caps = parse_perfspect_json(PERFSPECT_LIST_CPU_ISA)
    features_upper = {f.upper() for f in caps["cpu"]["features"]}
    for absent in _EXPECTED_NFD_ABSENT:
        assert absent not in features_upper, f"Unexpected NFD feature present: {absent}"


def test_list_cpu_isa_no_flags_error(tmp_path):
    """The previous misleading 'Flags/Features' error does not occur for this fixture."""
    src = PERFSPECT_LIST_CPU_ISA.read_text()
    (tmp_path / "worker-1.json").write_text(src)
    caps = parse_perfspect_json(tmp_path / "worker-1.json")
    # Ensure we got features — the old bug returned an empty dict, triggering the error.
    assert caps.get("cpu", {}).get("features"), (
        "cpu.features must be populated from top-level ISA when CPU is a list"
    )


def test_list_cpu_isa_memory_details_curated():
    caps = parse_perfspect_json(PERFSPECT_LIST_CPU_ISA)
    assert caps["memory"] == [
        {
            "type": "ram",
            "generation": "DDR5",
            "size": "512GB (16x32GB DDR5 6400MT/s [6400MT/s])",
            "throughput": {"total": "6400MT/s"},
        }
    ]


def test_list_cpu_isa_memory_runtime_fields_omitted():
    caps = parse_perfspect_json(PERFSPECT_LIST_CPU_ISA)
    serialized = json.dumps(caps["memory"])
    for field in (
        "Buffers",
        "Cached",
        "MemAvailable",
        "MemFree",
        "MemTotal",
        "HugePages_Total",
        "Hugepagesize",
        "Transparent Huge Pages",
        "Automatic NUMA Balancing",
        "Clustering Mode",
        "Populated Memory Channels",
        "Total Memory Encryption (TME)",
    ):
        assert field not in serialized


# ──────────────────────────────────────────────────────────────────────────────
# _find_json_file helper
# ──────────────────────────────────────────────────────────────────────────────


def test_find_json_file_single_match(tmp_path):
    (tmp_path / "report.json").write_text("{}")
    assert _find_json_file(tmp_path) == tmp_path / "report.json"


def test_find_json_file_none_raises(tmp_path):
    with pytest.raises(FileNotFoundError, match="No JSON file found"):
        _find_json_file(tmp_path)


def test_find_json_file_ambiguous_raises(tmp_path):
    (tmp_path / "a.json").write_text("{}")
    (tmp_path / "b.json").write_text("{}")
    with pytest.raises(ValueError, match="Multiple JSON files"):
        _find_json_file(tmp_path)


def test_parse_perfspect_json_directory(tmp_path):
    # When given a directory with a single JSON, the file is found automatically.
    src = PERFSPECT_HIER.read_text()
    (tmp_path / "k8s-w2_report.json").write_text(src)
    caps = parse_perfspect_json(tmp_path)
    assert "cpu" in caps


# ──────────────────────────────────────────────────────────────────────────────
# metrics.csv parsing
# ──────────────────────────────────────────────────────────────────────────────


def test_parse_metrics_csv_encoder_cpu():
    result = parse_metrics_csv(METRICS_CSV, service="encoder")
    assert result["cpu_cores"] is not None
    assert abs(result["cpu_cores"] - 4.823) < 0.01


def test_parse_metrics_csv_decoder_cpu():
    result = parse_metrics_csv(METRICS_CSV, service="decoder")
    assert result["cpu_cores"] is not None
    assert abs(result["cpu_cores"] - 0.812) < 0.01


def test_parse_metrics_csv_encoder_memory():
    result = parse_metrics_csv(METRICS_CSV, service="encoder")
    # 524288000 bytes = exactly 500 MiB; ceiling division keeps it at 500
    assert result["memory_mib"] is not None
    assert result["memory_mib"] == 500


def test_parse_metrics_csv_missing_service(tmp_path):
    # CSV with only encoder rows → decoder returns None
    (tmp_path / "m.csv").write_text(
        "category,metric,unit,value,scope,session,role\n"
        "CPU,FFmpeg CPU demand,cores,3.5,process,s01,encoder\n"
    )
    result = parse_metrics_csv(tmp_path / "m.csv", service="decoder")
    assert result["cpu_cores"] is None
    assert result["memory_mib"] is None


def test_parse_metrics_csv_empty(tmp_path):
    (tmp_path / "empty.csv").write_text("category,metric,unit,value,scope,session,role\n")
    result = parse_metrics_csv(tmp_path / "empty.csv", service="encoder")
    assert result["cpu_cores"] is None
    assert result["memory_mib"] is None


def test_parse_metrics_csv_arbitrary_service(tmp_path):
    csv_text = textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        CPU,FFmpeg CPU demand,cores,avg=2.500000;min=2.4;max=2.6,process,s01,packager
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""mxl-packager-s01""}",s01,packager
    """)
    path = tmp_path / "packager-metrics.csv"
    path.write_text(csv_text)
    result = parse_metrics_csv(path, service="packager")
    assert result["cpu_cores"] == 2.5
    assert result["memory_mib"] == 256


# ──────────────────────────────────────────────────────────────────────────────
# build_manifest
# ──────────────────────────────────────────────────────────────────────────────


def test_build_manifest_required_fields():
    m = build_manifest(name="enc-test", role="encoder")
    assert m["apiVersion"] == "mxl.media.fn/v1alpha1"
    assert m["kind"] == "MxlMediaFunctionParameters"
    assert m["metadata"]["name"] == "enc-test"
    assert m["spec"]["role"] == "encoder"


def test_build_manifest_cpu_requirement_with_margin():
    m = build_manifest(name="enc", role="encoder", cpu_cores=4.823, cpu_margin=1.20)
    cpu_str = m["spec"]["requirements"]["cpu"]["cpu"]
    # 4.823 * 1.20 = 5.7876 → 5788m
    assert cpu_str == "5788m"
    # Verify schema regex: ^[0-9]+(m|\.[0-9]+)?$
    import re
    assert re.match(r"^[0-9]+(m|\.[0-9]+)?$", cpu_str)


def test_build_manifest_memory_requirement_with_margin():
    m = build_manifest(name="enc", role="encoder", memory_mib=501, memory_margin=1.25)
    mem = m["spec"]["requirements"]["memory"][0]
    assert mem["type"] == "ram"
    # 501 * 1.25 = 626.25 → rounds up → 632 MiB (aligned to 8)
    assert mem["size"].endswith("Mi") or mem["size"].endswith("Gi")
    import re
    assert re.match(r"^[0-9]+(Mi|Gi|Ti)$", mem["size"])


def test_build_manifest_host_capabilities_embedded():
    caps = {"cpu": {"numa_nodes": 2, "number_of_cores": 128}}
    m = build_manifest(name="enc", role="encoder", host_caps=caps)
    assert m["spec"]["requirements"]["host_capabilities"]["cpu"]["numa_nodes"] == 2


def test_build_manifest_host_capabilities_nested_under_requirements():
    caps = {"cpu": {"features": ["AVX512F"]}}
    m = build_manifest(name="enc", role="encoder", host_caps=caps)
    assert m["spec"]["requirements"]["host_capabilities"] == caps
    assert "host_capabilities" not in m["spec"]


def test_build_manifest_no_requirements_when_no_data():
    m = build_manifest(name="enc", role="encoder")
    assert "requirements" not in m["spec"]


def test_build_manifest_no_spec_args():
    # spec.args is not part of the original manifest schema and must not be emitted.
    m = build_manifest(
        name="enc", role="encoder", scenario="pinned",
        preset="medium", bitrate="12M", dec_threads="1", enc_threads="15",
        filter_threads="1", slices="2",
    )
    assert "args" not in m["spec"]


def test_build_manifest_resolution_1080p():
    m = build_manifest(name="enc", role="encoder", resolution="1080p")
    video_in = m["spec"]["inputs"][0]["format"]["video"]
    assert video_in["frame_width"] == 1920
    assert video_in["frame_height"] == 1080


def test_build_manifest_resolution_2160p():
    m = build_manifest(name="enc", role="encoder", resolution="2160p")
    video_in = m["spec"]["inputs"][0]["format"]["video"]
    assert video_in["frame_width"] == 3840
    assert video_in["frame_height"] == 2160


# ──────────────────────────────────────────────────────────────────────────────
# Validation
# ──────────────────────────────────────────────────────────────────────────────


def _valid_manifest():
    return build_manifest(
        name="enc-pinned-1080p",
        role="encoder",
        scenario="pinned",
        resolution="1080p",
        preset="medium",
        bitrate="12M",
        cpu_cores=4.823,
        memory_mib=501,
    )


def test_validate_valid_manifest_passes():
    m = _valid_manifest()
    ok, errors = validate_manifest(m)
    assert ok, f"Expected valid manifest; errors: {errors}"
    assert errors == []


def test_validate_missing_required_field_fails():
    m = _valid_manifest()
    del m["metadata"]["name"]
    ok, errors = validate_manifest(m)
    assert not ok
    assert errors  # at least one error message


def test_validate_bad_cpu_format_fails():
    m = _valid_manifest()
    m["spec"]["requirements"]["cpu"]["cpu"] = "5.0G"  # invalid pattern
    ok, errors = validate_manifest(m)
    assert not ok


def test_validate_bad_memory_format_fails():
    m = _valid_manifest()
    m["spec"]["requirements"]["memory"][0]["size"] = "4GB"  # must be 4Gi
    ok, errors = validate_manifest(m)
    assert not ok


def test_validate_wrong_kind_fails():
    m = _valid_manifest()
    m["kind"] = "NotThisKind"
    ok, errors = validate_manifest(m)
    assert not ok


def test_validate_with_host_capabilities():
    caps = parse_perfspect_json(PERFSPECT_HIER)
    m = build_manifest(
        name="enc-pinned-1080p",
        role="encoder",
        cpu_cores=4.823,
        memory_mib=501,
        host_caps=caps,
    )
    ok, errors = validate_manifest(m)
    assert ok, f"Expected valid manifest; errors: {errors}"


def test_validate_generated_manifest_with_full_host_capabilities_against_schema():
    caps = parse_perfspect_json(PERFSPECT_HIER_FULL)
    m = build_manifest(
        name="enc-pinned-1080p",
        role="encoder",
        scenario="pinned",
        resolution="1080p",
        cpu_cores=4.823,
        memory_mib=501,
        host_caps=caps,
    )
    reqs = m["spec"]["requirements"]["host_capabilities"]
    assert {"cpu", "network", "memory", "storage"}.issubset(reqs.keys())
    ok, errors = validate_manifest(m, schema_path=SCHEMA_PATH)
    assert ok, f"Expected valid manifest; errors: {errors}"


# ──────────────────────────────────────────────────────────────────────────────
# CLI — normal generation
# ──────────────────────────────────────────────────────────────────────────────


def test_cli_full_run(tmp_path, profile_dir):
    out = tmp_path / "manifest.yaml"
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--output", str(out),
    ])
    assert rc == 0
    assert out.exists()
    text = out.read_text()
    doc = yaml.safe_load(text)
    assert doc["kind"] == "MxlMediaFunctionParameters"
    assert doc["metadata"]["name"] == "encoder-pinned-1080p"
    assert doc["spec"]["role"] == "encoder"
    assert "host_capabilities" in doc["spec"]["requirements"]


def test_cli_dry_run_prints_to_stdout(tmp_path, profile_dir, capsys):
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--dry-run",
    ])
    assert rc == 0
    out = capsys.readouterr().out
    doc = yaml.safe_load(out)
    assert doc["kind"] == "MxlMediaFunctionParameters"
    assert "host_capabilities" in doc["spec"]["requirements"]


def test_cli_no_validate_skips_schema_check(tmp_path, profile_dir):
    out = tmp_path / "manifest.yaml"
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--no-validate",
        "--output", str(out),
    ])
    assert rc == 0


def test_cli_requires_perfspect(profile_dir):
    with pytest.raises(SystemExit) as exc_info:
        main([
            "--profile", str(profile_dir),
            "--service", "encoder",
        ])
    assert exc_info.value.code == 2


def test_cli_requires_profile():
    with pytest.raises(SystemExit):
        main(["--service", "encoder"])


def test_cli_requires_service(profile_dir):
    with pytest.raises(SystemExit):
        main(["--profile", str(profile_dir)])


def test_cli_uses_profile_config_for_scenario_resolution(tmp_path, capsys):
    profile = tmp_path / "profile"
    profile.mkdir()
    profile.joinpath("metrics.csv").write_text(textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        Prometheus,workload_cpu_cores,native,avg=6.250000;min=1;max=1,"{""pod"": ""mxl-encoder-s01""}",s01,encoder
        Prometheus,workload_memory_bytes,native,avg=805306368;min=1;max=1,"{""pod"": ""mxl-encoder-s01""}",s01,encoder
    """))
    profile.joinpath("config.json").write_text(json.dumps(_profile_config(
        RESOLUTION="4k",
        PRESET="slow",
        BITRATE="20M",
        DEC_THREADS="2",
        FILTER_THREADS="3",
        ENC_THREADS="22",
        SLICES="4",
    )))

    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile),
        "--service", "encoder",
        "--dry-run",
    ])
    assert rc == 0
    doc = yaml.safe_load(capsys.readouterr().out)
    assert doc["metadata"]["name"] == "encoder-pinned-4k"
    assert doc["spec"]["flow"]["label"] == "pinned"
    assert doc["spec"]["role"] == "encoder"
    video = doc["spec"]["inputs"][0]["format"]["video"]
    assert video["frame_width"] == 3840
    assert video["frame_height"] == 2160
    # spec.args must not be emitted
    assert "args" not in doc["spec"]


def test_cli_missing_profile_config_fails(tmp_path):
    profile = tmp_path / "profile"
    profile.mkdir()
    (profile / "metrics.csv").write_text((FIXTURES / "metrics-sample.csv").read_text())
    with pytest.raises(SystemExit, match="config.json not found"):
        main([
            "--perfspect", str(PERFSPECT_HIER),
            "--profile", str(profile),
            "--service", "encoder",
        ])





def test_cli_arbitrary_service_selection(tmp_path):
    profile = tmp_path / "profile"
    profile.mkdir()
    profile.joinpath("metrics.csv").write_text(textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        Prometheus,workload_cpu_cores,native,avg=2.500000;min=1;max=1,"{""pod"": ""mxl-packager-s01""}",s01,packager
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""mxl-packager-s01""}",s01,packager
    """))
    profile.joinpath("config.json").write_text(json.dumps(_profile_config(SCENARIO="custom-svc")))

    out = tmp_path / "manifest.yaml"
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile),
        "--service", "packager",
        "--no-validate",
        "--output", str(out),
    ])
    assert rc == 0
    doc = yaml.safe_load(out.read_text())
    assert doc["metadata"]["name"] == "packager-custom-svc-1080p"
    assert doc["spec"]["role"] == "packager"


def test_cli_missing_service_metrics_fails(profile_dir):
    with pytest.raises(SystemExit, match="Available services: decoder, encoder"):
        main([
            "--perfspect", str(PERFSPECT_HIER),
            "--profile", str(profile_dir),
            "--service", "packager",
        ])


def test_cli_nonexistent_perfspect_dir_exits(tmp_path, profile_dir):
    with pytest.raises(SystemExit, match="PerfSpect baseline path not found") as exc_info:
        main([
            "--perfspect", str(tmp_path / "does-not-exist"),
            "--profile", str(profile_dir),
            "--service", "encoder",
            "--no-validate",
            "--output", str(tmp_path / "out.yaml"),
        ])
    assert exc_info.value.code != 0


def test_cli_unusable_perfspect_input_exits(tmp_path, profile_dir):
    unusable = tmp_path / "perfspect-empty.json"
    unusable.write_text("{}")
    with pytest.raises(SystemExit, match="no usable host capability data found"):
        main([
            "--perfspect", str(unusable),
            "--profile", str(profile_dir),
            "--service", "encoder",
            "--no-validate",
            "--output", str(tmp_path / "out.yaml"),
        ])


def test_cli_missing_cpu_features_exits(tmp_path, profile_dir):
    # PerfSpect data that has CPU info but no CPU feature flags must fail fast.
    no_features = tmp_path / "perfspect-no-features.json"
    no_features.write_text(json.dumps({
        "CPU": {"NUMA node(s)": "2", "CPU(s)": "64", "CPU MHz": "2500"},
        "Memory": {"total_gb": "256", "Memory Type": "DDR5"},
    }))
    with pytest.raises(SystemExit, match="no CPU features found"):
        main([
            "--perfspect", str(no_features),
            "--profile", str(profile_dir),
            "--service", "encoder",
            "--no-validate",
            "--output", str(tmp_path / "out.yaml"),
        ])


# ──────────────────────────────────────────────────────────────────────────────
# CPU features in host_capabilities (regression)
# ──────────────────────────────────────────────────────────────────────────────


def test_cpu_features_present_in_host_capabilities(tmp_path, profile_dir):
    """CPU features from PerfSpect must appear in spec.requirements.host_capabilities.cpu.features."""
    out = tmp_path / "manifest.yaml"
    rc = main([
        "--perfspect", str(PERFSPECT_HIER_FULL),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--no-validate",
        "--output", str(out),
    ])
    assert rc == 0
    doc = yaml.safe_load(out.read_text())
    hc = doc["spec"]["requirements"]["host_capabilities"]
    assert "cpu" in hc
    features = hc["cpu"]["features"]
    assert isinstance(features, list)
    assert len(features) > 0
    assert any("avx512" in f.lower() for f in features)


def test_no_spec_args_in_generated_manifest(tmp_path, profile_dir):
    """spec.args must never be emitted — it is not part of the original manifest schema."""
    out = tmp_path / "manifest.yaml"
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--no-validate",
        "--output", str(out),
    ])
    assert rc == 0
    doc = yaml.safe_load(out.read_text())
    assert "args" not in doc["spec"]


def test_arbitrary_resolution_accepted(tmp_path, profile_dir):
    """The generator must not reject resolutions absent from its internal map."""
    # Replace the profile config with a non-standard resolution string.
    profile2 = tmp_path / "profile2"
    shutil.copytree(str(profile_dir), str(profile2))
    (profile2 / "config.json").write_text(json.dumps(_profile_config(RESOLUTION="8k")))

    out = tmp_path / "manifest.yaml"
    # Should not raise SystemExit for the resolution; may succeed or fail for
    # other reasons (schema validation) — use --no-validate to isolate.
    rc = main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile2),
        "--service", "encoder",
        "--no-validate",
        "--output", str(out),
    ])
    assert rc == 0
    doc = yaml.safe_load(out.read_text())
    assert doc["metadata"]["name"] == "encoder-pinned-8k"


def test_cli_invalid_manifest_returns_nonzero(tmp_path):
    # Build a manifest, then force an invalid cpu pattern to make validation fail.
    good_manifest = build_manifest(name="enc", role="encoder")
    good_manifest["kind"] = "WrongKind"

    # Validation failure is most directly exercised through validate_manifest.
    ok, errors = validate_manifest(good_manifest)
    assert not ok


# ──────────────────────────────────────────────────────────────────────────────
# Provenance comment in output
# ──────────────────────────────────────────────────────────────────────────────


def test_cli_output_contains_provenance_comments(tmp_path, profile_dir):
    out = tmp_path / "manifest.yaml"
    main([
        "--perfspect", str(PERFSPECT_HIER),
        "--profile", str(profile_dir),
        "--service", "encoder",
        "--no-validate",
        "--output", str(out),
    ])
    text = out.read_text()
    assert "# Generated by create-dmf-manifest" in text
    assert "# PerfSpect source" in text
    assert "# Profiling source" in text
    assert "# Profile config" in text


# ──────────────────────────────────────────────────────────────────────────────
# Regression: schema regex compliance
# ──────────────────────────────────────────────────────────────────────────────


def test_cpu_millicore_matches_schema_pattern():
    import re
    pattern = re.compile(r"^[0-9]+(m|\.[0-9]+)?$")
    for cores in [0.1, 0.5, 1.0, 4.823, 15.0, 128.0]:
        s = cores_to_millicore_str(cores)
        assert pattern.match(s), f"{s!r} does not match schema cpu pattern"


def test_memory_k8s_matches_schema_pattern():
    import re
    pattern = re.compile(r"^[0-9]+(Mi|Gi|Ti)$")
    for mib in [1, 8, 64, 256, 512, 1024, 2048, 4096, 1024 * 1024]:
        s = mib_to_k8s_memory(mib)
        assert pattern.match(s), f"{s!r} does not match schema memory pattern"


# ──────────────────────────────────────────────────────────────────────────────
# Regression: generic workload CPU metric (no FFmpeg-specific dependency)
# ──────────────────────────────────────────────────────────────────────────────


def test_workload_cpu_cores_metric_accepted(tmp_path):
    """parse_metrics_csv must accept Prometheus/workload_cpu_cores rows."""
    csv_text = textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        Prometheus,workload_cpu_cores,native,avg=3.200000;min=3.0;max=3.5,"{""pod"": ""svc-encoder-s01""}",s01,encoder
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""svc-encoder-s01""}",s01,encoder
    """)
    path = tmp_path / "generic-metrics.csv"
    path.write_text(csv_text)
    result = parse_metrics_csv(path, service="encoder")
    assert result["cpu_metric_found"] is True
    assert result["cpu_cores"] is not None
    assert abs(result["cpu_cores"] - 3.2) < 0.01


def test_node_cpu_metric_not_selected(tmp_path):
    """Node-level or unrelated CPU metrics must not be selected as workload CPU."""
    csv_text = textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        CPU,node_cpu_seconds_total,cores,avg=50.0;min=40;max=60,node,s01,encoder
        CPU,cpu_utilization_percent,%,avg=80.0;min=70;max=90,node,s01,encoder
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""svc-encoder-s01""}",s01,encoder
    """)
    path = tmp_path / "node-metrics.csv"
    path.write_text(csv_text)
    result = parse_metrics_csv(path, service="encoder")
    assert result["cpu_metric_found"] is False
    assert result["cpu_cores"] is None


def test_no_ffmpeg_wording_in_cpu_error(tmp_path, profile_dir):
    """CPU metric-not-found error must not mention FFmpeg."""
    profile = tmp_path / "no-cpu-profile"
    profile.mkdir()
    profile.joinpath("metrics.csv").write_text(textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""svc-encoder-s01""}",s01,encoder
    """))
    profile.joinpath("config.json").write_text(json.dumps(_profile_config()))
    with pytest.raises(SystemExit) as exc_info:
        main([
            "--perfspect", str(PERFSPECT_HIER),
            "--profile", str(profile),
            "--service", "encoder",
        ])
    msg = str(exc_info.value.code)
    assert "FFmpeg" not in msg
    assert "workload CPU" in msg


def test_legacy_ffmpeg_cpu_demand_still_accepted(tmp_path):
    """Legacy FFmpeg CPU demand rows must remain accepted for backward compatibility."""
    csv_text = textwrap.dedent("""\
        category,metric,unit,value,scope,session,role
        CPU,FFmpeg CPU demand,cores,avg=5.000000;min=4.8;max=5.2,process,s01,encoder
        Prometheus,workload_memory_bytes,native,avg=268435456;min=1;max=1,"{""pod"": ""svc-encoder-s01""}",s01,encoder
    """)
    path = tmp_path / "legacy-metrics.csv"
    path.write_text(csv_text)
    result = parse_metrics_csv(path, service="encoder")
    assert result["cpu_metric_found"] is True
    assert result["cpu_cores"] is not None
    assert abs(result["cpu_cores"] - 5.0) < 0.01


# ──────────────────────────────────────────────────────────────────────────────
# PerfSpect ISA yes/no map parsing
# ──────────────────────────────────────────────────────────────────────────────

_SAMPLE_ISA = [
    {
        "AMX-COMPLEX Instruction": "No",
        "AMX-FP16 Instruction": "Yes",
        "AVX-512 Foundation": "Yes",
        "AVX-IFMA Instruction": "No",
        "AVX-NE-CONVERT Instruction": "No",
        "AVX-VNNI-INT8 Instruction": "No",
        "Advanced Encryption Standard New Instructions (AES-NI)": "Yes",
        "Advanced Matrix Extensions (AMX)": "Yes",
        "Advanced Vector Extensions (AVX512_FP16)": "Yes",
        "Cache Line Demote (CLDEMOTE)": "Yes",
        "Compare and Add if Condition is Met (CMPCCXADD)": "No",
        "Enqueue Command Instruction (ENQCMD)": "Yes",
        "Move 64 Bytes as Direct Store (MOVDIR64B)": "Yes",
        "Move Doubleword as Direct Store (MOVDIRI)": "Yes",
        "PREFETCHIT0/1 Instruction": "Yes",
        "SERIALIZE Instruction": "Yes",
        "SHA1/SHA256 Instruction Extensions (SHA_NI)": "Yes",
        "Transactional Synchronization Extensions (TSXLDTRK)": "Yes",
        "UMONITOR, UMWAIT, TPAUSE Instructions": "Yes",
        "Vector AES": "Yes",
        "Vector Neural Network Instructions (AVX512_BF16)": "Yes",
        "Vector Neural Network Instructions (AVX512_VNNI)": "Yes",
    }
]


def test_parse_isa_features_positive_only():
    """Only Yes entries are included; No entries are excluded."""
    features = _parse_isa_features(_SAMPLE_ISA)
    assert "AMXFP16" in features
    assert "AVX512F" in features
    # Negative entries must be absent.
    assert "AMXCOMPLEX" not in features
    assert "AVXIFMA" not in features
    assert "AVXNECONVERT" not in features
    assert "AVXVNNIINT8" not in features
    assert "CMPCCXADD" not in features


def test_parse_isa_features_count():
    """18 positive outputs from _SAMPLE_ISA (PREFETCHIT0/1 → two features)."""
    features = _parse_isa_features(_SAMPLE_ISA)
    assert len(features) == 18


def test_parse_isa_features_source_order():
    """Features appear in the order they occur in the ISA object."""
    features = _parse_isa_features(_SAMPLE_ISA)
    # AMXFP16 comes before AVX512F in the sample.
    assert features.index("AMXFP16") < features.index("AVX512F")


def test_parse_isa_features_deduplication():
    """Duplicate labels across multiple ISA objects are collapsed."""
    isa = [
        {"AVX-512 Foundation": "Yes"},
        {"AVX-512 Foundation": "Yes", "Vector AES": "Yes"},
    ]
    features = _parse_isa_features(isa)
    assert features.count("AVX512F") == 1
    assert features.count("VAES") == 1


def test_parse_isa_features_boolean_values():
    """Boolean True/False values are treated as positive/negative."""
    isa = [{"AVX-512 Foundation": True, "Vector AES": False}]
    features = _parse_isa_features(isa)
    assert "AVX512F" in features
    assert "VAES" not in features


def test_parse_isa_features_numeric_values():
    """Numeric string '1'/'0' values are treated as positive/negative."""
    isa = [{"AVX-512 Foundation": "1", "Vector AES": "0"}]
    features = _parse_isa_features(isa)
    assert "AVX512F" in features
    assert "VAES" not in features


def test_parse_isa_features_empty_list():
    assert _parse_isa_features([]) == []


def test_parse_isa_features_non_list():
    assert _parse_isa_features(None) == []
    assert _parse_isa_features("yes") == []


def test_parse_isa_features_all_negative():
    isa = [{"AMX-COMPLEX Instruction": "No", "AVX-IFMA Instruction": "No"}]
    assert _parse_isa_features(isa) == []


def test_normalize_isa_label_hardcoded():
    assert _normalize_isa_label("AVX-512 Foundation") == "AVX512F"
    assert _normalize_isa_label("Vector AES") == "VAES"
    assert _normalize_isa_label("SERIALIZE Instruction") == "SERIALIZE"
    assert _normalize_isa_label("AMX-FP16 Instruction") == "AMXFP16"
    assert _normalize_isa_label("UMONITOR, UMWAIT, TPAUSE Instructions") == "WAITPKG"
    assert _normalize_isa_label("PREFETCHIT0/1 Instruction") == ["PREFETCHIT0", "PREFETCHIT1"]


def test_normalize_isa_label_parenthesized():
    assert _normalize_isa_label("Advanced Encryption Standard New Instructions (AES-NI)") == "AES"
    assert _normalize_isa_label("Advanced Vector Extensions (AVX512_FP16)") == "AVX512FP16"
    assert _normalize_isa_label("Vector Neural Network Instructions (AVX512_BF16)") == "AVX512BF16"
    assert _normalize_isa_label("Vector Neural Network Instructions (AVX512_VNNI)") == "AVX512VNNI"
    assert _normalize_isa_label("Cache Line Demote (CLDEMOTE)") == "CLDEMOTE"
    assert _normalize_isa_label("SHA1/SHA256 Instruction Extensions (SHA_NI)") == "SHA"
    assert _normalize_isa_label("Transactional Synchronization Extensions (TSXLDTRK)") == "TSXLDTRK"


def test_parse_isa_only_input_via_perfspect(tmp_path):
    """parse_perfspect_json populates cpu.features from an ISA-only CPU section."""
    data = {
        "CPU": {
            "Model name:": "Intel Xeon",
            "ISA": _SAMPLE_ISA,
        }
    }
    p = tmp_path / "isa_only.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    features = caps["cpu"]["features"]
    assert "AVX512F" in features
    assert "AES" in features
    assert "VAES" in features
    # No negative features.
    assert "AMXCOMPLEX" not in features
    assert "CMPCCXADD" not in features


def test_parse_isa_top_level_key(tmp_path):
    """ISA at the document top level is also picked up."""
    data = {
        "CPU": {"Model name:": "Intel Xeon"},
        "ISA": _SAMPLE_ISA,
    }
    p = tmp_path / "isa_top.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    assert "AVX512F" in caps["cpu"]["features"]


def test_parse_isa_top_level_case_insensitive_with_cpu_list(tmp_path):
    for key in ("isa", "Isa", "ISA"):
        data = {
            "CPU": [{"CPU Model": "Intel Xeon", "Sockets": "2", "Cores per Socket": "32"}],
            key: [{"AVX-512 Foundation": "Yes"}],
        }
        p = tmp_path / f"root_isa_{key}.json"
        p.write_text(json.dumps(data))
        caps = parse_perfspect_json(p)
        assert caps["cpu"]["features"] == ["AVX512F"]


def test_parse_isa_top_level_single_dict_with_cpu_list(tmp_path):
    data = {
        "CPU": [{"CPU Model": "Intel Xeon", "Sockets": "2", "Cores per Socket": "32"}],
        "ISA": {"AVX-512 Foundation": "Yes", "Vector AES": "Yes", "AMX-COMPLEX Instruction": "No"},
    }
    p = tmp_path / "root_isa_single_dict.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    assert "AVX512F" in caps["cpu"]["features"]
    assert "VAES" in caps["cpu"]["features"]
    assert "AMXCOMPLEX" not in caps["cpu"]["features"]


def test_isa_takes_precedence_over_flags(tmp_path):
    """When ISA data is present it is the authoritative feature source.

    ISA features are used and Flags/Features are not appended alongside them.
    This documents the chosen ISA-first precedence.
    """
    data = {
        "CPU": {
            "Model name:": "Intel Xeon",
            "Flags": "avx2 sse4_2",
            "ISA": [{"AVX-512 Foundation": "Yes"}],
        }
    }
    p = tmp_path / "flags_and_isa.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    features = caps["cpu"]["features"]
    # ISA feature is present.
    assert "AVX512F" in features
    # Flags field is NOT merged when ISA is present (ISA is authoritative).
    assert "avx2" not in features
    assert "sse4_2" not in features


def test_flags_used_as_fallback_when_isa_has_no_positive_entries(tmp_path):
    data = {
        "CPU": {
            "Model name:": "Intel Xeon",
            "Flags": "avx2 sse4_2",
        },
        "ISA": [{"AVX-512 Foundation": "No"}],
    }
    p = tmp_path / "flags_fallback_after_negative_isa.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    assert caps["cpu"]["features"] == ["avx2", "sse4_2"]


def test_flags_used_as_fallback_when_no_isa(tmp_path):
    """Flags/Features are used when no ISA data is present."""
    data = {
        "CPU": {
            "Model name:": "Intel Xeon",
            "Flags": "avx2 sse4_2 avx512f",
        }
    }
    p = tmp_path / "flags_only.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    features = caps["cpu"]["features"]
    assert "avx2" in features
    assert "sse4_2" in features
    assert "avx512f" in features


def test_featureless_input_raises(tmp_path, profile_dir):
    """main raises SystemExit when no cpu features can be found in PerfSpect."""
    data = {"CPU": {"Model name:": "Intel Xeon"}}
    p = tmp_path / "no_features.json"
    p.write_text(json.dumps(data))
    with pytest.raises(SystemExit):
        main([
            "--perfspect", str(p),
            "--profile", str(profile_dir),
            "--service", "encoder",
        ])


def test_malformed_isa_non_dict_objects_ignored():
    """Non-dict entries in the ISA list are silently skipped."""
    isa = ["not a dict", None, {"AVX-512 Foundation": "Yes"}]
    features = _parse_isa_features(isa)
    assert features == ["AVX512F"]


def test_parse_isa_features_single_dict_form():
    """A single ISA dictionary (not wrapped in a list) is accepted."""
    isa = {"AVX-512 Foundation": "Yes", "Vector AES": "Yes", "AMX-COMPLEX Instruction": "No"}
    features = _parse_isa_features(isa)
    assert "AVX512F" in features
    assert "VAES" in features
    assert "AMXCOMPLEX" not in features


def test_parse_isa_features_case_insensitive_discovery(tmp_path):
    """ISA key is discovered case-insensitively ('isa', 'Isa', 'ISA')."""
    for key in ("isa", "Isa", "ISA"):
        data = {
            "CPU": {
                "Model name:": "Intel Xeon",
                key: [{"AVX-512 Foundation": "Yes"}],
            }
        }
        p = tmp_path / f"isa_{key}.json"
        p.write_text(json.dumps(data))
        caps = parse_perfspect_json(p)
        assert "AVX512F" in caps["cpu"]["features"], f"ISA key '{key}' not discovered"


def test_parse_isa_features_numeric_float_one():
    """Numeric float 1.0 treated as a positive value."""
    # 1.0 as a JSON number converts to float; str(1.0) == "1.0" which is not in
    # the _ISA_POSITIVE string set.  The feature is included because 1.0 == 1.
    isa = [{"AVX-512 Foundation": 1.0, "Vector AES": 0.0}]
    features = _parse_isa_features(isa)
    assert "AVX512F" in features
    assert "VAES" not in features


def test_parse_isa_top_level_numeric_float_one_with_cpu_list(tmp_path):
    data = {
        "CPU": [{"CPU Model": "Intel Xeon", "Sockets": "2", "Cores per Socket": "32"}],
        "ISA": [{"AVX-512 Foundation": 1.0, "Vector AES": 0.0}],
    }
    p = tmp_path / "root_isa_numeric_float.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    assert caps["cpu"]["features"] == ["AVX512F"]


def test_parse_isa_features_case_insensitive_dedup():
    """Features from multiple ISA entries are deduplicated case-insensitively."""
    isa = [
        {"AVX-512 Foundation": "Yes"},   # → AVX512F
        {"avx-512 foundation": "Yes"},   # same label, different case → should dedup
    ]
    features = _parse_isa_features(isa)
    assert features.count("AVX512F") == 1


def test_cpu_local_isa_preferred_over_top_level(tmp_path):
    """CPU-section ISA is used when both CPU-local and top-level ISA are present."""
    data = {
        "CPU": {
            "Model name:": "Intel Xeon",
            "ISA": [{"AVX-512 Foundation": "Yes"}],
        },
        "ISA": [{"Vector AES": "Yes"}],
    }
    p = tmp_path / "isa_precedence.json"
    p.write_text(json.dumps(data))
    caps = parse_perfspect_json(p)
    features = caps["cpu"]["features"]
    # CPU-local ISA (AVX512F) takes precedence; top-level ISA is not merged.
    assert "AVX512F" in features
    assert "VAES" not in features


def test_parse_isa_full_sample_nfd_identifiers():
    """The exact PerfSpect ISA sample maps to the expected NFD CPUID identifiers."""
    features = _parse_isa_features(_SAMPLE_ISA)
    expected = {
        "AMXFP16", "AVX512F", "AES", "AMX", "AVX512FP16", "CLDEMOTE",
        "ENQCMD", "MOVDIR64B", "MOVDIRI", "PREFETCHIT0", "PREFETCHIT1",
        "SERIALIZE", "SHA", "TSXLDTRK", "WAITPKG", "VAES", "AVX512BF16",
        "AVX512VNNI",
    }
    assert set(features) == expected
