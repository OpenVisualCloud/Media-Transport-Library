# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Offline tests for the lean KahawaiTest runner and nic_setup.sh."""

from __future__ import annotations

import importlib.util
import json
import os
import stat
import subprocess  # nosec B404 # runs nic_setup.sh in offline test via argv list
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
if "--list-json" in sys.argv:
    print(json.dumps([
        {{"case": "NoCtxTest.st20p_user_pacing", "isolated": False}},
        {{"case": "NoCtxTest.st20p_pf_tsn", "isolated": False}},
    ]))
    sys.exit(0)
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  st20p_pf_tsn" if "_pf_" in flt and "-NoCtxTest.*_pf_*" not in flt else "  st20p_user_pacing")
    sys.exit(0)
if "St20p*" in flt:
    cases = ["St20p.ok", "St20p.flaky", "St20p.fail"]
elif "St3*" in flt:
    cases = ["St30p.ok"]
else:
    cases = [flt]
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
    assert len(runs) == 14
    assert by_name["st2110_20"]["tier"] == "pr"
    assert by_name["st2110_20"]["topology"] == "vf_pair"
    assert (
        by_name["st2110_20"]["filter"] == "St20_rx*:St20_tx*-St20_tx.create_queue_cnt"
    )
    assert by_name["st2110_20"]["dma_auto"] is True
    assert by_name["st2110_20p"]["tier"] == "pr"
    assert by_name["st2110_20p"]["filter"] == "St20p*-St20p.redundant*"
    assert by_name["st2110_20p"]["dma_auto"] is False
    assert by_name["st2110_20_tx_queue_cnt"]["extra_args"] == ["--queue_cnt", "24"]
    assert by_name["st2110_20_tx_queue_cnt"]["filter"] == "St20_tx.create_queue_cnt"
    assert by_name["st2110_20_tx_queue_cnt"]["dma_auto"] is False
    assert by_name["st2110_22"]["filter"] == "St22*"
    assert by_name["st2110_22"]["dma_auto"] is False
    assert (
        by_name["st2110_3x_4x"]["filter"]
        == "St3*:St4*-St30p.redundant*:St40p.redundant*"
    )
    assert by_name["st2110_3x_4x"]["dma_auto"] is False
    assert by_name["core_misc"]["filter"] == "Main*:Misc*:Sch*:TxUserTimestampSync*"
    assert by_name["core_misc"]["tier"] == "nightly"
    assert by_name["core_misc"]["dma_auto"] is False
    assert by_name["dma_cvt_va"]["filter"] == "Dma*:Cvt*-Dma.iova_pa*"
    assert by_name["dma_cvt_va"]["extra_args"] == ["--iova_mode", "va"]
    assert by_name["dma_cvt_va"]["dma_auto"] is True
    assert "--multi_src_port" in by_name["st20p_auto_pacing_pa"]["extra_args"]
    assert "Dma.iova_pa*" in by_name["st20p_auto_pacing_pa"]["filter"]
    assert by_name["st20p_auto_pacing_pa"]["dma_auto"] is False
    assert by_name["st20p_kernel_loopback"]["needs_root"] is False
    assert by_name["st20p_kernel_loopback"]["topology"] == "kernel_lo"
    assert len(by_name["st20p_kernel_loopback"]["filter"].split(":")) == 10
    assert by_name["noctx"]["mode"] == "noctx"
    assert by_name["noctx"]["topology"] == "cross_pf_quad"
    assert by_name["noctx"]["env"]["MTL_ISOLATE"] == "require"
    assert by_name["noctx"]["env"]["NOCTX_STABILITY_SECONDS"] == "60"
    assert by_name["noctx"]["env"]["NOCTX_TEST_DURATION"] == "10"
    assert by_name["noctx_pf"]["mode"] == "noctx"
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
        "--profile=^(st2110_20p|st2110_3x_4x|noctx|noctx_pf)$",
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
    assert recs[("st2110_3x_4x", "St30p.ok")] == "passed"
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
            "--profile=^st2110_3x_4x$",
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
            "--profile=^st2110_3x_4x$",
            "--bind-pvf=0000:31:00.0,0000:31:00.1",
        )
        == 0
    )


def test_runner_ignores_expect_fail_invalid_ip_log(tmp_path: Path) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    ok_bin = _exec(
        tmp_path / "KahawaiTestOk",
        """#!/usr/bin/env python3
import sys, xml.etree.ElementTree as ET
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
print("MTL: Error: tv_ops_check(1), invalid ip 0.0.0.0")
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="St20p")
ET.SubElement(ts, "testcase", classname="St20p", name="tx_create_expect_fail", time="0.01")
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(0)
""",
    )
    assert (
        _run(
            ok_bin,
            tmp_path / "o_ok",
            pci,
            hp,
            "--profile=^st2110_20p$",
            "--ports=0000:4b:01.0,0000:4b:01.1",
        )
        == 0
    )
    bad_bin = _exec(
        tmp_path / "KahawaiTestBad",
        """#!/usr/bin/env python3
import sys
print("MTL: Error: mt_user_params_check(0), invalid ip 0.0.0.0")
sys.exit(1)
""",
    )
    assert (
        _run(
            bad_bin,
            tmp_path / "o_bad",
            pci,
            hp,
            "--profile=^st2110_20p$",
            "--ports=0000:4b:01.0,0000:4b:01.1",
        )
        == gr.HOST_FAULT_EXIT
    )


def test_runner_noctx_host_fault_detected_with_failing_xml(tmp_path: Path) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    noctx_bad = _exec(
        tmp_path / "KahawaiTestNoCtxBad",
        """#!/usr/bin/env python3
import sys, xml.etree.ElementTree as ET
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  init_32_queues")
    sys.exit(0)
if "--list-json" in sys.argv:
    print('[{"case": "NoCtxTest.init_32_queues", "isolated": false}]')
    sys.exit(0)
print("EAL: Cannot set up DMA remapping, error 12")
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="NoCtxTest")
tc = ET.SubElement(ts, "testcase", classname="NoCtxTest", name="init_32_queues", time="0.01")
ET.SubElement(tc, "failure", message="mtl_init failed").text = "mtl_init failed"
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(1)
""",
    )
    assert (
        _run(
            noctx_bad,
            tmp_path / "o_noctx_bad",
            pci,
            hp,
            f"--repo-root={tmp_path}",
            "--profile=^noctx$",
            "--ports=0000:4b:01.0,0000:4b:01.1,0000:4b:01.2,0000:4b:01.3",
            "--retry-delay=0",
        )
        == gr.HOST_FAULT_EXIT
    )


def test_runner_shared_retry_strips_shard_env_and_keeps_profile_env(
    tmp_path: Path,
) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    profiles = tmp_path / "profiles.yaml"
    profiles.write_text(
        """topologies:
  vf_pair:
    ports: 2
    kind: vf
profiles:
  custom:
    env:
      CUSTOM_FLAG: "keep_me"
runs:
  pr:
    - name: sharded_case
      profile: custom
      topology: vf_pair
      filter: "St20_rx*"
      env:
        GTEST_TOTAL_SHARDS: "2"
        GTEST_SHARD_INDEX: "0"
groups:
  g: [sharded_case]
""",
        encoding="utf-8",
    )
    seen = tmp_path / "seen.jsonl"
    bin_p = _exec(
        tmp_path / "KahawaiTestShardRetry",
        f"""#!/usr/bin/env python3
import json, os, sys, xml.etree.ElementTree as ET
from pathlib import Path
flt = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_filter=")), "")
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
with Path("{seen}").open("a", encoding="utf-8") as fh:
    fh.write(json.dumps({{
        "flt": flt,
        "custom": os.environ.get("CUSTOM_FLAG"),
        "shards": os.environ.get("GTEST_TOTAL_SHARDS"),
        "idx": os.environ.get("GTEST_SHARD_INDEX"),
    }}) + "\\n")
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="St20_rx")
tc = ET.SubElement(ts, "testcase", classname="St20_rx", name="flaky_one", time="0.01")
fail = (flt == "St20_rx*")
if fail:
    ET.SubElement(tc, "failure", message="transient").text = "transient"
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(1 if fail else 0)
""",
    )
    out = tmp_path / "out"
    rc = _run(
        bin_p,
        out,
        pci,
        hp,
        f"--profiles={profiles}",
        "--tier=pr",
        "--ports=0000:4b:01.0,0000:4b:01.1",
        "--retry-delay=0",
    )
    assert rc == 0
    calls = [json.loads(ln) for ln in seen.read_text(encoding="utf-8").splitlines()]
    assert len(calls) == 2
    assert calls[0]["shards"] == "2" and calls[0]["custom"] == "keep_me"
    assert (
        calls[1]["flt"] == "St20_rx.flaky_one"
        and calls[1]["custom"] == "keep_me"
        and calls[1]["shards"] is None
        and calls[1]["idx"] is None
    )


def test_real_kahawai_unified_cli_discovery_without_ports(tmp_path: Path) -> None:
    """Compiled KahawaiTest lists tests and handles empty filters with zero port flags."""
    repo_root = RUNNER_DIR.parent.parent.parent
    candidates = (
        repo_root / ".local_install/mtl/bin/KahawaiTest",
        repo_root / "build/tests/KahawaiTest",
    )
    binary = next((p for p in candidates if p.is_file()), None)
    if binary is None:
        pytest.skip("KahawaiTest binary not built")

    all_cases = gr.list_cases(binary, "*")
    assert any(c.startswith("St20p.") for c in all_cases)
    assert any(c.startswith("NoCtxTest.") for c in all_cases)

    noctx_cases = gr.list_cases(binary, "NoCtxTest.*")
    assert noctx_cases and all(c.startswith("NoCtxTest.") for c in noctx_cases)

    xml_out = tmp_path / "empty.xml"
    empty = subprocess.run(
        [
            str(binary),
            "--gtest_filter=NoSuchSuite.*",
            f"--gtest_output=xml:{xml_out}",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert empty.returncode == 0, empty.stdout + empty.stderr
    assert xml_out.is_file()

    # --help and -h must print usage once and exit 0 without running mtl_init or forking.
    for help_flag in ("--help", "-h"):
        hp = subprocess.run(
            [str(binary), help_flag],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        assert hp.returncode == 0, hp.stdout + hp.stderr
        assert hp.stdout.count("MTL Integration Test Options") == 1
        if help_flag == "--help":
            assert (
                hp.stdout.count(
                    "This program contains tests written using Google Test."
                )
                == 1
            )

    # Empty and colon-only filters match 0 tests, matching GoogleTest semantics.
    for empty_flt in ("", ":"):
        res = subprocess.run(
            [str(binary), f"--gtest_filter={empty_flt}"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        assert res.returncode == 0, res.stdout + res.stderr
        assert "0 tests" in res.stdout

    # Invalid shard env vars must fail loudly instead of silently running unsharded.
    bad_shard = subprocess.run(
        [str(binary), "--gtest_filter=Misc.*"],
        env={**os.environ, "GTEST_TOTAL_SHARDS": "2", "GTEST_SHARD_INDEX": "5"},
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    assert bad_shard.returncode != 0

    # Unsupported JSON output in multi-phase mode must fail loudly, not silently drop.
    bad_json = subprocess.run(
        [
            str(binary),
            "--gtest_filter=Misc.ptp:NoCtxTest.st40i_smoke",
            f"--gtest_output=json:{tmp_path / 'out.json'}",
        ],
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    assert bad_json.returncode != 0


def test_load_profiles_rejects_unknown_profile_topology_and_duplicate_name(
    tmp_path: Path,
) -> None:
    bad_profile = tmp_path / "bad_profile.yaml"
    bad_profile.write_text(
        """topologies:
  vf_pair: {ports: 2, kind: vf}
profiles:
  default: {}
runs:
  pr:
    - {name: r1, profile: missing_prof, topology: vf_pair, filter: "St20*"}
""",
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="unknown profile 'missing_prof'"):
        gr.load_profiles(bad_profile)

    bad_topo = tmp_path / "bad_topo.yaml"
    bad_topo.write_text(
        """topologies:
  vf_pair: {ports: 2, kind: vf}
profiles:
  default: {}
runs:
  pr:
    - {name: r1, profile: default, topology: missing_topo, filter: "St20*"}
""",
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="unknown topology 'missing_topo'"):
        gr.load_profiles(bad_topo)

    dup_name = tmp_path / "dup_name.yaml"
    dup_name.write_text(
        """topologies:
  vf_pair: {ports: 2, kind: vf}
profiles:
  default: {}
runs:
  pr:
    - {name: same_run, profile: default, topology: vf_pair, filter: "St20*"}
  nightly:
    - {name: same_run, profile: default, topology: vf_pair, filter: "St30*"}
""",
        encoding="utf-8",
    )
    with pytest.raises(ValueError, match="Duplicate run name 'same_run'"):
        gr.load_profiles(dup_name)


def test_runner_dry_run_without_nics_or_binary(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    rc = gr.main(
        [
            "--dry-run",
            "--no-sudo",
            f"--binary={tmp_path / 'nonexistent' / 'KahawaiTest'}",
            f"--sysfs-pci={tmp_path / 'no_pci'}",
            f"--sysfs-hugepages={tmp_path / 'no_hp'}",
            f"--out-dir={tmp_path / 'out'}",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "[dry-run]" in out
    assert "--p_port 0000:00:00.1 --r_port 0000:00:00.2" in out
    assert "--port_list 0000:00:00.1,0000:00:00.2,0000:00:00.3,0000:00:00.4" in out
    assert "--p_port kernel:lo --r_port kernel:lo" in out
    assert "--port_list=0000:00:00.1,0000:00:01.1,0000:00:00.2,0000:00:01.2" in out
    assert "--gtest_filter=NoCtxTest.*-NoCtxTest.*_pf_*" in out
    assert "==> [noctx_pf] skipped" in out


def test_runner_vf_rss_lut_all_reused_pfs_and_no_dup_after_auto_create(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def _add_vf(pci: Path, vfio_drv: Path, pf: str, vf: str) -> None:
        d = pci / vf
        d.mkdir(parents=True)
        (d / "class").write_text("0x020000\n", encoding="utf-8")
        (d / "device").write_text("0x1889\n", encoding="utf-8")
        os.symlink(vfio_drv, d / "driver")
        os.symlink(pci / pf, d / "physfn")

    def _sysfs_nicctl(base: Path, pci: Path, log: Path) -> Path:
        vfio_drv = base / "drivers/vfio-pci"
        vfio_drv.mkdir(parents=True, exist_ok=True)
        return _exec(
            base / "nicctl_sysfs.sh",
            f"""#!/usr/bin/env bash
set -euo pipefail
echo "$*" >> "{log}"
if [ "$1" = create_tvf ]; then
  pf="$2"; cnt="$3"
  slot="01"; [ "${{pf##*.}}" = "1" ] && slot="11"
  for i in $(seq 0 $((cnt - 1))); do
    vf="0000:4b:${{slot}}.${{i}}"
    mkdir -p "{pci}/${{vf}}"
    printf '0x020000\\n' > "{pci}/${{vf}}/class"
    printf '0x1889\\n' > "{pci}/${{vf}}/device"
    ln -sfn "{vfio_drv}" "{pci}/${{vf}}/driver"
    ln -sfn "{pci}/${{pf}}" "{pci}/${{vf}}/physfn"
  done
elif [ "$1" = list ]; then
  pf="$2"
  slot="01"; [ "${{pf##*.}}" = "1" ] && slot="11"
  for d in "{pci}"/0000:4b:${{slot}}.*; do
    [ -d "$d" ] && basename "$d"
  done
fi
""",
        )

    def _recording_bin(base: Path, args_log: Path) -> Path:
        return _exec(
            base / "KahawaiTest",
            f"""#!/usr/bin/env python3
import json, sys, xml.etree.ElementTree as ET
from pathlib import Path
with Path("{args_log}").open("a", encoding="utf-8") as fh:
    fh.write(json.dumps(sys.argv[1:]) + "\\n")
if "--list-json" in sys.argv:
    print(json.dumps([{{"case": "NoCtxTest.st20p_user_pacing", "isolated": False}}]))
    sys.exit(0)
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  st20p_user_pacing")
    sys.exit(0)
xml = next((a.split("xml:", 1)[1] for a in sys.argv[1:] if a.startswith("--gtest_output=xml:")), "")
root = ET.Element("testsuites")
ts = ET.SubElement(root, "testsuite", name="s")
ET.SubElement(ts, "testcase", classname="NoCtxTest", name="st20p_user_pacing", time="0.01")
if xml:
    ET.ElementTree(root).write(xml, encoding="utf-8", xml_declaration=True)
sys.exit(0)
""",
        )

    monkeypatch.setenv("MTL_NICCTL_NO_SUDO", "1")

    # 1) Pre-existing VFs across BOTH PFs: interleave cross-PF and run vf-rss-lut on BOTH PFs.
    reuse_dir = tmp_path / "reuse"
    pci_r, hp_r = _fake_sysfs(reuse_dir)
    vfio_r = reuse_dir / "drivers/vfio-pci"
    vfio_r.mkdir(parents=True)
    for pf, vfs in (
        ("0000:4b:00.0", ("0000:4b:01.0", "0000:4b:01.1")),
        ("0000:4b:00.1", ("0000:4b:11.0", "0000:4b:11.1")),
    ):
        for vf in vfs:
            _add_vf(pci_r, vfio_r, pf, vf)
    log_r, args_r = reuse_dir / "nic.log", reuse_dir / "args.jsonl"
    monkeypatch.setenv("MTL_NICCTL", str(_sysfs_nicctl(reuse_dir, pci_r, log_r)))
    assert (
        _run(
            _recording_bin(reuse_dir, args_r),
            reuse_dir / "out",
            pci_r,
            hp_r,
            f"--repo-root={reuse_dir}",
            "--profile=^noctx$",
            "--retry-delay=0",
        )
        == 0
    )
    nic_lines_r = log_r.read_text(encoding="utf-8").splitlines()
    assert "vf_rss_lut 0000:4b:00.0" in nic_lines_r
    assert "vf_rss_lut 0000:4b:00.1" in nic_lines_r
    assert not any(ln.startswith("create_tvf") for ln in nic_lines_r)
    invocations = [
        json.loads(ln) for ln in args_r.read_text(encoding="utf-8").splitlines()
    ]
    assert any(
        "--port_list=0000:4b:01.0,0000:4b:11.0,0000:4b:01.1,0000:4b:11.1" in argv
        for argv in invocations
    )

    # 2) Auto-create on single PF: must NOT run duplicate vf-rss-lut on newly created PF.
    single_dir = tmp_path / "auto_single"
    pci_s, hp_s = _fake_sysfs(single_dir)
    log_s = single_dir / "nic.log"
    monkeypatch.setenv("MTL_NICCTL", str(_sysfs_nicctl(single_dir, pci_s, log_s)))
    assert (
        _run(
            _fake_bin(single_dir, single_dir / "st.json"),
            single_dir / "out",
            pci_s,
            hp_s,
            "--profile=^st2110_3x_4x$",
        )
        == 0
    )
    nic_lines_s = log_s.read_text(encoding="utf-8").splitlines()
    assert "create_tvf 0000:4b:00.0 6" in nic_lines_s
    assert "disable_vf 0000:4b:00.0" in nic_lines_s
    assert not any(ln.startswith("vf_rss_lut") for ln in nic_lines_s)

    # 3) Partial reuse (PF0 reused, PF1 auto-created): vf-rss-lut ONLY on PF0, disable ONLY PF1.
    part_dir = tmp_path / "partial"
    pci_p, hp_p = _fake_sysfs(part_dir)
    vfio_p = part_dir / "drivers/vfio-pci"
    vfio_p.mkdir(parents=True)
    for vf in ("0000:4b:01.0", "0000:4b:01.1"):
        _add_vf(pci_p, vfio_p, "0000:4b:00.0", vf)
    log_p, args_p = part_dir / "nic.log", part_dir / "args.jsonl"
    monkeypatch.setenv("MTL_NICCTL", str(_sysfs_nicctl(part_dir, pci_p, log_p)))
    assert (
        _run(
            _recording_bin(part_dir, args_p),
            part_dir / "out",
            pci_p,
            hp_p,
            f"--repo-root={part_dir}",
            "--profile=^noctx$",
            "--retry-delay=0",
        )
        == 0
    )
    nic_lines_p = log_p.read_text(encoding="utf-8").splitlines()
    assert "create_tvf 0000:4b:00.1 6" in nic_lines_p
    assert "vf_rss_lut 0000:4b:00.0" in nic_lines_p
    assert "vf_rss_lut 0000:4b:00.1" not in nic_lines_p
    assert "disable_vf 0000:4b:00.1" in nic_lines_p
    assert "disable_vf 0000:4b:00.0" not in nic_lines_p


def test_runner_pf_pair_skip_decoupled_from_run_name(tmp_path: Path) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    profiles = tmp_path / "profiles.yaml"
    profiles.write_text(
        """topologies:
  pf_pair:
    ports: 2
    kind: pf
profiles:
  own_pf:
    instance: own
runs:
  nightly:
    - name: custom_pf_suite
      profile: own_pf
      topology: pf_pair
      filter: "NoCtxTest.*_pf_*"
groups:
  g: [custom_pf_suite]
""",
        encoding="utf-8",
    )
    out = tmp_path / "out"
    rc = _run(
        _fake_bin(tmp_path, tmp_path / "st.json"),
        out,
        pci,
        hp,
        f"--profiles={profiles}",
    )
    assert rc == 0
    recs = [
        json.loads(ln)
        for ln in (out / "results.jsonl").read_text(encoding="utf-8").splitlines()
    ]
    assert len(recs) == 1
    assert recs[0]["profile"] == "custom_pf_suite"
    assert recs[0]["result"] == "skipped"
    assert recs[0]["message"] == "no PF ports"


def test_runner_noctx_empty_metadata_is_host_fault(tmp_path: Path) -> None:
    pci, hp = _fake_sysfs(tmp_path)
    bad_meta_bin = _exec(
        tmp_path / "KahawaiTestEmptyMeta",
        """#!/usr/bin/env python3
import sys
if "--gtest_list_tests" in sys.argv:
    print("NoCtxTest.")
    print("  st20p_user_pacing")
    sys.exit(0)
if "--list-json" in sys.argv:
    print("[]")
    sys.exit(0)
sys.exit(0)
""",
    )
    out = tmp_path / "out"
    rc = _run(
        bad_meta_bin,
        out,
        pci,
        hp,
        f"--repo-root={tmp_path}",
        "--profile=^noctx$",
        "--ports=0000:4b:01.0,0000:4b:01.1,0000:4b:01.2,0000:4b:01.3",
    )
    assert rc == gr.HOST_FAULT_EXIT
    summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    assert "--list-json returned empty metadata" in summary["host_fault"]
