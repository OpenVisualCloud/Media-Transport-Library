"""Regression analyzer: compares current test results against a baseline run."""


def _build_test_index(pytest_data, gtest_data):
    """Build a dict mapping (platform, nic, category, test_name) -> result."""
    index = {}
    for suite_data in pytest_data:
        for tc in suite_data.get("test_cases", []):
            key = (tc["platform"], tc["nic"], tc["category"], tc["test_name"])
            index[key] = tc["result"]
    for suite_data in gtest_data:
        for tc in suite_data.get("test_cases", []):
            key = (tc["platform"], tc["nic"], tc["category"], tc["test_name"])
            index[key] = tc["result"]
    return index


def analyze_regressions(
    current_pytest, current_gtest, baseline_pytest, baseline_gtest
):
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

    for key, cur_result in current.items():
        platform, nic, category, test_name = key
        entry = {
            "platform": platform,
            "nic": nic,
            "category": category,
            "test_name": test_name,
        }

        if key in baseline:
            base_result = baseline[key]
            if _is_pass(base_result) and _is_fail(cur_result):
                regressions.append(
                    {**entry, "baseline_result": base_result, "current_result": cur_result}
                )
            elif _is_fail(base_result) and _is_pass(cur_result):
                fixes.append(
                    {**entry, "baseline_result": base_result, "current_result": cur_result}
                )
        else:
            if _is_fail(cur_result):
                new_failures.append(
                    {**entry, "baseline_result": None, "current_result": cur_result}
                )

    # Sort each list for deterministic output
    sort_key = lambda e: (e["platform"], e["nic"], e["category"], e["test_name"])
    regressions.sort(key=sort_key)
    fixes.sort(key=sort_key)
    new_failures.sort(key=sort_key)

    return {
        "regressions": regressions,
        "fixes": fixes,
        "new_failures": new_failures,
    }


def _is_pass(result):
    """Return True if the result represents a passing test."""
    return result.upper() in ("PASSED", "XFAILED")


def _is_fail(result):
    """Return True if the result represents a failing test."""
    return result.upper() in ("FAILED", "ERROR")
