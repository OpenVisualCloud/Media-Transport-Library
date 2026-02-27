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

        # Extract RAM from MemTotal if present
        mem_match = re.search(r"MemTotal:\s+(\d+)\s+kB", content)
        if mem_match:
            mem_gb = int(mem_match.group(1)) / (1024 * 1024)
            info["ram"] = f"{mem_gb:.0f}GB"
        else:
            info["ram"] = "N/A"

        # Extract unique NIC models from dpdk-devbind output
        nic_models = set()
        for m in re.finditer(
            r"'Ethernet Controller (E\d+-\S+)[^']*'", content
        ):
            nic_models.add(m.group(1))
        for m in re.finditer(
            r"'Ethernet.*?(E8[0-9]{2}[^']*?)(?:\\\\x00|')", content
        ):
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
            "ram": "N/A",
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

    print(
        f"  DEBUG: Found {len(test_details)} test details elements for {Path(html_file).name}"
    )

    for test_idx, test_detail in enumerate(test_details):
        test_classes = test_detail.get("class", [])
        if isinstance(test_classes, list):
            test_classes = " ".join(test_classes)
        else:
            test_classes = str(test_classes)

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

        if test_name and len(test_name) > 0:
            test_cases.append(
                {
                    "nic": nic,
                    "category": category,
                    "test_name": test_name,
                    "result": result,
                    "duration": duration,
                    "platform": "pytest",
                }
            )
            if test_idx < 3:  # Debug first few
                print(f"  DEBUG: Test {test_idx}: {test_name[:50]} -> {result}")

    return test_cases


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
        # Extract suite name (everything before last dot)
        if "." in test_name:
            parts = test_name.rsplit(".", 1)
            suite = parts[0]
        else:
            suite = "unknown"

        # Initialize suite in results if not present
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
                "unstable": test_name
                in failed_tests,  # Mark if it failed before passing
            }
        )

    # Process tests that ONLY failed (never passed)
    for test_name, duration in failed_tests.items():
        if test_name not in passed_tests:
            # This test failed and never passed
            if "." in test_name:
                parts = test_name.rsplit(".", 1)
                suite = parts[0]
            else:
                suite = "unknown"

            # Initialize suite in results if not present
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

    passed_pattern = r"\[\s*PASSED\s*\]\s+(\w+)\.(\w+)\s*\((\d+)\s*ms\)"
    failed_pattern = r"\[\s*FAILED\s*\]\s+(\w+)\.(\w+)\s*\((\d+)\s*ms\)"
    skipped_pattern = r"\[\s*SKIPPED\s*\]\s+(\w+)\.(\w+)"

    for line in content.split("\n"):
        # Track passed tests
        match = re.search(passed_pattern, line)
        if match:
            suite, test_name, duration = match.group(1), match.group(2), match.group(3)
            if suite not in test_suites:
                test_suites[suite] = {"passed": 0, "failed": 0, "skipped": 0}
            test_suites[suite]["passed"] += 1
            all_test_cases.append(
                {
                    "nic": nic,
                    "category": suite,
                    "test_name": f"{suite}.{test_name}",
                    "result": "PASSED",
                    "duration": f"{duration} ms",
                    "platform": "gtest",
                }
            )

        # Track failed tests
        match = re.search(failed_pattern, line)
        if match:
            suite, test_name, duration = match.group(1), match.group(2), match.group(3)
            if suite not in test_suites:
                test_suites[suite] = {"passed": 0, "failed": 0, "skipped": 0}
            test_suites[suite]["failed"] += 1
            all_test_cases.append(
                {
                    "nic": nic,
                    "category": suite,
                    "test_name": f"{suite}.{test_name}",
                    "result": "FAILED",
                    "duration": f"{duration} ms",
                    "platform": "gtest",
                }
            )

        # Track skipped tests
        match = re.search(skipped_pattern, line)
        if match:
            suite, test_name = match.group(1), match.group(2)
            if suite not in test_suites:
                test_suites[suite] = {"passed": 0, "failed": 0, "skipped": 0}
            test_suites[suite]["skipped"] += 1
            all_test_cases.append(
                {
                    "nic": nic,
                    "category": suite,
                    "test_name": f"{suite}.{test_name}",
                    "result": "SKIPPED",
                    "duration": "N/A",
                    "platform": "gtest",
                }
            )

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
