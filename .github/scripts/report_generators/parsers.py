# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Parsers for pytest, gtest, and system information files."""

import json
import re
from pathlib import Path

from bs4 import BeautifulSoup


def parse_pytest_jsonl(jsonl_file):
    """Parse structured pytest results.jsonl emitted by pytest_jsonl_plugin."""
    path = Path(jsonl_file)
    default_nic, default_cat = _extract_nic_category_from_filename(path.stem)
    stats = {
        "nic": default_nic,
        "category": default_cat,
        "passed": 0,
        "failed": 0,
        "skipped": 0,
        "error": 0,
        "xpassed": 0,
        "xfailed": 0,
        "total": 0,
        "runner": "unknown",
        "test_cases": [],
    }
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        rec = json.loads(line)
        nic = (rec.get("nic") or default_nic).upper()
        cat = rec.get("category") or default_cat
        stats["nic"], stats["category"] = nic, cat
        if rec.get("host"):
            stats["runner"] = rec["host"]
        res = str(rec.get("result", "failed")).lower()
        key = (
            res
            if res in ("passed", "failed", "skipped", "error", "xpassed", "xfailed")
            else "failed"
        )
        stats[key] += 1
        stats["total"] += 1
        stats["test_cases"].append(
            {
                "nic": nic,
                "category": cat,
                "test_name": rec["case"],
                "result": key.upper(),
                "duration": f"{rec.get('duration_ms', 0)} ms",
                "platform": "pytest",
                "log": (
                    rec.get("message", "")
                    if key in ("failed", "error", "skipped")
                    else ""
                ),
            }
        )
    return stats


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

    # Extract runner hostname from setup phase logs
    stats["runner"] = _extract_runner_from_pytest(soup)

    # Parse individual test cases
    test_cases = _parse_pytest_test_cases(soup, nic, category, html_file)
    stats["test_cases"] = test_cases

    print(f"  Found {len(test_cases)} test cases in {Path(html_file).name}")
    return stats


def parse_gtest_log(log_file):
    """Parse gtest results from structured results.jsonl (or legacy gtest.log)."""
    path = Path(log_file)
    content = path.read_text(encoding="utf-8", errors="replace")
    match = re.match(r"nightly-gtest-report-(.+)", path.stem)
    default_nic = match.group(1).upper() if match else "UNKNOWN"

    # Preferred format: structured JSON lines from tests/integration_tests/runner/
    jsonl_records = []
    for raw in content.splitlines():
        line = raw.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                obj = json.loads(line)
                if "case" in obj and "result" in obj:
                    jsonl_records.append(obj)
            except json.JSONDecodeError:
                pass
    if jsonl_records:
        return _parse_gtest_jsonl(jsonl_records, default_nic)

    # Minimal legacy fallback for older plain-text gtest.log artifacts
    return _parse_gtest_legacy_text(content, default_nic)


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


def _extract_runner_from_pytest(soup):
    """Extract runner hostname from pytest HTML setup phase logs.

    Looks for uname output like 'Linux mtl-runner-11 6.8.0-...' in pre tags.
    The pattern matches 'Linux <hostname> <kernel-version>' format.
    """
    pres = soup.find_all("pre")
    for pre in pres:
        text = pre.get_text()
        # Match uname output: Linux <hostname> <kernel-version>
        match = re.search(r"Linux\s+([\w.-]+)\s+\d+\.\d+\.\d+", text)
        if match:
            return match.group(1)
    return "unknown"


def _extract_nic_category_from_filename(filename):
    """Extract NIC and category from pytest report filename."""
    # Try vendor-specific pattern first: nightly-test-report-{nic}-{vendor}-{category}.html
    match = re.match(
        r"nightly-test-report-([^-]+)-(dell|broadcom|mellanox|intel|cisco)-(.+)",
        filename,
    )
    if match:
        return f"{match.group(1)}-{match.group(2)}".upper(), match.group(3)

    # Fallback to simple pattern: nightly-test-report-{nic}-{category}.html,
    # or nightly-test-report-{nic}-{category}-{shard}.html for a shard
    match = re.match(r"nightly-test-report-([^-]+)-(.+?)(?:-\d+)?$", filename)
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

        # Extract log from the detail body (for failed, error, and skipped tests)
        log = ""
        if result in ("FAILED", "ERROR", "SKIPPED"):
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

    # xpassed and xfailed contain passed and failed, so they go first.
    if "xpassed" in test_classes_lower or "xpassed" in result_text_lower:
        return "XPASSED"
    elif "xfailed" in test_classes_lower or "xfailed" in result_text_lower:
        return "XFAILED"
    elif "passed" in test_classes_lower or "passed" in result_text_lower:
        return "PASSED"
    elif "error" in test_classes_lower or "error" in result_text_lower:
        return "ERROR"
    elif "failed" in test_classes_lower or "failed" in result_text_lower:
        return "FAILED"
    elif "skipped" in test_classes_lower or "skipped" in result_text_lower:
        return "SKIPPED"

    return "UNKNOWN"


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


def _parse_gtest_jsonl(records, default_nic):
    """Aggregate structured results.jsonl records by suite without hiding failures."""
    suites = {}
    for rec in records:
        case = rec["case"]
        suite = _extract_suite_name(case)
        nic = (rec.get("nic") or default_nic).upper()
        runner = rec.get("host") or "Unknown"
        _init_suite_result(suites, suite, nic)
        suites[suite]["runner"] = runner
        suites[suite].setdefault("test_cases", [])

        raw_res = str(rec.get("result", "")).lower()
        status = {
            "passed": "PASSED",
            "flaky": "PASSED",
            "failed": "FAILED",
            "skipped": "SKIPPED",
        }.get(raw_res, "FAILED")
        key = (
            "passed"
            if status == "PASSED"
            else ("skipped" if status == "SKIPPED" else "failed")
        )
        suites[suite][key] += 1
        suites[suite]["total"] += 1
        suites[suite]["test_cases"].append(
            {
                "nic": nic,
                "category": suite,
                "test_name": case,
                "profile": rec.get("profile", "default"),
                "result": status,
                "duration": f"{rec.get('duration_ms', 0)} ms",
                "platform": "gtest",
                "unstable": raw_res == "flaky",
                "log": rec.get("message", "") if status == "FAILED" else "",
            }
        )
    return list(suites.values())


def _parse_gtest_legacy_text(content, nic):
    """Minimal fallback parser for plain-text gtest.log artifacts."""
    suites = {}
    seen = {}
    for pat, st in (
        (r"\[\s+OK\s+\]\s+(\S+)\s+\((\d+)\s*ms\)", "PASSED"),
        (r"\[\s+FAILED\s+\]\s+(\S+)\s+\((\d+)\s*ms\)", "FAILED"),
        (r"\[\s+SKIPPED\s+\]\s+(\S+)\s+\((\d+)\s*ms\)", "SKIPPED"),
    ):
        for m in re.finditer(pat, content):
            name, dur = m.group(1), m.group(2)
            prev = seen.get(name)
            if prev is None or st == "FAILED":
                seen[name] = (st, dur, prev is not None and prev[0] != st)
    for name, (st, dur, unstable) in seen.items():
        suite = _extract_suite_name(name)
        _init_suite_result(suites, suite, nic)
        suites[suite]["runner"] = "Unknown"
        suites[suite].setdefault("test_cases", [])
        suites[suite][st.lower()] += 1
        suites[suite]["total"] += 1
        suites[suite]["test_cases"].append(
            {
                "nic": nic,
                "category": suite,
                "test_name": name,
                "result": st,
                "duration": f"{dur} ms",
                "platform": "gtest",
                "unstable": unstable,
                "log": "",
            }
        )
    return list(suites.values())
