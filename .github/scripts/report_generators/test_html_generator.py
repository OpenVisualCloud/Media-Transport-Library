# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Unit tests of the nightly report's HTML generator."""

import re
import unittest

from report_generators.html_generator import _generate_regression_section


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


if __name__ == "__main__":
    unittest.main()
