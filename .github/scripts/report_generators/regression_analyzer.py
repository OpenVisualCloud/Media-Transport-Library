# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Regression analyzer: compares current test results against a baseline run."""


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
    Each entry is a dict with platform, nic, category, test_name,
    baseline_result (or None), current_result.
    """
    current = _build_test_index(current_pytest, current_gtest)
    baseline = _build_test_index(baseline_pytest, baseline_gtest)

    regressions = []
    fixes = []
    new_failures = []

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

    # Coverage comparison: how many tests exist in each index
    current_keys = set(current)
    baseline_keys = set(baseline)
    common = len(current_keys & baseline_keys)

    return {
        "regressions": regressions,
        "fixes": fixes,
        "new_failures": new_failures,
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
