"""HTML report generation for MTL nightly test reports."""

from datetime import datetime

import pandas as pd


def generate_html_report(
    pytest_data, gtest_data, output_file, system_info_list=None, test_metadata=None
):
    """Create HTML report combining pytest and gtest results."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S UTC")

    # Calculate statistics
    stats = _calculate_statistics(pytest_data, gtest_data)

    # Generate HTML sections
    test_run_info = _generate_test_run_info(test_metadata)
    overall_summary = _generate_overall_summary(stats)
    pytest_summary = _generate_pytest_summary(stats) if pytest_data else ""
    gtest_summary = _generate_gtest_summary(stats) if gtest_data else ""
    system_info_section = (
        _generate_system_info(system_info_list) if system_info_list else ""
    )
    pytest_table = _generate_pytest_table(pytest_data) if pytest_data else ""
    gtest_table = _generate_gtest_table(gtest_data) if gtest_data else ""

    # Assemble final HTML
    html = _get_html_template().format(
        timestamp=timestamp,
        test_run_info=test_run_info,
        overall_summary=overall_summary,
        pytest_summary=pytest_summary,
        gtest_summary=gtest_summary,
        system_info_section=system_info_section,
        pytest_table=pytest_table,
        gtest_table=gtest_table,
    )

    with open(output_file, "w") as f:
        f.write(html)

    print(f"HTML report saved to: {output_file}")


def _calculate_statistics(pytest_data, gtest_data):
    """Calculate test statistics from data."""
    pytest_total = pytest_passed = pytest_failed = pytest_skipped = 0
    gtest_total = gtest_passed = gtest_failed = gtest_skipped = 0

    if pytest_data:
        df_pytest = pd.DataFrame(
            [{k: v for k, v in d.items() if k != "test_cases"} for d in pytest_data]
        )
        pytest_total = df_pytest["total"].sum()
        pytest_passed = df_pytest["passed"].sum()
        pytest_failed = df_pytest["failed"].sum()
        pytest_skipped = df_pytest["skipped"].sum()

    if gtest_data:
        df_gtest = pd.DataFrame(
            [{k: v for k, v in d.items() if k != "test_cases"} for d in gtest_data]
        )
        gtest_total = df_gtest["total"].sum()
        gtest_passed = df_gtest["passed"].sum()
        gtest_failed = df_gtest["failed"].sum()
        gtest_skipped = df_gtest["skipped"].sum()

    combined_total = pytest_total + gtest_total
    combined_passed = pytest_passed + gtest_passed
    combined_failed = pytest_failed + gtest_failed
    combined_skipped = pytest_skipped + gtest_skipped

    # Calculate pass rates (excluding skipped)
    pytest_pass_rate = (
        (pytest_passed / (pytest_passed + pytest_failed) * 100)
        if (pytest_passed + pytest_failed) > 0
        else 0
    )
    gtest_pass_rate = (
        (gtest_passed / (gtest_passed + gtest_failed) * 100)
        if (gtest_passed + gtest_failed) > 0
        else 0
    )
    combined_pass_rate = (
        (combined_passed / (combined_passed + combined_failed) * 100)
        if (combined_passed + combined_failed) > 0
        else 0
    )

    return {
        "pytest": {
            "total": pytest_total,
            "passed": pytest_passed,
            "failed": pytest_failed,
            "skipped": pytest_skipped,
            "pass_rate": pytest_pass_rate,
        },
        "gtest": {
            "total": gtest_total,
            "passed": gtest_passed,
            "failed": gtest_failed,
            "skipped": gtest_skipped,
            "pass_rate": gtest_pass_rate,
        },
        "combined": {
            "total": combined_total,
            "passed": combined_passed,
            "failed": combined_failed,
            "skipped": combined_skipped,
            "pass_rate": combined_pass_rate,
        },
    }


def _generate_test_run_info(test_metadata):
    """Generate test run information section."""
    if not test_metadata:
        return ""

    html = '<div class="summary"><h2>Test Run Information</h2><table>'
    html += (
        "<tr><th>Test Suite</th><th>Run</th><th>Branch</th><th>Date & Time</th></tr>"
    )

    if test_metadata.get("pytest_run_number") and test_metadata.get("pytest_branch"):
        run_date = (
            test_metadata["pytest_run_date"].split("T")[0]
            if test_metadata.get("pytest_run_date")
            else "N/A"
        )
        run_time = (
            test_metadata["pytest_run_date"].split("T")[1].split("Z")[0]
            if test_metadata.get("pytest_run_date")
            and "T" in test_metadata["pytest_run_date"]
            else "N/A"
        )
        run_link = (
            f'<a href="{test_metadata["pytest_run_url"]}" target="_blank">#{test_metadata["pytest_run_number"]}</a>'
            if test_metadata.get("pytest_run_url")
            else f'#{test_metadata["pytest_run_number"]}'
        )
        html += (
            f"<tr><td>Pytest</td><td>{run_link}</td>"
            f'<td>{test_metadata["pytest_branch"]}</td><td>{run_date} {run_time}</td></tr>'
        )

    if test_metadata.get("gtest_run_number") and test_metadata.get("gtest_branch"):
        run_date = (
            test_metadata["gtest_run_date"].split("T")[0]
            if test_metadata.get("gtest_run_date")
            else "N/A"
        )
        run_time = (
            test_metadata["gtest_run_date"].split("T")[1].split("Z")[0]
            if test_metadata.get("gtest_run_date")
            and "T" in test_metadata["gtest_run_date"]
            else "N/A"
        )
        run_link = (
            f'<a href="{test_metadata["gtest_run_url"]}" target="_blank">#{test_metadata["gtest_run_number"]}</a>'
            if test_metadata.get("gtest_run_url")
            else f'#{test_metadata["gtest_run_number"]}'
        )
        html += (
            f"<tr><td>GTest</td><td>{run_link}</td>"
            f'<td>{test_metadata["gtest_branch"]}</td><td>{run_date} {run_time}</td></tr>'
        )

    html += "</table></div>"
    return html


def _generate_overall_summary(stats):
    """Generate overall summary section."""
    return f"""
    <div class="summary">
        <h2>Overall Summary</h2>
        <div class="summary-grid">
            <div class="summary-card">
                <h3>Total Tests</h3>
                <div class="value">{stats['combined']['total']}</div>
            </div>
            <div class="summary-card passed">
                <h3>Passed</h3>
                <div class="value">{stats['combined']['passed']}</div>
            </div>
            <div class="summary-card failed">
                <h3>Failed</h3>
                <div class="value">{stats['combined']['failed']}</div>
            </div>
            <div class="summary-card skipped">
                <h3>Skipped</h3>
                <div class="value">{stats['combined']['skipped']}</div>
            </div>
            <div class="summary-card pass-rate">
                <h3>Pass Rate</h3>
                <div class="value">{stats['combined']['pass_rate']:.2f}%</div>
            </div>
        </div>
    </div>
    """


def _generate_pytest_summary(stats):
    """Generate pytest summary section."""
    return f"""
    <div class="summary">
        <h2>Pytest Summary</h2>
        <div class="summary-grid">
            <div class="summary-card">
                <h3>Total Tests</h3>
                <div class="value">{stats['pytest']['total']}</div>
            </div>
            <div class="summary-card passed">
                <h3>Passed</h3>
                <div class="value">{stats['pytest']['passed']}</div>
            </div>
            <div class="summary-card failed">
                <h3>Failed</h3>
                <div class="value">{stats['pytest']['failed']}</div>
            </div>
            <div class="summary-card skipped">
                <h3>Skipped</h3>
                <div class="value">{stats['pytest']['skipped']}</div>
            </div>
            <div class="summary-card pass-rate">
                <h3>Pass Rate</h3>
                <div class="value">{stats['pytest']['pass_rate']:.2f}%</div>
            </div>
        </div>
    </div>
    """


def _generate_gtest_summary(stats):
    """Generate gtest summary section."""
    return f"""
    <div class="summary">
        <h2>GTest Summary</h2>
        <div class="summary-grid">
            <div class="summary-card">
                <h3>Total Tests</h3>
                <div class="value">{stats['gtest']['total']}</div>
            </div>
            <div class="summary-card passed">
                <h3>Passed</h3>
                <div class="value">{stats['gtest']['passed']}</div>
            </div>
            <div class="summary-card failed">
                <h3>Failed</h3>
                <div class="value">{stats['gtest']['failed']}</div>
            </div>
            <div class="summary-card skipped">
                <h3>Skipped</h3>
                <div class="value">{stats['gtest']['skipped']}</div>
            </div>
            <div class="summary-card pass-rate">
                <h3>Pass Rate</h3>
                <div class="value">{stats['gtest']['pass_rate']:.2f}%</div>
            </div>
        </div>
    </div>
    """


def _generate_pytest_table(pytest_data):
    """Generate pytest results table with expandable details."""
    html = (
        "<table><thead><tr><th>NIC</th><th>Category</th><th>Passed</th><th>Failed</th>"
        "<th>Skipped</th><th>Error</th><th>XPassed</th><th>XFailed</th><th>Total</th>"
        "</tr></thead><tbody>"
    )

    for idx, data in enumerate(
        sorted(pytest_data, key=lambda x: (x["nic"], x["category"]))
    ):
        detail_id = f"pytest-detail-{idx}"
        html += f"""
        <tr>
            <td>{data['nic']}</td>
            <td><span class="clickable" onclick="toggleDetails('{detail_id}')">{data['category']}</span></td>
            <td>{data['passed']}</td>
            <td>{data['failed']}</td>
            <td>{data['skipped']}</td>
            <td>{data.get('error', 0)}</td>
            <td>{data.get('xpassed', 0)}</td>
            <td>{data.get('xfailed', 0)}</td>
            <td>{data['total']}</td>
        </tr>
        """

        if "test_cases" in data and data["test_cases"]:
            html += f'<tr><td colspan="9"><div id="{detail_id}" class="details">'
            html += (
                f'<h4>Detailed Test Cases for {data["category"]}</h4>'
                "<table><thead><tr><th>Test Name</th><th>Result</th><th>Duration</th>"
                "</tr></thead><tbody>"
            )

            for tc in sorted(data["test_cases"], key=lambda x: x["test_name"]):
                result_class = (
                    f"result-{tc['result'].lower()}"
                    if tc["result"].lower() in ["passed", "failed", "skipped"]
                    else ""
                )
                html += (
                    f'<tr><td>{tc["test_name"]}</td>'
                    f'<td class="{result_class}">{tc["result"]}</td>'
                    f'<td>{tc["duration"]}</td></tr>'
                )

            html += "</tbody></table></div></td></tr>"

    html += "</tbody></table>"
    return html


def _generate_gtest_table(gtest_data):
    """Generate gtest results table with expandable details."""
    html = (
        "<table><thead><tr><th>NIC</th><th>Test Category</th><th>Passed</th>"
        "<th>Failed</th><th>Skipped</th><th>Total</th></tr></thead><tbody>"
    )

    for idx, data in enumerate(
        sorted(gtest_data, key=lambda x: (x["nic"], x["category"]))
    ):
        detail_id = f"gtest-detail-{idx}"
        html += f"""
        <tr>
            <td>{data['nic']}</td>
            <td><span class="clickable" onclick="toggleDetails('{detail_id}')">{data['category']}</span></td>
            <td>{data['passed']}</td>
            <td>{data['failed']}</td>
            <td>{data['skipped']}</td>
            <td>{data['total']}</td>
        </tr>
        """

        if "test_cases" in data and data["test_cases"]:
            html += f'<tr><td colspan="6"><div id="{detail_id}" class="details">'
            html += (
                f'<h4>Detailed Test Cases for {data["category"]}</h4>'
                "<table><thead><tr><th>Test Name</th><th>Result</th><th>Duration</th>"
                "</tr></thead><tbody>"
            )

            for tc in sorted(data["test_cases"], key=lambda x: x["test_name"]):
                result_class = (
                    f"result-{tc['result'].lower()}"
                    if tc["result"].lower() in ["passed", "failed", "skipped"]
                    else ""
                )
                html += (
                    f'<tr><td>{tc["test_name"]}</td>'
                    f'<td class="{result_class}">{tc["result"]}</td>'
                    f'<td>{tc["duration"]}</td></tr>'
                )

            html += "</tbody></table></div></td></tr>"

    html += "</tbody></table>"
    return html


def _generate_system_info(system_info_list):
    """Generate system information section."""
    html = (
        '<div class="summary"><h2>Test Environment Information</h2><table><thead><tr>'
    )
    html += (
        "<th>Hostname</th><th>Platform</th><th>CPU</th><th>Cores</th><th>RAM</th>"
        "<th>HugePages</th><th>OS</th><th>Kernel</th><th>NICs</th>"
    )
    html += "</tr></thead><tbody>"

    for sys_info in system_info_list:
        html += f"""<tr>
            <td>{sys_info.get('hostname', 'unknown')}</td>
            <td>{sys_info.get('platform', 'unknown')}</td>
            <td>{sys_info.get('cpu', 'unknown')}</td>
            <td>{sys_info.get('cpu_cores', 'unknown')}</td>
            <td>{sys_info.get('ram', 'unknown')}</td>
            <td>{sys_info.get('hugepages', 'unknown')}</td>
            <td>{sys_info.get('os', 'unknown')}</td>
            <td>{sys_info.get('kernel', 'unknown')}</td>
            <td>{sys_info.get('nics', 'unknown')}</td>
        </tr>"""

    html += "</tbody></table></div>"
    return html


def _get_html_template():
    """Return the HTML template with CSS and JavaScript."""
    return """<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8"/>
    <title>MTL Nightly Test Report</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }}
        h1 {{ color: #366092; border-bottom: 2px solid #366092; padding-bottom: 10px; }}
        h2 {{ color: #555; margin-top: 30px; }}
        .summary {{
            background: white; padding: 20px; border-radius: 5px;
            margin: 20px 0; box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        .summary-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }}
        .summary-card {{ padding: 15px; border-radius: 5px; text-align: center; }}
        .summary-card h3 {{ margin: 0; font-size: 14px; color: #666; }}
        .summary-card .value {{ font-size: 32px; font-weight: bold; margin: 10px 0; }}
        .passed {{ background-color: #d4edda; color: #155724; }}
        .pass-rate {{ background-color: #cfe2ff; color: #084298; }}
        .failed {{ background-color: #f8d7da; color: #721c24; }}
        .skipped {{ background-color: #fff3cd; color: #856404; }}
        table {{
            border-collapse: collapse; width: 100%; background: white;
            margin: 20px 0; box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        th {{ background-color: #366092; color: white; padding: 12px; text-align: left; }}
        td {{ padding: 10px; border-bottom: 1px solid #ddd; }}
        tr:hover {{ background-color: #f5f5f5; }}
        .timestamp {{ color: #666; font-size: 0.9em; margin-bottom: 20px; }}
        .clickable {{ cursor: pointer; color: #366092; text-decoration: underline; }}
        .clickable:hover {{ color: #1e3a5f; font-weight: bold; }}
        .details {{
            display: none; margin-top: 10px; padding: 10px;
            background-color: #f9f9f9; border-left: 3px solid #366092;
        }}
        .details.show {{ display: block; }}
        .details table {{ margin: 10px 0; font-size: 0.9em; }}
        .details th {{ background-color: #555; font-size: 0.85em; }}
        .result-passed {{ background-color: #d4edda; }}
        .result-failed {{ background-color: #f8d7da; }}
        .result-skipped {{ background-color: #fff3cd; }}
    </style>
    <script>
        function toggleDetails(id) {{
            const element = document.getElementById(id);
            if (element) {{
                element.classList.toggle('show');
            }}
        }}
    </script>
</head>
<body>
    <h1>MTL Nightly Test Report - Combined Results</h1>
    <div class="timestamp">Generated: {timestamp}</div>
    {test_run_info}
    {overall_summary}
    {pytest_summary}
    {gtest_summary}
    {system_info_section}
    <h2>Pytest Results by NIC and Category</h2>
    {pytest_table}
    <h2>GTest Results by NIC and Test Category</h2>
    {gtest_table}
</body>
</html>"""
