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
    """Collect and parse system information files."""
    system_info_list = []

    if not system_info_dir or not Path(system_info_dir).exists():
        return system_info_list

    info_files = list(Path(system_info_dir).rglob("system_info.txt"))
    print(f"Found {len(info_files)} system info files")

    for info_file in info_files:
        try:
            info = parse_system_info(info_file)
            system_info_list.append(info)
        except Exception as e:
            print(f"Error parsing {info_file}: {e}")

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

    # Generate reports
    print("\nGenerating Excel report...")
    generate_excel_report(
        pytest_data, gtest_data, args.output_excel, system_info_list, test_metadata
    )

    print("Generating HTML report...")
    generate_html_report(
        pytest_data, gtest_data, args.output_html, system_info_list, test_metadata
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
