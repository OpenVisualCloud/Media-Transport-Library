# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Regression analyzer: compares current test results against a baseline run."""

import re

# Numbers and hex ids that change between runs of the same failure
_REASON_VARIABLE = re.compile(r"0x[0-9a-fA-F]+|[0-9a-fA-F]{8,}|\d+")


def _build_test_index(pytest_data, gtest_data):
    """Build a dict mapping (platform, nic, category, test_name) -> (result, log)."""
    index = {}
    for suite_data in [*pytest_data, *gtest_data]:
        for tc in suite_data.get("test_cases", []):
            key = (tc["platform"], tc["nic"], tc["category"], tc["test_name"])
            index[key] = (tc["result"], tc.get("log", ""))
    return index


def analyze_regressions(current_pytest, current_gtest, baseline_pytest, baseline_gtest):
    """Compare current results against baseline and classify changes.

    Returns a dict with:
        regressions: tests that PASSED in baseline but FAILED now
        fixes:       tests that FAILED in baseline but PASSED now
        new_failures: tests absent from baseline that FAILED now
        now_failing: tests that were SKIPPED in baseline and FAILED now
        changed_failures: tests that FAILED in both, for a different reason
        lost_coverage: tests that ran in baseline and are SKIPPED now
    Each entry is a dict with platform, nic, category, test_name,
    baseline_result (or None), current_result.
    """
    current = _build_test_index(current_pytest, current_gtest)
    baseline = _build_test_index(baseline_pytest, baseline_gtest)

    regressions = []
    fixes = []
    new_failures = []
    now_failing = []
    changed_failures = []
    lost_coverage = []

    for key, (cur_result, cur_log) in current.items():
        platform, nic, category, test_name = key
        entry = {
            "platform": platform,
            "nic": nic,
            "category": category,
            "test_name": test_name,
        }

        if key in baseline:
            base_result, _base_log = baseline[key]
            if _is_pass(base_result) and _is_fail(cur_result):
                regressions.append(
                    {
                        **entry,
                        "baseline_result": base_result,
                        "current_result": cur_result,
                        "log": cur_log,
                    }
                )
            elif _is_fail(base_result) and _is_pass(cur_result):
                fixes.append(
                    {
                        **entry,
                        "baseline_result": base_result,
                        "current_result": cur_result,
                        "log": _base_log,
                    }
                )
            elif _is_skip(base_result) and _is_fail(cur_result):
                now_failing.append(
                    {
                        **entry,
                        "baseline_result": base_result,
                        "current_result": cur_result,
                        "log": cur_log,
                    }
                )
            elif _is_fail(base_result) and _is_fail(cur_result):
                base_reason = _failure_reason(_base_log)
                cur_reason = _failure_reason(cur_log)
                if base_reason != cur_reason:
                    changed_failures.append(
                        {
                            **entry,
                            "baseline_result": base_result,
                            "current_result": cur_result,
                            "log": f"Baseline: {base_reason}\n"
                            f"Current: {cur_reason}\n\n{cur_log}",
                        }
                    )
            elif _is_skip(cur_result) and not _is_skip(base_result):
                lost_coverage.append(
                    {
                        **entry,
                        "baseline_result": base_result,
                        "current_result": cur_result,
                        "log": cur_log,
                    }
                )
        else:
            if _is_fail(cur_result):
                new_failures.append(
                    {
                        **entry,
                        "baseline_result": None,
                        "current_result": cur_result,
                        "log": cur_log,
                    }
                )

    # Sort each list for deterministic output
    def _sort_key(e):
        return (e["platform"], e["nic"], e["category"], e["test_name"])

    regressions.sort(key=_sort_key)
    fixes.sort(key=_sort_key)
    new_failures.sort(key=_sort_key)
    now_failing.sort(key=_sort_key)
    changed_failures.sort(key=_sort_key)
    lost_coverage.sort(key=_sort_key)

    # Coverage comparison: how many tests exist in each index
    current_keys = set(current)
    baseline_keys = set(baseline)
    common = len(current_keys & baseline_keys)

    return {
        "regressions": regressions,
        "fixes": fixes,
        "new_failures": new_failures,
        "now_failing": now_failing,
        "changed_failures": changed_failures,
        "lost_coverage": lost_coverage,
        "coverage": {
            "current_total": len(current_keys),
            "baseline_total": len(baseline_keys),
            "common": common,
            "only_in_current": len(current_keys) - common,
            "only_in_baseline": len(baseline_keys) - common,
        },
    }


def _is_pass(result):
    """Return True if the result represents a passing test."""
    return result.upper() in ("PASSED", "XFAILED")


def _is_fail(result):
    """Return True if the result represents a failing test."""
    return result.upper() in ("FAILED", "ERROR")


def _is_skip(result):
    """Return True if the result represents a skipped test."""
    return result.upper() == "SKIPPED"


def _failure_reason(log):
    """The first assertion line of a failure log, its numbers and hex ids masked."""
    for line in log.splitlines():
        line = line.strip()
        if line.startswith(("AssertionError", "Failed:", "E ")):
            return _REASON_VARIABLE.sub("N", line)[:160]
    return ""
