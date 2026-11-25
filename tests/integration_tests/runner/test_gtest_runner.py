# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Offline tests for the lean KahawaiTest runner and nic_setup.sh."""

from __future__ import annotations

import importlib.util
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

RUNNER_DIR = Path(__file__).resolve().parent


def _load_runner():
    spec = importlib.util.spec_from_file_location(
        "gtest_runner", RUNNER_DIR / "gtest_runner.py"
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


gr = _load_runner()


def _exec(path: Path, content: str) -> Path:
    path.write_text(content, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def _fake_sysfs(tmp_path: Path, hugepages: int = 1024) -> tuple[Path, Path]:
    pci, hp = tmp_path / "pci", tmp_path / "hp"
    (hp / "hugepages-2048kB").mkdir(parents=True)
    (hp / "hugepages-2048kB/free_hugepages").write_text(
        str(hugepages), encoding="utf-8"
    )
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


def _fake_nicctl(tmp_path: Path, log: Path) -> Path:
    return _exec(
        tmp_path / "nicctl.sh",
        f"""#!/usr/bin/env bash
set -euo pipefail
echo "$*" >> "{log}"
if [ "$1" = list ]; then
  p="${{2%.*}}"
  printf '%s.1\\n%s.2\\n%s.3\\n%s.4\\n%s.5\\n%s.6\\n' "$p" "$p" "$p" "$p" "$p" "$p"
fi
""",
    )


def _fake_bin(tmp_path: Path, state: Path) -> Path:
    return _exec(
        tmp_path / "KahawaiTest",
        f"""#!/usr/bin/env python3
import json, sys, xml.etree.ElementTree as ET
from pathlib import Path
st_p = Path("{state}")
st = json.loads(st_p.read_text()) if st_p.exists() else {{}}
flt = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_filter=")), "")
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  st20p_pf_tsn" if "_pf_" in flt and "-NoCtxTest.*_pf_*" not in flt else "  st20p_user_pacing")
    sys.exit(0)
cases = ["St20p.ok", "St20p.flaky", "St20p.fail"] if flt == "St20p*" else (["St30p.ok"] if flt == "St3*" else [flt])
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="s")
fail = False
for c in cases:
    cls, nm = c.split(".", 1)
    n = st.get(c, 0) + 1
    st[c] = n
    tc = ET.SubElement(ts, "testcase", classname=cls, name=nm, time="0.1")
    if (c == "St20p.flaky" and n == 1) or c == "St20p.fail":
        ET.SubElement(tc, "failure", message="boom").text = "boom"
        fail = True
st_p.write_text(json.dumps(st))
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(1 if fail else 0)
""",
    )


def _run(binary: Path, out: Path, pci: Path, hp: Path, *extra: str) -> int:
    return gr.main(
        [
            f"--binary={binary}",
            f"--out-dir={out}",
            "--no-sudo",
            "--quiet",
            f"--sysfs-pci={pci}",
            f"--sysfs-hugepages={hp}",
            *extra,
        ]
    )


def test_load_profiles_yaml() -> None:
    runs = gr.load_profiles()
    by_name = {r["name"]: r for r in runs}
    assert len(runs) == 26
    assert by_name["st2110_20_rx_shard0"]["env"]["GTEST_TOTAL_SHARDS"] == "2"
    assert "--multi_src_port" in by_name["st20p_auto_pacing_pa"]["extra_args"]
    assert by_name["st20p_kernel_loopback"]["needs_root"] is False
    assert by_name["noctx"]["mode"] == "noctx"
    assert by_name["noctx"]["env"]["MTL_ISOLATE"] == "require"
    assert by_name["noctx_pf"]["topology"] == "pf_pair"


def test_nic_setup_create_tvf_bind_pvf_and_validation(tmp_path: Path) -> None:
    log = tmp_path / "nic.log"
    env = {
        **os.environ,
        "MTL_NICCTL": str(_fake_nicctl(tmp_path, log)),
        "MTL_NICCTL_NO_SUDO": "1",
    }
    sh = str(RUNNER_DIR / "nic_setup.sh")
    assert (
        subprocess.check_output(
            [sh, "create-tvf", "0000:4b:00.0", "6"], env=env, text=True
        ).splitlines()[0]
        == "0000:4b:00.1"
    )
    assert subprocess.check_output(
        [sh, "bind-pvf", "0000:31:00.0", "0000:31:00.1"], env=env, text=True
    ).splitlines() == ["0000:31:00.0", "0000:31:00.1"]
    subprocess.check_call([sh, "restore-pvf", "0000:31:00.0"], env=env)
    subprocess.check_call([sh, "disable-vf", "0000:4b:00.0"], env=env)
    assert (
        subprocess.run(
            [sh, "bind-pvf", "bad"], env=env, capture_output=True, check=False
        ).returncode
        == 2
    )
    assert log.read_text(encoding="utf-8").splitlines() == [
        "create_tvf 0000:4b:00.0 6",
        "list 0000:4b:00.0",
        "bind_pmd 0000:31:00.0",
        "bind_pmd 0000:31:00.1",
        "bind_kernel 0000:31:00.0",
        "disable_vf 0000:4b:00.0",
    ]


def test_runner_tvf_pvf_flaky_and_continue(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    log = tmp_path / "nic.log"
    monkeypatch.setenv("MTL_NICCTL", str(_fake_nicctl(tmp_path, log)))
    monkeypatch.setenv("MTL_NICCTL_NO_SUDO", "1")
    out = tmp_path / "out"
    rc = _run(
        _fake_bin(tmp_path, tmp_path / "st.json"),
        out,
        pci,
        hp,
        f"--repo-root={tmp_path}",
        "--profile=^(st2110_20p|st2110_3x|noctx|noctx_pf)$",
        "--tvf-pf=0000:4b:00.0",
        "--bind-pvf=0000:31:00.0,0000:31:00.1",
        "--retry-delay=0",
    )
    assert rc == 1
    recs = {
        (r["profile"], r["case"]): r["result"]
        for r in map(json.loads, (out / "results.jsonl").read_text().splitlines())
    }
    assert recs[("st2110_20p", "St20p.ok")] == "passed"
    assert recs[("st2110_20p", "St20p.flaky")] == "flaky"
    assert recs[("st2110_20p", "St20p.fail")] == "failed"
    assert recs[("st2110_3x", "St30p.ok")] == "passed"
    assert recs[("noctx", "NoCtxTest.st20p_user_pacing")] == "passed"
    assert recs[("noctx_pf", "NoCtxTest.st20p_pf_tsn")] == "passed"
    assert "disable_vf 0000:4b:00.0" in log.read_text()
    assert "bind_kernel 0000:31:00.0" in log.read_text()


def test_runner_host_fault_and_direct_2port_pvf(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    pci, hp0 = _fake_sysfs(tmp_path, hugepages=0)
    monkeypatch.setenv("MTL_NICCTL", str(_fake_nicctl(tmp_path, tmp_path / "nic.log")))
    monkeypatch.setenv("MTL_NICCTL_NO_SUDO", "1")
    assert (
        _run(
            _fake_bin(tmp_path, tmp_path / "s0.json"),
            tmp_path / "o0",
            pci,
            hp0,
            "--profile=^st2110_3x$",
            "--ports=0000:4b:01.0,0000:4b:01.1",
        )
        == gr.HOST_FAULT_EXIT
    )
    _, hp1 = _fake_sysfs(tmp_path / "ok", hugepages=1024)
    assert (
        _run(
            _fake_bin(tmp_path, tmp_path / "s1.json"),
            tmp_path / "o1",
            pci,
            hp1,
            "--profile=^st2110_3x$",
            "--bind-pvf=0000:31:00.0,0000:31:00.1",
        )
        == 0
    )
