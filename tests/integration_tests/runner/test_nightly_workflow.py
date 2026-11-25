# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""End-to-end self-test harness for the nightly gtest + nightly pytest CI workflow.

Tests the full nightly pipeline locally in seconds without bare-metal NICs:
  1. profiles.yaml <-> nightly.yml <-> run-gtest/pytest-collect/reports.sh contracts
  2. Sharded nightly gtest via .github/scripts/gtest.sh (fake KahawaiTest + sysfs)
  3. Unified structured results.jsonl output from nightly pytest
  4. reports.sh flatten-gtest / flatten-pytest + combine_all_reports.py + regression
     detection (including profile-scoped gtest regressions)
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import yaml

RUNNER_DIR = Path(__file__).resolve().parent
REPO_ROOT = RUNNER_DIR.parent.parent.parent
PROFILES_YAML = REPO_ROOT / "tests/integration_tests/profiles.yaml"
NIGHTLY_YML = REPO_ROOT / ".github/workflows/nightly.yml"
RUN_GTEST_YML = REPO_ROOT / ".github/actions/run-gtest/action.yml"
PYTEST_COLLECT_YML = REPO_ROOT / ".github/actions/pytest-collect/action.yml"
REPORTS_SH = REPO_ROOT / ".github/scripts/ci/report/reports.sh"
COMBINE_PY = REPO_ROOT / ".github/scripts/combine_all_reports.py"

sys.path.insert(0, str(REPO_ROOT / ".github/scripts"))
sys.path.insert(0, str(RUNNER_DIR))


def _exec(path: Path, content: str) -> Path:
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def _fake_sysfs(tmp_path: Path) -> tuple[Path, Path]:
    pci, hp = tmp_path / "pci", tmp_path / "hp"
    (hp / "hugepages-2048kB").mkdir(parents=True)
    (hp / "hugepages-2048kB/free_hugepages").write_text("1024", encoding="utf-8")
    drv = tmp_path / "drivers/ice"
    drv.mkdir(parents=True)
    for pf, ifn in (("0000:4b:00.0", "ens1f0"), ("0000:4b:00.1", "ens1f1")):
        d = pci / pf
        (d / "net" / ifn).mkdir(parents=True)
        (d / "class").write_text("0x020000\n", encoding="utf-8")
        (d / "device").write_text("0x1592\n", encoding="utf-8")
        (d / "net" / ifn / "operstate").write_text("up\n", encoding="utf-8")
        os.symlink(drv, d / "driver")
    return pci, hp


def _fake_nicctl(tmp_path: Path) -> Path:
    return _exec(
        tmp_path / "nicctl.sh",
        """#!/usr/bin/env bash
set -euo pipefail
if [ "$1" = list ]; then
  p="${2%.*}"
  printf '%s.1\\n%s.2\\n%s.3\\n%s.4\\n%s.5\\n%s.6\\n' "$p" "$p" "$p" "$p" "$p" "$p"
fi
""",
    )


def _fake_kahawai(tmp_path: Path, fail_on_arg: str = "") -> Path:
    return _exec(
        tmp_path / "KahawaiTest",
        f"""#!/usr/bin/env python3
import json, sys, xml.etree.ElementTree as ET
flt = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_filter=")), "")
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
if "--list-json" in sys.argv:
    print(json.dumps([
        {{"case": "St20p.same_case", "suite": "St20p", "instance": "shared",
          "topology": "vf_pair", "isolated": False}},
        {{"case": "NoCtxTest.st20p_user_pacing", "suite": "NoCtxTest", "instance": "own",
          "topology": "cross_pf_quad", "isolated": True}},
        {{"case": "NoCtxTest.st20p_pf_tsn", "suite": "NoCtxTest", "instance": "own",
          "topology": "pf_pair", "isolated": False}},
    ]))
    sys.exit(0)
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  st20p_pf_tsn" if "_pf_" in flt and "-NoCtxTest.*_pf_*" not in flt else "  st20p_user_pacing")
    sys.exit(0)
cases = ["St20p.same_case"] if "St20p" in flt else [flt if "." in flt else "Misc.ok"]
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="s")
fail = False
for c in cases:
    cls, nm = c.split(".", 1)
    tc = ET.SubElement(ts, "testcase", classname=cls, name=nm, time="0.05")
    if {fail_on_arg!r} and {fail_on_arg!r} in sys.argv:
        ET.SubElement(tc, "failure", message="pacing_mismatch_on_pa").text = "pacing_mismatch_on_pa"
        fail = True
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(1 if fail else 0)
""",
    )


def test_nightly_yaml_and_profiles_contract() -> None:
    """Verify profiles.yaml shards match nightly.yml and structured JSONL artifacts."""
    pdata = yaml.safe_load(PROFILES_YAML.read_text(encoding="utf-8"))
    all_runs = [r["name"] for tier in ("pr", "nightly") for r in pdata["runs"][tier]]
    groups = pdata.get("groups") or {}
    assert groups, "profiles.yaml must define nightly execution groups for sharding"

    grouped = [name for members in groups.values() for name in members]
    assert len(grouped) == len(set(grouped)), "profiles.yaml groups must not overlap"
    assert set(grouped) == set(all_runs), (
        f"profiles.yaml groups must partition all runs; "
        f"missing={set(all_runs) - set(grouped)}, extra={set(grouped) - set(all_runs)}"
    )

    nightly_text = NIGHTLY_YML.read_text(encoding="utf-8")
    for grp_name in groups:
        assert (
            grp_name in nightly_text
        ), f"nightly.yml run-gtest matrix missing group '{grp_name}'"

    run_gtest_text = RUN_GTEST_YML.read_text(encoding="utf-8")
    assert (
        "results.jsonl" in run_gtest_text
    ), "run-gtest action must upload results.jsonl"

    pytest_collect_text = PYTEST_COLLECT_YML.read_text(encoding="utf-8")
    assert (
        "results.jsonl" in pytest_collect_text
    ), "pytest-collect action must support results.jsonl"

    reports_text = REPORTS_SH.read_text(encoding="utf-8")
    assert (
        "results.jsonl" in reports_text
    ), "reports.sh flatten helpers must handle results.jsonl"


def test_nightly_gtest_sharded_via_shim(tmp_path: Path) -> None:
    """Run .github/scripts/gtest.sh across groups and verify deterministic outputs."""
    pci, hp = _fake_sysfs(tmp_path)
    nicctl = _fake_nicctl(tmp_path)
    binary = _fake_kahawai(tmp_path, fail_on_arg="--iova_mode")

    out_dir = tmp_path / "gtest_out"
    log_file = tmp_path / "gtest.log"
    env = {
        **os.environ,
        "NIGHTLY": "1",
        "EXIT_ON_FAILURE": "0",
        "GTEST_GROUP": "pacing_dma",
        "GTEST_OUT_DIR": str(out_dir),
        "LOG_FILE": str(log_file),
        "KAHAWAI_TEST_BINARY": str(binary),
        "MTL_NICCTL": str(nicctl),
        "MTL_NICCTL_NO_SUDO": "1",
        "MTL_NIC_LABEL": "E810",
        "RETRY_DELAY": "0",
        "MAX_RETRIES": "1",
    }
    proc = subprocess.run(
        [
            "bash",
            str(REPO_ROOT / ".github/scripts/gtest.sh"),
            "--no-sudo",
            "--quiet",
            "--tvf-pf",
            "0000:4b:00.0",
            "--sysfs-pci",
            str(pci),
            "--sysfs-hugepages",
            str(hp),
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode in (0, 1), proc.stderr
    jsonl = out_dir / "results.jsonl"
    assert jsonl.is_file(), "gtest.sh must write results.jsonl under GTEST_OUT_DIR"
    assert log_file.is_file(), "gtest.sh must also populate LOG_FILE for compatibility"
    recs = [
        json.loads(line)
        for line in jsonl.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    profiles_seen = {r["profile"] for r in recs}
    assert "st20p_auto_pacing_pa" in profiles_seen
    assert "st20p_tsc_pacing" in profiles_seen


def test_nightly_pytest_emits_unified_jsonl(tmp_path: Path) -> None:
    """Verify the pytest JSONL plugin emits the same schema as gtest_runner."""
    plugin_path = REPO_ROOT / ".github/scripts/report_generators/pytest_jsonl_plugin.py"
    assert plugin_path.is_file(), "Unified pytest_jsonl_plugin.py must exist"

    suite_file = tmp_path / "test_sample.py"
    suite_file.write_text(
        """
import pytest

def test_pass():
    assert 1 == 1

def test_fail():
    assert 1 == 2, "expected mismatch"

@pytest.mark.skip(reason="no hardware")
def test_skip():
    pass
""",
        encoding="utf-8",
    )
    out_jsonl = tmp_path / "results.jsonl"
    env = {
        **os.environ,
        "PYTHONPATH": str(plugin_path.parent),
        "MTL_PYTEST_JSONL": str(out_jsonl),
        "MTL_NIC_LABEL": "E810",
        "TEST_PATH": "st20p",
    }
    proc = subprocess.run(
        [
            sys.executable,
            "-m",
            "pytest",
            "-q",
            "-p",
            "no:cacheprovider",
            "-p",
            "pytest_jsonl_plugin",
            str(suite_file),
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert out_jsonl.is_file()
    recs = [
        json.loads(line)
        for line in out_jsonl.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    by_name = {r["case"].split("::")[-1]: r for r in recs}
    assert by_name["test_pass"]["result"] == "passed"
    assert by_name["test_fail"]["result"] == "failed"
    assert "expected mismatch" in by_name["test_fail"]["message"]
    assert by_name["test_skip"]["result"] == "skipped"
    assert by_name["test_pass"]["nic"] == "E810"
    assert by_name["test_pass"]["category"] == "st20p"


def test_nightly_flatten_combine_and_profile_regression_e2e(tmp_path: Path) -> None:
    """Simulate full combined-report job with current + baseline JSONL artifacts."""
    work = tmp_path / "work"
    for sub in (
        "gtest-reports/nightly-gtest-report-e810-pr_core",
        "gtest-reports/nightly-gtest-report-e810-pacing_dma",
        "baseline-gtest-reports/nightly-gtest-report-e810-pr_core",
        "baseline-gtest-reports/nightly-gtest-report-e810-pacing_dma",
        "pytest-reports/nightly-test-report-e810-st20p-1",
        "baseline-pytest-reports/nightly-test-report-e810-st20p-1",
    ):
        (work / sub).mkdir(parents=True)

    # Current gtest: St20p.same_case PASSES in st2110_20p but REGRESSES in st20p_auto_pacing_pa
    (work / "gtest-reports/nightly-gtest-report-e810-pr_core/results.jsonl").write_text(
        json.dumps(
            {
                "case": "St20p.same_case",
                "profile": "st2110_20p",
                "nic": "E810",
                "host": "r1",
                "result": "passed",
                "duration_ms": 50.0,
                "message": "",
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (
        work / "gtest-reports/nightly-gtest-report-e810-pacing_dma/results.jsonl"
    ).write_text(
        json.dumps(
            {
                "case": "St20p.same_case",
                "profile": "st20p_auto_pacing_pa",
                "nic": "E810",
                "host": "r1",
                "result": "failed",
                "duration_ms": 80.0,
                "message": "vrx overflow",
            }
        )
        + "\n",
        encoding="utf-8",
    )
    # Baseline gtest: BOTH profiles passed
    (
        work / "baseline-gtest-reports/nightly-gtest-report-e810-pr_core/results.jsonl"
    ).write_text(
        json.dumps(
            {
                "case": "St20p.same_case",
                "profile": "st2110_20p",
                "nic": "E810",
                "host": "r1",
                "result": "passed",
                "duration_ms": 48.0,
                "message": "",
            }
        )
        + "\n",
        encoding="utf-8",
    )
    (
        work
        / "baseline-gtest-reports/nightly-gtest-report-e810-pacing_dma/results.jsonl"
    ).write_text(
        json.dumps(
            {
                "case": "St20p.same_case",
                "profile": "st20p_auto_pacing_pa",
                "nic": "E810",
                "host": "r1",
                "result": "passed",
                "duration_ms": 75.0,
                "message": "",
            }
        )
        + "\n",
        encoding="utf-8",
    )
    # Pytest JSONL: current has 1 pass + 1 regression; baseline had 2 passes
    (work / "pytest-reports/nightly-test-report-e810-st20p-1/results.jsonl").write_text(
        "\n".join(
            [
                json.dumps(
                    {
                        "case": "test_st20p.py::test_ok",
                        "category": "st20p",
                        "nic": "E810",
                        "host": "r2",
                        "platform": "pytest",
                        "result": "passed",
                        "duration_ms": 1200.0,
                        "message": "",
                    }
                ),
                json.dumps(
                    {
                        "case": "test_st20p.py::test_reg",
                        "category": "st20p",
                        "nic": "E810",
                        "host": "r2",
                        "platform": "pytest",
                        "result": "failed",
                        "duration_ms": 900.0,
                        "message": "AssertionError: compliance failed",
                    }
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    (
        work / "baseline-pytest-reports/nightly-test-report-e810-st20p-1/results.jsonl"
    ).write_text(
        "\n".join(
            [
                json.dumps(
                    {
                        "case": "test_st20p.py::test_ok",
                        "category": "st20p",
                        "nic": "E810",
                        "host": "r2",
                        "platform": "pytest",
                        "result": "passed",
                        "duration_ms": 1100.0,
                        "message": "",
                    }
                ),
                json.dumps(
                    {
                        "case": "test_st20p.py::test_reg",
                        "category": "st20p",
                        "nic": "E810",
                        "host": "r2",
                        "platform": "pytest",
                        "result": "passed",
                        "duration_ms": 850.0,
                        "message": "",
                    }
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    for cmd, rdir in (
        ("flatten-gtest", work / "gtest-reports"),
        ("flatten-gtest", work / "baseline-gtest-reports"),
        ("flatten-pytest", work / "pytest-reports"),
        ("flatten-pytest", work / "baseline-pytest-reports"),
    ):
        subprocess.run(
            ["bash", str(REPORTS_SH), cmd],
            env={**os.environ, "REPORT_DIR": str(rdir)},
            check=True,
        )

    from report_generators.html_generator import generate_html_report
    from report_generators.parsers import parse_gtest_log, parse_pytest_jsonl
    from report_generators.regression_analyzer import analyze_regressions

    cur_gtest = [
        item
        for p in sorted((work / "gtest-reports").glob("*.jsonl"))
        for item in parse_gtest_log(p)
    ]
    base_gtest = [
        item
        for p in sorted((work / "baseline-gtest-reports").glob("*.jsonl"))
        for item in parse_gtest_log(p)
    ]
    cur_pytest = [
        parse_pytest_jsonl(p) for p in sorted((work / "pytest-reports").glob("*.jsonl"))
    ]
    base_pytest = [
        parse_pytest_jsonl(p)
        for p in sorted((work / "baseline-pytest-reports").glob("*.jsonl"))
    ]

    reg = analyze_regressions(cur_pytest, cur_gtest, base_pytest, base_gtest)
    reg_names = {r["test_name"] for r in reg["regressions"]}
    assert "[st20p_auto_pacing_pa] St20p.same_case" in reg_names, reg
    assert "test_st20p.py::test_reg" in reg_names, reg
    assert len(reg["regressions"]) == 2, reg

    out_html = work / "combined_nightly_report.html"
    generate_html_report(cur_pytest, cur_gtest, str(out_html), [], {}, reg)
    assert (
        out_html.is_file()
        and "[st20p_auto_pacing_pa] St20p.same_case"
        in out_html.read_text(encoding="utf-8")
    )

    try:
        import openpyxl  # noqa: F401
    except ImportError:
        return
    out_xlsx = work / "combined_nightly_report.xlsx"
    res = subprocess.run(
        [
            sys.executable,
            str(COMBINE_PY),
            "--pytest-dir",
            str(work / "pytest-reports"),
            "--gtest-dir",
            str(work / "gtest-reports"),
            "--baseline-pytest-dir",
            str(work / "baseline-pytest-reports"),
            "--baseline-gtest-dir",
            str(work / "baseline-gtest-reports"),
            "--output-excel",
            str(out_xlsx),
            "--output-html",
            str(out_html),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    assert out_xlsx.is_file() and "Regressions: 2," in res.stdout
