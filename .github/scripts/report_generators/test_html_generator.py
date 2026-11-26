# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Unit tests of the nightly report's HTML generator."""

import json
import re
import tempfile
import unittest
from pathlib import Path

from report_generators.html_generator import (
    _detail_section,
    _generate_regression_section,
    _generate_status_detail_sections,
)
from report_generators.parsers import parse_gtest_log


class RegressionSectionTest(unittest.TestCase):
    def test_each_log_toggle_opens_its_own_log(self):
        entry = {
            "platform": "pytest",
            "nic": "e810",
            "category": "st20p",
            "test_name": "t",
            "baseline_result": "PASSED",
            "current_result": "FAILED",
            "log": "AssertionError: x",
        }
        html = _generate_regression_section(
            {"regressions": [entry], "now_failing": [entry], "fixes": [entry]}
        )
        ids = re.findall(r'id="(reglog-[^"]+)"', html)
        self.assertEqual(len(ids), 3)
        self.assertEqual(len(set(ids)), 3, ids)

    def test_html_escapes_angle_brackets_and_separates_profiles(self):
        data = {
            "nic": "E810",
            "category": "St20p",
            "test_cases": [
                {
                    "test_name": "st2110_20.<process>",
                    "profile": "p1",
                    "result": "FAILED",
                    "duration": "10 ms",
                    "log": "boom <tag>",
                },
                {
                    "test_name": "st2110_20.<process>",
                    "profile": "p2",
                    "result": "FAILED",
                    "duration": "20 ms",
                    "log": "boom 2",
                },
            ],
        }
        detail_html = _detail_section(data, "gtest-detail-0", 7)
        self.assertIn("[p1] st2110_20.&lt;process&gt;", detail_html)
        self.assertIn("[p2] st2110_20.&lt;process&gt;", detail_html)
        self.assertNotIn("<process>", detail_html)
        # Keep 3-column order (Test Name, Result, Duration) for lab-portal parsing.
        self.assertNotIn("<th>Profile</th>", detail_html)
        self.assertRegex(
            detail_html,
            r'<tr><td>\[p1\] st2110_20\.&lt;process&gt;.*?</td><td class="[^"]*">FAILED</td>',
        )
        ids = re.findall(r'id="(gtest-detail-0-log-[^"]+)"', detail_html)
        self.assertEqual(len(ids), 2)
        self.assertEqual(len(set(ids)), 2)

        status_html = _generate_status_detail_sections([data], "gtest")
        self.assertIn("[p1] st2110_20.&lt;process&gt;", status_html)
        self.assertNotIn("<process>", status_html)

    def test_parse_gtest_jsonl_preserves_full_log_and_profile(self):
        long_msg = "line\n" * 200
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "nightly-gtest-report-e810.jsonl"
            p.write_text(
                json.dumps(
                    {
                        "case": "St20p.digest_1080p_s1",
                        "profile": "st2110_20p",
                        "nic": "E810",
                        "host": "mtl-runner-1",
                        "result": "failed",
                        "duration_ms": 1234.0,
                        "message": long_msg,
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            suites = parse_gtest_log(p)
        self.assertEqual(len(suites), 1)
        tc = suites[0]["test_cases"][0]
        self.assertEqual(tc["profile"], "st2110_20p")
        self.assertEqual(tc["result"], "FAILED")
        self.assertEqual(tc["log"], long_msg)


if __name__ == "__main__":
    unittest.main()
