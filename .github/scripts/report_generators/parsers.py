# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Parsers for pytest, gtest, and system information files."""

import re
from pathlib import Path

from bs4 import BeautifulSoup


def parse_pytest_html(html_file):
    """Parse pytest HTML report and extract test results and individual test cases."""
    with open(html_file, "r") as f:
        soup = BeautifulSoup(f.read(), "html.parser")

    # Extract NIC and category from filename
    filename = Path(html_file).stem
    nic, category = _extract_nic_category_from_filename(filename)

    # Parse summary statistics
    stats = {"nic": nic, "category": category}
    for status in ["passed", "failed", "skipped", "error", "xpassed", "xfailed"]:
        elem = soup.find("span", class_=status)
        if elem:
            count_text = elem.get_text(strip=True)
            match = re.search(r"(\d+)", count_text)
            stats[status] = int(match.group(1)) if match else 0
        else:
            stats[status] = 0

    stats["total"] = sum(
        stats.get(k, 0)
        for k in ["passed", "failed", "skipped", "error", "xpassed", "xfailed"]
    )

    # Parse individual test cases
    test_cases = _parse_pytest_test_cases(soup, nic, category, html_file)
    stats["test_cases"] = test_cases

    print(f"  Found {len(test_cases)} test cases in {Path(html_file).name}")
    return stats


def parse_gtest_log(log_file):
    """Parse gtest log and extract test results."""
    with open(log_file, "r") as f:
        content = f.read()

    # Extract NIC from filename
    filename = Path(log_file).stem
    match = re.match(r"nightly-gtest-report-(.+)", filename)
    nic = match.group(1).upper() if match else "UNKNOWN"

    # Try multi-execution format first (primary strategy for current logs)
    results, all_test_cases = _parse_gtest_multi_execution(content, nic)

    # Fallback: Try table format
    if not results:
        results = _parse_gtest_table(content, nic)
        test_suites, all_test_cases = _parse_gtest_individual_tests(content, nic)

        # If no table found, create results from test suites
        if not results and test_suites:
            results = _create_results_from_test_suites(test_suites, nic)

    # Fallback: Try summary format
    if not results:
        results = _parse_gtest_summary(content, nic)
        test_suites, all_test_cases = _parse_gtest_individual_tests(content, nic)

    # Add test cases to results if not already added
    for result in results:
        if "test_cases" not in result:
            result["test_cases"] = [
                tc for tc in all_test_cases if tc["category"] == result["category"]
            ]

    return results


def parse_system_info(info_file):
    """Parse system_info.txt and extract key information."""
    try:
        with open(info_file, "r") as f:
            content = f.read()

        info = {}

        # Extract tested NIC from directory name (e.g. system-info-e810-video)
        dirname = Path(info_file).parent.name
        dir_match = re.match(r"system-info-([^-]+)", dirname)
        info["tested_nic"] = dir_match.group(1).upper() if dir_match else "Unknown"

        # Extract hostname and kernel from uname line
        uname_match = re.search(r"Linux (\S+) ([\d\.-]+\S*)", content)
        if uname_match:
            info["hostname"] = uname_match.group(1)
            info["kernel"] = uname_match.group(2)
        else:
            info["hostname"] = "unknown"
            info["kernel"] = "unknown"

        # Extract OS version from kernel version
        info["os"] = _detect_ubuntu_version(info["kernel"])

        # Extract CPU info
        cpu_match = re.search(r"Model name:\s+(.+)", content)
        info["cpu"] = cpu_match.group(1).strip() if cpu_match else "Unknown"

        # Extract CPU cores
        cores_match = re.search(r"CPU\(s\):\s+(\d+)", content)
        info["cpu_cores"] = cores_match.group(1) if cores_match else "Unknown"

        # Extract HugePages
        hugepages_match = re.search(r"HugePages_Total:\s+(\d+)", content)
        hugepagesize_match = re.search(r"Hugepagesize:\s+(\d+)\s+kB", content)
        if hugepages_match and hugepagesize_match:
            total_pages = int(hugepages_match.group(1))
            page_size_kb = int(hugepagesize_match.group(1))
            page_size_mb = page_size_kb // 1024
            total_gb = (total_pages * page_size_kb) / (1024 * 1024)
            if page_size_mb >= 1024:
                info["hugepages"] = (
                    f"{total_pages} x {page_size_mb // 1024}GB = {total_gb:.0f}GB"
                )
            else:
                info["hugepages"] = (
                    f"{total_pages} x {page_size_mb}MB = {total_gb:.1f}GB"
                )
        else:
            info["hugepages"] = "Unknown"

        # Extract unique NIC models from dpdk-devbind output
        nic_models = set()
        for m in re.finditer(r"'Ethernet Controller (E\d+-\S+)[^']*'", content):
            nic_models.add(m.group(1))
        for m in re.finditer(r"'Ethernet.*?(E8[0-9]{2}[^']*?)(?:\\\\x00|')", content):
            short = m.group(1).strip().split()[0]
            if short:
                nic_models.add(short)
        info["nics"] = (
            ", ".join(sorted(nic_models)) if nic_models else info["tested_nic"]
        )

        return info
    except Exception as e:
        print(f"Error parsing {info_file}: {e}")
        return {
            "hostname": "unknown",
            "kernel": "unknown",
            "os": "unknown",
            "cpu": "unknown",
            "cpu_cores": "unknown",
            "hugepages": "unknown",
            "nics": "Unknown",
            "tested_nic": "Unknown",
        }


# Ubuntu kernel-to-release mapping (major.minor prefix)
_UBUNTU_KERNEL_MAP = {
    "5.4": "20.04",
    "5.15": "22.04",
    "5.19": "22.10",
    "6.2": "23.04",
    "6.5": "23.10",
    "6.8": "24.04",
    "6.11": "24.10",
}


def _detect_ubuntu_version(kernel_version):
    """Map kernel version to Ubuntu release."""
    m = re.match(r"(\d+\.\d+)", kernel_version)
    if m:
        ver = _UBUNTU_KERNEL_MAP.get(m.group(1))
        if ver:
            return f"Ubuntu {ver}"
    return f"Linux {kernel_version}"


# Private helper functions


def _extract_nic_category_from_filename(filename):
    """Extract NIC and category from pytest report filename."""
    # Try vendor-specific pattern first: nightly-test-report-{nic}-{vendor}-{category}.html
    match = re.match(
        r"nightly-test-report-([^-]+)-(dell|broadcom|mellanox|intel|cisco)-(.+)",
        filename,
    )
    if match:
        return f"{match.group(1)}-{match.group(2)}".upper(), match.group(3)

    # Fallback to simple pattern: nightly-test-report-{nic}-{category}.html
    match = re.match(r"nightly-test-report-([^-]+)-(.+)", filename)
    if match:
        return match.group(1).upper(), match.group(2)

    return "UNKNOWN", "UNKNOWN"


def _parse_pytest_test_cases(soup, nic, category, html_file):
    """Parse individual test cases from pytest HTML."""
    test_cases = []
    test_details = soup.find_all("details", class_="test")

    for test_idx, test_detail in enumerate(test_details):
        test_classes = test_detail.get("class", [])
        test_classes = (
            " ".join(test_classes)
            if isinstance(test_classes, list)
            else str(test_classes)
        )

        summary = test_detail.find("summary")
        if not summary:
            continue

        status_badge = summary.find("span", class_="status")
        result_text = status_badge.get_text(strip=True) if status_badge else ""

        # Extract test name
        test_name_span = summary.find("span", class_="test-name")
        if test_name_span:
            test_name = test_name_span.get_text(strip=True)
        else:
            title = summary.find(["h3", "h2", "h1"])
            if title:
                test_name = title.get_text(strip=True)
                if status_badge:
                    test_name = test_name.replace(result_text, "").strip()
            else:
                continue

        # Extract duration
        duration_span = summary.find("span", class_="duration")
        duration = duration_span.get_text(strip=True) if duration_span else "N/A"

        # Determine result
        result = _determine_test_result(test_classes, result_text)

        # Extract failure log from the detail body (only for non-passing tests)
        log = ""
        if result in ("FAILED", "ERROR"):
            log = _extract_pytest_log(test_detail, summary)

        if test_name:
            test_cases.append(
                {
                    "nic": nic,
                    "category": category,
                    "test_name": test_name,
                    "result": result,
                    "duration": duration,
                    "platform": "pytest",
                    "log": log,
                }
            )

    return test_cases


def _extract_pytest_log(test_detail, summary):
    """Extract failure log/traceback from a pytest test detail element.

    Collects all <pre> blocks (tracebacks, captured output) preserving
    newlines so the full multi-line log is available in the report.
    """
    parts = []

    # Collect <div class="log"> content
    log_div = test_detail.find("div", class_="log")
    if log_div:
        text = log_div.get_text(separator="\n").strip()
        if text:
            parts.append(text)

    # Collect ALL <pre> blocks (tracebacks, captured stdout/stderr)
    for pre in test_detail.find_all("pre"):
        text = pre.get_text(separator="\n").strip()
        if text and text not in parts:
            parts.append(text)

    if parts:
        return "\n\n".join(parts)

    # Last resort: all text after summary
    full_text = test_detail.get_text(separator="\n", strip=True)
    summary_text = summary.get_text(strip=True)
    idx = full_text.find(summary_text)
    if idx >= 0:
        remainder = full_text[idx + len(summary_text) :].strip()
        if remainder:
            return remainder
    return ""


def _determine_test_result(test_classes, result_text):
    """Determine test result from class names and result text."""
    test_classes_lower = test_classes.lower()
    result_text_lower = result_text.lower()

    if "passed" in test_classes_lower or "passed" in result_text_lower:
        return "PASSED"
    elif (
        "failed" in test_classes_lower
        or "failed" in result_text_lower
        or "error" in result_text_lower
    ):
        return "FAILED"
    elif "skipped" in test_classes_lower or "skipped" in result_text_lower:
        return "SKIPPED"
    elif "xpassed" in test_classes_lower or "xpassed" in result_text_lower:
        return "XPASSED"
    elif "xfailed" in test_classes_lower or "xfailed" in result_text_lower:
        return "XFAILED"

    return "UNKNOWN"


def _extract_gtest_failure_logs(content):
    """Extract failure output for each test from gtest log.

    For each test, finds ALL executions by pairing each '[ RUN      ]' with
    its immediately following '[ FAILED  ] ... (N ms)' line. Collects output
    from every failed execution so the full log is available in the report.
    """
    logs = {}
    # Match [ FAILED ] lines WITH duration (actual test results, not summary)
    marker_pattern = re.compile(
        r"^\[ RUN      \]\s+(\S+)" r"|^\[  FAILED  \]\s+(\S+)\s+\(",
        re.MULTILINE,
    )

    # Track the start of the current test run
    current_test = None
    current_start = None

    for m in marker_pattern.finditer(content):
        run_name = m.group(1)
        fail_name = m.group(2)

        if run_name:
            # [ RUN ] marker — record start position
            current_test = run_name
            current_start = m.end()
        elif fail_name and fail_name == current_test and current_start is not None:
            # [ FAILED ] marker matching the current [ RUN ] — extract log
            log_text = content[current_start : m.start()].strip()
            if log_text:
                if fail_name in logs:
                    # Append subsequent execution logs separated by a header
                    logs[fail_name] += f"\n\n--- retry ---\n\n{log_text}"
                else:
                    logs[fail_name] = log_text
            current_test = None
            current_start = None

    return logs


def _extract_suite_name(test_name):
    """Extract suite name from dotted test name (everything before the last dot)."""
    if "." in test_name:
        return test_name.rsplit(".", 1)[0]
    return "unknown"


def _init_suite_result(results, suite, nic):
    """Initialize a suite entry in the results dict if not already present."""
    if suite not in results:
        results[suite] = {
            "nic": nic,
            "category": suite,
            "passed": 0,
            "failed": 0,
            "skipped": 0,
            "error": 0,
            "xpassed": 0,
            "xfailed": 0,
            "total": 0,
        }


def _parse_gtest_multi_execution(content, nic):
    """Parse gtest logs - use bash script logic.

    Logic matches bash script:
    1. Find all unique tests with [       OK ] (passed)
    2. Find all unique tests with [  FAILED  ] (failed)
    3. Tests that appear in BOTH lists: count as PASSED (eventually passed after retry)
    4. Tests ONLY in failed list: count as FAILED (never passed)

    Args:
        content: Full log file content
        nic: NIC identifier

    Returns:
        Tuple of (results list, all_test_cases list)
    """
    # Pattern to match test results with duration (excludes "listed below:" summary lines)
    # [       OK ] Suite.TestName (123 ms) or [  FAILED  ] Suite.TestName (123 ms)
    passed_pattern = r"\[       OK \]\s+(\S+)\s+\((\d+)\s*ms\)"
    failed_pattern = r"\[  FAILED  \]\s+(\S+)\s+\((\d+)\s*ms\)"

    # Track unique tests that passed and failed (using sets like bash script sort -u)
    passed_tests = {}  # key: "Suite.TestName", value: duration
    failed_tests = {}  # key: "Suite.TestName", value: duration

    # Extract failure logs: text between [ RUN ] and [ FAILED ] for each test
    failure_logs = _extract_gtest_failure_logs(content)

    # Find all passed tests
    for match in re.finditer(passed_pattern, content):
        test_name, duration = match.group(1), match.group(2)
        # Keep the last duration seen (though we mainly care about presence)
        passed_tests[test_name] = duration

    # Find all failed tests
    for match in re.finditer(failed_pattern, content):
        test_name, duration = match.group(1), match.group(2)
        failed_tests[test_name] = duration

    # Determine final results using bash script logic:
    # - If test is in passed_tests: PASSED (even if also in failed_tests)
    # - If test is ONLY in failed_tests: FAILED
    results = {}  # Use dict to aggregate by test suite name
    all_test_cases = []

    # Process all passed tests (including those that also failed but eventually passed)
    for test_name, duration in passed_tests.items():
        suite = _extract_suite_name(test_name)
        _init_suite_result(results, suite, nic)

        results[suite]["passed"] += 1
        results[suite]["total"] += 1

        all_test_cases.append(
            {
                "nic": nic,
                "category": suite,
                "test_name": test_name,
                "result": "PASSED",
                "duration": f"{duration} ms",
                "platform": "gtest",
                "unstable": test_name in failed_tests,
                "log": "",
            }
        )

    # Process tests that ONLY failed (never passed)
    for test_name, duration in failed_tests.items():
        if test_name not in passed_tests:
            suite = _extract_suite_name(test_name)
            _init_suite_result(results, suite, nic)

            results[suite]["failed"] += 1
            results[suite]["total"] += 1

            all_test_cases.append(
                {
                    "nic": nic,
                    "category": suite,
                    "test_name": test_name,
                    "result": "FAILED",
                    "duration": f"{duration} ms",
                    "platform": "gtest",
                    "unstable": False,
                    "log": failure_logs.get(test_name, ""),
                }
            )

    # Convert results dict to list
    return list(results.values()), all_test_cases


def _parse_gtest_table(content, nic):
    """Parse gtest summary table format."""
    results = []
    table_pattern = (
        r"(\S+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+([\d.]+)%"
    )

    for line in content.split("\n"):
        match = re.search(table_pattern, line)
        if match and match.group(1) not in ["Test", "TOTAL", "---"]:
            results.append(
                {
                    "nic": nic,
                    "category": match.group(1),
                    "passed": int(match.group(2)),
                    "failed": int(match.group(3)),
                    "skipped": int(match.group(4)),
                    "error": 0,
                    "xpassed": 0,
                    "xfailed": 0,
                    "total": int(match.group(5)),
                }
            )

    return results


def _parse_gtest_individual_tests(content, nic):
    """Parse individual gtest test cases."""
    test_suites = {}
    all_test_cases = []
    failure_logs = _extract_gtest_failure_logs(content)

    # (regex, result_string, has_duration)
    patterns = [
        (r"\[\s*PASSED\s*\]\s+(\w+)\.(\w+)\s*\((\d+)\s*ms\)", "PASSED", True),
        (r"\[\s*FAILED\s*\]\s+(\w+)\.(\w+)\s*\((\d+)\s*ms\)", "FAILED", True),
        (r"\[\s*SKIPPED\s*\]\s+(\w+)\.(\w+)", "SKIPPED", False),
    ]

    for line in content.split("\n"):
        for pattern, result, has_duration in patterns:
            match = re.search(pattern, line)
            if match:
                suite, test_name = match.group(1), match.group(2)
                full_name = f"{suite}.{test_name}"
                duration = f"{match.group(3)} ms" if has_duration else "N/A"
                if suite not in test_suites:
                    test_suites[suite] = {"passed": 0, "failed": 0, "skipped": 0}
                test_suites[suite][result.lower()] += 1
                log = failure_logs.get(full_name, "") if result == "FAILED" else ""
                all_test_cases.append(
                    {
                        "nic": nic,
                        "category": suite,
                        "test_name": full_name,
                        "result": result,
                        "duration": duration,
                        "platform": "gtest",
                        "log": log,
                    }
                )
                break  # Only match one pattern per line

    return test_suites, all_test_cases


def _create_results_from_test_suites(test_suites, nic):
    """Create results list from test suite dictionary."""
    results = []
    for suite, counts in test_suites.items():
        total = counts["passed"] + counts["failed"] + counts["skipped"]
        if total > 0:
            results.append(
                {
                    "nic": nic,
                    "category": suite,
                    "passed": counts["passed"],
                    "failed": counts["failed"],
                    "skipped": counts["skipped"],
                    "error": 0,
                    "xpassed": 0,
                    "xfailed": 0,
                    "total": total,
                }
            )
    return results


def _parse_gtest_summary(content, nic):
    """Parse gtest summary at end of log."""
    total_passed = 0
    total_failed = 0

    for line in content.split("\n"):
        if re.search(r"\[\s*PASSED\s*\]\s+(\d+)\s+test", line):
            match = re.search(r"(\d+)", line)
            if match:
                total_passed = int(match.group(1))
        elif re.search(r"\[\s*FAILED\s*\]\s+(\d+)\s+test", line):
            match = re.search(r"(\d+)", line)
            if match:
                total_failed = int(match.group(1))

    if total_passed > 0 or total_failed > 0:
        return [
            {
                "nic": nic,
                "category": "all_tests",
                "passed": total_passed,
                "failed": total_failed,
                "skipped": 0,
                "error": 0,
                "xpassed": 0,
                "xfailed": 0,
                "total": total_passed + total_failed,
            }
        ]

    return []
