# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Unit tests of the nightly report's regression analyzer."""

import unittest

from report_generators.regression_analyzer import analyze_regressions

_KEYS = (
    "regressions",
    "fixes",
    "new_failures",
    "now_failing",
    "changed_failures",
    "lost_coverage",
)


def _run(*cases):
    """The pytest data of one report holding (test_name, result, log) cases."""
    return [
        {
            "test_cases": [
                {
                    "platform": "pytest",
                    "nic": "e810",
                    "category": "st20p",
                    "test_name": name,
                    "result": result,
                    "log": log,
                }
                for name, result, log in cases
            ]
        }
    ]


def _names(current, baseline, key):
    report = analyze_regressions(current, [], baseline, [])
    return [entry["test_name"] for entry in report[key]]


class AnalyzeRegressionsTest(unittest.TestCase):
    def test_a_skipped_test_that_starts_failing_is_reported(self):
        # 2026-10-19: missing media arrived, and 80 cases that had skipped failed
        current = _run(("t", "FAILED", "AssertionError: FAILURE: x"))
        baseline = _run(("t", "SKIPPED", "Skipped: Media file not present"))
        self.assertEqual(_names(current, baseline, "now_failing"), ["t"])
        self.assertEqual(_names(current, baseline, "regressions"), [])

    def test_a_skipped_test_that_errors_is_reported(self):
        current = _run(("t", "ERROR", "Failed: Media file not present on host"))
        baseline = _run(("t", "SKIPPED", "Skipped: Media file not present"))
        self.assertEqual(_names(current, baseline, "now_failing"), ["t"])

    def test_the_first_assertion_line_is_the_failure_reason(self):
        for first in ("AssertionError: ", "Failed: ", "E   AssertionError: "):
            current = _run(("t", "FAILED", f"log\n{first}wide\nE   AssertionError: x"))
            baseline = _run(("t", "FAILED", f"log\n{first}late\nE   AssertionError: x"))
            self.assertEqual(
                _names(current, baseline, "changed_failures"), ["t"], first
            )

    def test_a_failure_for_another_reason_is_reported(self):
        current = _run(
            ("t", "FAILED", "log\nAssertionError: FAILURE: LIST non-compliance")
        )
        baseline = _run(("t", "FAILED", "log\nAssertionError: FAILURE: only 'wide'"))
        self.assertEqual(_names(current, baseline, "changed_failures"), ["t"])

    def test_numbers_and_ids_do_not_change_a_failure_reason(self):
        current = _run(
            ("t", "FAILED", "AssertionError: 3 streams, 815f825a46d3 0x716f")
        )
        baseline = _run(
            ("t", "FAILED", "AssertionError: 2 streams, 6a9fb101bcc5 0x7fe3")
        )
        self.assertEqual(_names(current, baseline, "changed_failures"), [])

    def test_a_test_that_stops_running_is_reported(self):
        for result in ("PASSED", "FAILED"):
            current = _run(("t", "SKIPPED", "Skipped: x"))
            baseline = _run(("t", result, ""))
            self.assertEqual(_names(current, baseline, "lost_coverage"), ["t"], result)

    def test_unchanged_results_report_nothing(self):
        cases = (
            ("p", "PASSED", ""),
            ("s", "SKIPPED", "Skipped: x"),
            ("f", "FAILED", "AssertionError: x"),
        )
        for key in _KEYS:
            self.assertEqual(_names(_run(*cases), _run(*cases), key), [], key)


if __name__ == "__main__":
    unittest.main()
