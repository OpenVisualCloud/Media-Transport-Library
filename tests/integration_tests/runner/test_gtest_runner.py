#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
"""Unit tests for gtest_runner.py orchestration and parsing helpers."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gtest_runner as gr  # noqa: E402


class TestGtestRunnerHelpers(unittest.TestCase):
    def test_slot_sip_preserves_slot_zero_and_validates_octets(self) -> None:
        self.assertEqual(gr._slot_sip("192.168.50.1", 0), "192.168.50.1")
        s1 = gr._slot_sip("192.168.50.1", 1)
        s2 = gr._slot_sip("192.168.50.1", 2)
        self.assertNotEqual(s1, "192.168.50.1")
        self.assertNotEqual(s1, s2)
        # Invalid octet (>255) falls back to valid private address
        fb = gr._slot_sip("192.168.999.1", 0)
        self.assertTrue(fb.startswith("192.168."))
        self.assertNotEqual(fb, "192.168.999.1")

    def test_parse_xml_handles_corrupt_and_notrun_and_disabled(self) -> None:
        ctx = {"profile": "p", "nic": "E810", "host": "h"}
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "bad.xml"
            p.write_text("<testsuites><testcase", encoding="utf-8")
            self.assertEqual(gr._parse_xml(p, ctx), [])

            p2 = Path(td) / "mixed.xml"
            p2.write_text(
                """<testsuites>
                <testsuite name="S">
                  <testcase classname="S" name="ok" status="run" result="completed" time="0.1"/>
                  <testcase classname="S" name="aborted" status="notrun" result="completed" time="0.0"/>
                  <testcase classname="S" name="DISABLED_skip" status="notrun" result="suppressed" time="0.0"/>
                  <testcase classname="S" name="bad" status="run" result="completed" time="0.2">
                    <failure message="boom"/>
                  </testcase>
                </testsuite>
                </testsuites>""",
                encoding="utf-8",
            )
            rows = {r["case"]: r["result"] for r in gr._parse_xml(p2, ctx)}
            self.assertEqual(rows["S.ok"], "passed")
            self.assertEqual(rows["S.aborted"], "not_run")
            self.assertEqual(rows["S.DISABLED_skip"], "skipped")
            self.assertEqual(rows["S.bad"], "failed")

    def test_list_cases_excludes_disabled(self) -> None:
        fake = mock.Mock(
            returncode=0,
            stdout=(
                "NoCtxTest.\n"
                "  active_one\n"
                "  DISABLED_broken\n"
                "DISABLED_Suite.\n"
                "  hidden\n"
            ),
        )
        with mock.patch.object(gr.subprocess, "run", return_value=fake):
            cases = gr._list_cases(Path("/bin/true"), "*")
        self.assertEqual(cases, ["NoCtxTest.active_one"])

    def test_negative_jobs_rejected(self) -> None:
        rc = gr.main(["--jobs", "-1", "--profile", "core_misc"])
        self.assertEqual(rc, 2)

    def test_setup_ports_rejects_short_explicit_ports(self) -> None:
        selected = [r for r in gr.RUNS if r[0] == "redundant_stats"]
        with self.assertRaises(gr.HostFault):
            gr._setup_ports("0000:4b:01.0,0000:4b:01.1", selected, [], [])

    def test_main_nonzero_rc_with_all_passed_xml_marks_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_p = Path(td)
            fake_bin = td_p / "KahawaiTest"
            fake_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_bin.chmod(0o755)
            out_dir = td_p / "out"

            def fake_run_cmd(cmd, env, to_s, tmp, log_fh, io_lock, *_a):
                for arg in cmd:
                    if arg.startswith("--gtest_output=xml:"):
                        xml_p = Path(arg.split("xml:", 1)[1])
                        xml_p.write_text(
                            '<testsuites><testsuite name="S">'
                            '<testcase classname="S" name="t1" status="run" '
                            'result="completed" time="0.01"/>'
                            "</testsuite></testsuites>",
                            encoding="utf-8",
                        )
                return 139, "Segmentation fault"

            with (
                mock.patch.object(
                    gr,
                    "_setup_ports",
                    return_value=([], [], [], "E810"),
                ),
                mock.patch.object(gr, "_run_cmd", side_effect=fake_run_cmd),
            ):
                rc = gr.main(
                    [
                        "--binary",
                        str(fake_bin),
                        "--profile",
                        "st20p_kernel_loopback",
                        "--out-dir",
                        str(out_dir),
                        "--retries",
                        "1",
                        "--jobs",
                        "1",
                    ]
                )
            self.assertEqual(rc, 1)
            recs = [
                json.loads(ln)
                for ln in (out_dir / "results.jsonl")
                .read_text(encoding="utf-8")
                .splitlines()
                if ln.strip()
            ]
            self.assertTrue(
                any(
                    r["case"].endswith(".__process__") and r["result"] == "failed"
                    for r in recs
                )
            )
            self.assertFalse(any("<" in r["case"] for r in recs))

    def test_noctx_env_does_not_shorten_durations(self) -> None:
        self.assertNotIn("NOCTX_STABILITY_SECONDS", gr.NOCTX_ENV)
        self.assertNotIn("NOCTX_TEST_DURATION", gr.NOCTX_ENV)

    def test_extract_case_logs_and_recover_crash_without_xml(self) -> None:
        log = (
            "[ RUN      ] St20_tx.first\n"
            "ok line\n"
            "[       OK ] St20_tx.first (12 ms)\n"
            "[ RUN      ] St20_tx.crasher\n"
            "fatal line before sigsegv\n"
        )
        ctx = {"profile": "st2110_20", "nic": "E810", "host": "h"}
        recs = gr._recover_from_log(log, ctx, 139)
        by_case = {r["case"]: r for r in recs}
        self.assertEqual(by_case["St20_tx.first"]["result"], "passed")
        self.assertEqual(by_case["St20_tx.crasher"]["result"], "failed")
        self.assertIn(
            "fatal line before sigsegv", by_case["St20_tx.crasher"]["message"]
        )
        self.assertIn("rc=139", by_case["St20_tx.crasher"]["message"])

    def test_empty_noctx_list_fails_instead_of_passing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_p = Path(td)
            fake_bin = td_p / "KahawaiTest"
            fake_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_bin.chmod(0o755)
            out_dir = td_p / "out"
            with (
                mock.patch.object(
                    gr,
                    "_setup_ports",
                    return_value=(["0000:4b:01.0", "0000:4b:01.1"], [], [], "E810"),
                ),
                mock.patch.object(gr, "_list_cases", return_value=[]),
            ):
                rc = gr.main(
                    [
                        "--binary",
                        str(fake_bin),
                        "--profile",
                        "^init_modes$",
                        "--out-dir",
                        str(out_dir),
                        "--jobs",
                        "1",
                    ]
                )
            self.assertEqual(rc, 1)
            recs = [
                json.loads(ln)
                for ln in (out_dir / "results.jsonl")
                .read_text(encoding="utf-8")
                .splitlines()
                if ln.strip()
            ]
            self.assertTrue(
                any(
                    r["case"] == "init_modes.__empty__" and r["result"] == "failed"
                    for r in recs
                )
            )

    def test_pr_tier_does_not_pass_gtest_fail_fast(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_p = Path(td)
            fake_bin = td_p / "KahawaiTest"
            fake_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            fake_bin.chmod(0o755)
            seen_cmds: list[list[str]] = []

            def fake_run_cmd(cmd, env, to_s, last_log, log_fh, io_lock, *_a):
                seen_cmds.append(cmd)
                for arg in cmd:
                    if arg.startswith("--gtest_output=xml:"):
                        Path(arg.split("xml:", 1)[1]).write_text(
                            '<testsuites><testsuite name="S">'
                            '<testcase classname="S" name="t1" status="run" '
                            'result="completed" time="0.01"/>'
                            "</testsuite></testsuites>",
                            encoding="utf-8",
                        )
                return 0, ""

            with (
                mock.patch.object(
                    gr,
                    "_setup_ports",
                    return_value=(["0000:4b:01.0", "0000:4b:01.1"], [], [], "E810"),
                ),
                mock.patch.object(gr, "_run_cmd", side_effect=fake_run_cmd),
            ):
                rc = gr.main(
                    [
                        "--binary",
                        str(fake_bin),
                        "--tier",
                        "pr",
                        "--profile",
                        "^st2110_22$",
                        "--out-dir",
                        str(td_p / "out"),
                        "--retries",
                        "1",
                        "--jobs",
                        "1",
                    ]
                )
            self.assertEqual(rc, 0)
            self.assertTrue(seen_cmds)
            self.assertNotIn("--gtest_fail_fast", seen_cmds[0])

    def test_negative_test_log_does_not_trigger_host_fault_on_later_crash(self):
        with tempfile.TemporaryDirectory() as td:
            td_p = Path(td)
            fake_bin = td_p / "KahawaiTest"
            fake_bin.write_text("#!/bin/sh\n", encoding="utf-8")
            fake_bin.chmod(0o755)
            out_dir = td_p / "out"
            crash_log = (
                "Note: Google Test filter = Main*\n"
                "[==========] Running 2 tests from 1 test suite.\n"
                "[ RUN      ] Main.init_expect_fail\n"
                "MTL: Error: mt_user_params_check(0), invalid ip 0.0.0.0\n"
                "[       OK ] Main.init_expect_fail (0 ms)\n"
                "[ RUN      ] Main.crash_later\n"
            )
            with (
                mock.patch.object(
                    gr,
                    "_setup_ports",
                    return_value=(["0000:4b:01.0", "0000:4b:01.1"], [], [], "E810"),
                ),
                mock.patch.object(gr, "_run_cmd", return_value=(-11, crash_log)),
                mock.patch.object(
                    gr,
                    "_list_cases",
                    return_value=["Main.init_expect_fail", "Main.crash_later"],
                ),
            ):
                rc = gr.main(
                    [
                        "--binary",
                        str(fake_bin),
                        "--tier",
                        "nightly",
                        "--profile",
                        "^core_misc$",
                        "--out-dir",
                        str(out_dir),
                        "--retries",
                        "1",
                        "--jobs",
                        "1",
                    ]
                )
            self.assertEqual(rc, 1)
            recs = [
                json.loads(line)
                for line in (out_dir / "results.jsonl").read_text().splitlines()
                if line.strip()
            ]
            by_case = {r["case"]: r for r in recs}
            self.assertEqual(by_case["Main.init_expect_fail"]["result"], "passed")
            self.assertEqual(by_case["Main.crash_later"]["result"], "failed")
            self.assertNotIn("host_fault", by_case["Main.crash_later"]["message"])

    def test_shared_profile_zero_tests_fails_with_empty_record(self):
        with tempfile.TemporaryDirectory() as td:
            td_p = Path(td)
            fake_bin = td_p / "KahawaiTest"
            fake_bin.write_text("#!/bin/sh\n", encoding="utf-8")
            fake_bin.chmod(0o755)
            out_dir = td_p / "out"
            with (
                mock.patch.object(
                    gr,
                    "_setup_ports",
                    return_value=(["0000:4b:01.0", "0000:4b:01.1"], [], [], "E810"),
                ),
                mock.patch.object(gr, "_run_cmd", return_value=(0, "")),
            ):
                rc = gr.main(
                    [
                        "--binary",
                        str(fake_bin),
                        "--tier",
                        "pr",
                        "--profile",
                        "^st2110_22$",
                        "--out-dir",
                        str(out_dir),
                        "--retries",
                        "1",
                        "--jobs",
                        "1",
                    ]
                )
            self.assertEqual(rc, 1)
            recs = [
                json.loads(line)
                for line in (out_dir / "results.jsonl").read_text().splitlines()
                if line.strip()
            ]
            self.assertEqual(len(recs), 1)
            self.assertEqual(recs[0]["case"], "st2110_22.__empty__")
            self.assertEqual(recs[0]["result"], "failed")

    def test_setup_ports_recreates_vfs_when_each_pf_has_only_two_vfs(self):
        with tempfile.TemporaryDirectory() as td:
            sysfs = Path(td) / "devices"
            pf0 = sysfs / "0000:4b:00.0"
            (pf0 / "net" / "eth0").mkdir(parents=True)
            (pf0 / "class").write_text("0x020000\n", encoding="utf-8")
            (pf0 / "device").write_text("0x1592\n", encoding="utf-8")
            (pf0 / "net" / "eth0" / "operstate").write_text("up\n", encoding="utf-8")
            drv_ice = Path(td) / "drv_ice" / "ice"
            drv_ice.mkdir(parents=True)
            (pf0 / "driver").symlink_to(drv_ice)
            drv_vfio = Path(td) / "drv_vfio" / "vfio-pci"
            drv_vfio.mkdir(parents=True)

            def make_vfs(count: int) -> None:
                for p in list(sysfs.glob("0000:4b:01.*")):
                    for sub in sorted(p.rglob("*"), reverse=True):
                        sub.unlink()
                    p.rmdir()
                for i in range(count):
                    vf = sysfs / f"0000:4b:01.{i}"
                    vf.mkdir(parents=True)
                    (vf / "class").write_text("0x020000\n", encoding="utf-8")
                    (vf / "device").write_text("0x1889\n", encoding="utf-8")
                    (vf / "driver").symlink_to(drv_vfio)
                    (vf / "physfn").symlink_to(pf0)

            make_vfs(2)
            nicctl_calls: list[tuple] = []

            def fake_nicctl(sub: str, *args: str) -> None:
                nicctl_calls.append((sub, *args))
                if sub == "disable_vf":
                    make_vfs(0)
                elif sub == "create_tvf":
                    make_vfs(int(args[1]))

            hp_dir = Path(td) / "hugepages" / "hugepages-2048kB"
            hp_dir.mkdir(parents=True)
            (hp_dir / "free_hugepages").write_text("1024\n", encoding="utf-8")

            orig_path = gr.Path

            def fake_path(p):
                s = str(p)
                if s == "/sys/bus/pci/devices":
                    return sysfs
                if s == "/sys/kernel/mm/hugepages":
                    return Path(td) / "hugepages"
                return orig_path(p)

            created, bound = [], []
            selected = [("redundant_stats", "St20p.redundant*", "vf_quad", "")]
            with (
                mock.patch.object(gr, "Path", side_effect=fake_path),
                mock.patch.object(gr, "_nicctl", side_effect=fake_nicctl),
            ):
                vf_p, _, _, nic = gr._setup_ports(None, selected, created, bound)
            self.assertEqual(len(vf_p), 6)
            self.assertEqual(nic, "E810")
            self.assertIn(("disable_vf", "0000:4b:00.0"), nicctl_calls)
            self.assertIn(("create_tvf", "0000:4b:00.0", "6"), nicctl_calls)

    def test_tx_queue_cnt_is_in_pr_tier_and_serial(self):
        by_name = {r[0]: r for r in gr.RUNS}
        self.assertIn("tx_queue_cnt", by_name)
        flags = set(by_name["tx_queue_cnt"][3].split(","))
        self.assertIn("pr", flags)
        self.assertNotIn("par", flags)


if __name__ == "__main__":
    unittest.main()
