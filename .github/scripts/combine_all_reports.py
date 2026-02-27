#!/usr/bin/env python3
"""Combine pytest HTML reports and gtest logs into unified Excel and HTML reports."""

import argparse
import sys
from pathlib import Path

from report_generators.excel_generator import generate_excel_report
from report_generators.html_generator import generate_html_report

# Import report generators and parsers
from report_generators.parsers import (
    parse_gtest_log,
    parse_pytest_html,
    parse_system_info,
)
from report_generators.regression_analyzer import analyze_regressions


def collect_pytest_reports(pytest_dir):
    """Collect and parse all pytest HTML reports."""
    pytest_data = []
    pytest_reports = list(Path(pytest_dir).glob("*.html"))

    if not pytest_reports:
        print(f"Warning: No pytest HTML reports found in {pytest_dir}")
        return pytest_data

    print(f"Found {len(pytest_reports)} pytest reports")
    for html_file in pytest_reports:
        try:
            stats = parse_pytest_html(html_file)
            pytest_data.append(stats)
        except Exception as e:
            print(f"Error parsing {html_file}: {e}")

    print(f"Parsed {len(pytest_data)} pytest reports")
    return pytest_data


def collect_gtest_reports(gtest_dir):
    """Collect and parse all gtest log files."""
    gtest_data = []
    gtest_logs = list(Path(gtest_dir).glob("*.log"))

    if not gtest_logs:
        print(f"Warning: No gtest log files found in {gtest_dir}")
        return gtest_data

    print(f"Found {len(gtest_logs)} gtest logs")
    for log_file in gtest_logs:
        try:
            results = parse_gtest_log(log_file)
            gtest_data.extend(results)
        except Exception as e:
            print(f"Error parsing {log_file}: {e}")

    print(f"Parsed {len(gtest_logs)} gtest logs")
    return gtest_data


def collect_system_info(system_info_dir):
    """Collect and parse system information files, deduplicated by hostname."""
    if not system_info_dir or not Path(system_info_dir).exists():
        return []

    info_files = list(Path(system_info_dir).rglob("system_info.txt"))
    print(f"Found {len(info_files)} system info files")

    seen = {}  # hostname -> info dict
    for info_file in info_files:
        try:
            info = parse_system_info(info_file)
            hostname = info.get("hostname", "unknown")
            if hostname not in seen:
                seen[hostname] = info
        except Exception as e:
            print(f"Error parsing {info_file}: {e}")

    system_info_list = list(seen.values())
    print(f"Unique hosts: {len(system_info_list)}")
    return system_info_list


def build_test_metadata(args):
    """Build test metadata dictionary from command line arguments."""
    return {
        "pytest_run_id": args.pytest_run_id,
        "pytest_run_date": args.pytest_run_date,
        "pytest_run_number": args.pytest_run_number,
        "pytest_branch": args.pytest_branch,
        "pytest_run_url": args.pytest_run_url,
        "gtest_run_id": args.gtest_run_id,
        "gtest_run_date": args.gtest_run_date,
        "gtest_run_number": args.gtest_run_number,
        "gtest_branch": args.gtest_branch,
        "gtest_run_url": args.gtest_run_url,
        "baseline_pytest_run_id": args.baseline_pytest_run_id,
        "baseline_pytest_run_date": args.baseline_pytest_run_date,
        "baseline_pytest_run_number": args.baseline_pytest_run_number,
        "baseline_pytest_branch": args.baseline_pytest_branch,
        "baseline_pytest_run_url": args.baseline_pytest_run_url,
        "baseline_gtest_run_id": args.baseline_gtest_run_id,
        "baseline_gtest_run_date": args.baseline_gtest_run_date,
        "baseline_gtest_run_number": args.baseline_gtest_run_number,
        "baseline_gtest_branch": args.baseline_gtest_branch,
        "baseline_gtest_run_url": args.baseline_gtest_run_url,
    }


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description="Combine pytest and gtest reports")
    parser.add_argument(
        "--pytest-dir", required=True, help="Directory containing pytest HTML reports"
    )
    parser.add_argument(
        "--gtest-dir", required=True, help="Directory containing gtest log files"
    )
    parser.add_argument("--output-excel", required=True, help="Output Excel file path")
    parser.add_argument("--output-html", required=True, help="Output HTML file path")
    parser.add_argument(
        "--system-info-dir", help="Directory containing system info files"
    )

    # Test metadata arguments
    parser.add_argument("--pytest-run-id", help="Pytest workflow run ID")
    parser.add_argument("--pytest-run-date", help="Pytest workflow run date")
    parser.add_argument("--pytest-run-number", help="Pytest workflow run number")
    parser.add_argument("--pytest-branch", help="Pytest workflow branch")
    parser.add_argument("--pytest-run-url", help="Pytest workflow run URL")
    parser.add_argument("--gtest-run-id", help="GTest workflow run ID")
    parser.add_argument("--gtest-run-date", help="GTest workflow run date")
    parser.add_argument("--gtest-run-number", help="GTest workflow run number")
    parser.add_argument("--gtest-branch", help="GTest workflow branch")
    parser.add_argument("--gtest-run-url", help="GTest workflow run URL")

    # Baseline (regression comparison) arguments
    parser.add_argument(
        "--baseline-pytest-dir",
        help="Directory containing baseline pytest HTML reports for regression comparison",
    )
    parser.add_argument(
        "--baseline-gtest-dir",
        help="Directory containing baseline gtest log files for regression comparison",
    )
    parser.add_argument("--baseline-pytest-run-id", help="Baseline pytest run ID")
    parser.add_argument("--baseline-pytest-run-date", help="Baseline pytest run date")
    parser.add_argument(
        "--baseline-pytest-run-number", help="Baseline pytest run number"
    )
    parser.add_argument("--baseline-pytest-branch", help="Baseline pytest branch")
    parser.add_argument("--baseline-pytest-run-url", help="Baseline pytest run URL")
    parser.add_argument("--baseline-gtest-run-id", help="Baseline gtest run ID")
    parser.add_argument("--baseline-gtest-run-date", help="Baseline gtest run date")
    parser.add_argument(
        "--baseline-gtest-run-number", help="Baseline gtest run number"
    )
    parser.add_argument("--baseline-gtest-branch", help="Baseline gtest branch")
    parser.add_argument("--baseline-gtest-run-url", help="Baseline gtest run URL")

    args = parser.parse_args()

    # Validate input directories
    if not Path(args.pytest_dir).exists():
        print(f"Error: Pytest directory not found: {args.pytest_dir}")
        sys.exit(1)

    if not Path(args.gtest_dir).exists():
        print(f"Error: GTest directory not found: {args.gtest_dir}")
        sys.exit(1)

    # Collect all test data
    print("Collecting pytest reports...")
    pytest_data = collect_pytest_reports(args.pytest_dir)

    print("Collecting gtest reports...")
    gtest_data = collect_gtest_reports(args.gtest_dir)

    print("Collecting system information...")
    system_info_list = collect_system_info(args.system_info_dir)

    # Build metadata
    test_metadata = build_test_metadata(args)

    # Validate we have some data
    if not pytest_data and not gtest_data:
        print("Error: No test data found in either pytest or gtest directories")
        sys.exit(1)

    # Collect baseline data for regression comparison
    regression_data = None
    baseline_pytest_dir = args.baseline_pytest_dir
    baseline_gtest_dir = args.baseline_gtest_dir

    if baseline_pytest_dir or baseline_gtest_dir:
        baseline_pytest = []
        baseline_gtest = []
        if baseline_pytest_dir and Path(baseline_pytest_dir).exists():
            print("Collecting baseline pytest reports...")
            baseline_pytest = collect_pytest_reports(baseline_pytest_dir)
        if baseline_gtest_dir and Path(baseline_gtest_dir).exists():
            print("Collecting baseline gtest reports...")
            baseline_gtest = collect_gtest_reports(baseline_gtest_dir)

        if baseline_pytest or baseline_gtest:
            print("Analyzing regressions...")
            regression_data = analyze_regressions(
                pytest_data, gtest_data, baseline_pytest, baseline_gtest
            )
            print(
                f"  Regressions: {len(regression_data['regressions'])}, "
                f"Fixes: {len(regression_data['fixes'])}, "
                f"New failures: {len(regression_data['new_failures'])}"
            )

    # Generate reports
    print("\nGenerating Excel report...")
    generate_excel_report(
        pytest_data,
        gtest_data,
        args.output_excel,
        system_info_list,
        test_metadata,
        regression_data,
    )

    print("Generating HTML report...")
    generate_html_report(
        pytest_data,
        gtest_data,
        args.output_html,
        system_info_list,
        test_metadata,
        regression_data,
    )

    # Print summary
    print("\nSummary:")
    print(f"  Pytest reports processed: {len(pytest_data)}")
    print(f"  GTest categories processed: {len(gtest_data)}")
    print(f"  System info entries: {len(system_info_list)}")
    print(f"  Excel report: {args.output_excel}")
    print(f"  HTML report: {args.output_html}")


if __name__ == "__main__":
    main()
