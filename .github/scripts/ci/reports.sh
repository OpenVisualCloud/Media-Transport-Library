#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

flatten_reports() {
	local directory=$1 pattern=$2 source_name=$3 extension=$4
	if [[ ! -d ${root_dir}/${directory} ]]; then
		# The download that fills this directory is allowed to find nothing: a
		# source run that failed before uploading has no report artifacts, and
		# the baseline downloads are continue-on-error. Nothing to flatten is
		# not an error here -- combine-all is what decides whether enough data
		# arrived.
		echo "::warning::No ${directory} directory; no ${extension} reports to flatten"
		return 0
	fi
	cd "${root_dir}/${directory}"
	for report_dir in ./${pattern}; do
		if [[ -d $report_dir && -f $report_dir/$source_name ]]; then
			mv "$report_dir/$source_name" "${report_dir}.${extension}"
			rm -rf "$report_dir"
		fi
	done
	ls -lh ./*."$extension" || echo "No ${extension} reports found"
}

case "${1:-}" in
status)
	"${root_dir}/script/status_report.sh"
	for status_dir in "${root_dir}"/mtl_system_status_*; do
		if [[ -d $status_dir ]]; then
			mv "$status_dir" "${root_dir}/${STATUS_NAME:?STATUS_NAME is required}"
			break
		fi
	done
	;;
performance)
	acceptance_dir="${root_dir}/tests/acceptance"
	# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
	. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"
	if [[ -d ${acceptance_dir}/logs/performance ]]; then
		# shellcheck disable=SC2154 # venv_python comes from mtl_acceptance_venv.sh, sourced above
		"${venv_python}" "${acceptance_dir}/common/generate_report.py" \
			"${acceptance_dir}/logs/performance" -o "${acceptance_dir}/performance_report.html" ||
			echo "::warning::Performance report generation failed"
	else
		echo "::warning::No performance logs found; skipping report generation"
	fi
	;;
# No default for REPORT_DIR: the download-artifact `path:` differs per caller
# (python-reports, pytest-reports, baseline-pytest-reports, ...), so any default
# would silently be wrong for every caller but one. Each caller names its own.
flatten-pytest) flatten_reports "${REPORT_DIR:?REPORT_DIR is required}" 'nightly-test-report-*' report.html html ;;
flatten-gtest) flatten_reports "${REPORT_DIR:?REPORT_DIR is required}" 'nightly-gtest-report-*' gtest.log log ;;
list-system-info)
	if [[ -d ${root_dir}/system-info-reports ]]; then
		echo "System info reports found:"
		ls -lah "${root_dir}/system-info-reports"
	else
		echo "No system info reports found (this is optional)"
	fi
	;;
install-dependencies)
	python3 -m pip install --upgrade pip
	python3 -m pip install pandas beautifulsoup4 openpyxl ${REPORT_REQUIRE_LXML:+lxml}
	;;
combine-pytest)
	timestamp=$(date -u +%Y%m%d_%H%M%S)
	output_path="${root_dir}/python-reports/nightly_pytest_report_${timestamp}.html"
	temporary_dir=$(mktemp -d)
	trap 'rm -rf "$temporary_dir"' EXIT
	"${REPORT_PYTHON:-python3}" "${root_dir}/.github/scripts/combine_all_reports.py" \
		--pytest-dir "${root_dir}/python-reports" \
		--gtest-dir "$temporary_dir" \
		--output-excel "${temporary_dir}/pytest-report.xlsx" \
		--output-html "$output_path"
	if [[ -z $output_path || ! -f $output_path ]]; then
		echo "Combined report not found at ${output_path:-<empty>}" >&2
		ls -la "${root_dir}/python-reports" >&2
		exit 1
	fi
	echo "Combined report path: ${output_path}"
	printf 'report_path=%s\n' "$output_path" >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	;;
combine-all)
	baseline_args=()
	if [[ -d ${root_dir}/baseline-pytest-reports ]]; then
		baseline_args+=(--baseline-pytest-dir baseline-pytest-reports
			--baseline-pytest-run-id "${BASELINE_PYTEST_RUN_ID:-}"
			--baseline-pytest-run-date "${BASELINE_PYTEST_RUN_DATE:-}"
			--baseline-pytest-run-number "${BASELINE_PYTEST_RUN_NUMBER:-}"
			--baseline-pytest-branch "${BASELINE_PYTEST_BRANCH:-}"
			--baseline-pytest-run-url "${BASELINE_PYTEST_RUN_URL:-}")
	fi
	if [[ -d ${root_dir}/baseline-gtest-reports ]]; then
		baseline_args+=(--baseline-gtest-dir baseline-gtest-reports
			--baseline-gtest-run-id "${BASELINE_GTEST_RUN_ID:-}"
			--baseline-gtest-run-date "${BASELINE_GTEST_RUN_DATE:-}"
			--baseline-gtest-run-number "${BASELINE_GTEST_RUN_NUMBER:-}"
			--baseline-gtest-branch "${BASELINE_GTEST_BRANCH:-}"
			--baseline-gtest-run-url "${BASELINE_GTEST_RUN_URL:-}")
	fi
	cd "$root_dir"
	python3 .github/scripts/combine_all_reports.py \
		--pytest-dir pytest-reports --gtest-dir gtest-reports \
		--system-info-dir system-info-reports \
		--output-excel combined_nightly_report.xlsx \
		--output-html combined_nightly_report.html \
		--pytest-run-id "${PYTEST_RUN_ID:?PYTEST_RUN_ID is required}" \
		--pytest-run-date "${PYTEST_RUN_DATE:?PYTEST_RUN_DATE is required}" \
		--pytest-run-number "${PYTEST_RUN_NUMBER:?PYTEST_RUN_NUMBER is required}" \
		--pytest-branch "${PYTEST_BRANCH:?PYTEST_BRANCH is required}" \
		--pytest-run-url "${PYTEST_RUN_URL:?PYTEST_RUN_URL is required}" \
		--gtest-run-id "${GTEST_RUN_ID:?GTEST_RUN_ID is required}" \
		--gtest-run-date "${GTEST_RUN_DATE:?GTEST_RUN_DATE is required}" \
		--gtest-run-number "${GTEST_RUN_NUMBER:?GTEST_RUN_NUMBER is required}" \
		--gtest-branch "${GTEST_BRANCH:?GTEST_BRANCH is required}" \
		--gtest-run-url "${GTEST_RUN_URL:?GTEST_RUN_URL is required}" \
		"${baseline_args[@]}"
	[[ -f combined_nightly_report.xlsx && -f combined_nightly_report.html ]]
	echo 'reports_generated=true' >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	;;
smoke-summary)
	{
		echo '## Smoke Tests Report'
		echo
		if [[ -f ${root_dir}/report.json ]]; then
			# pytest-json-report counts outcomes, so the keys are the outcome
			# names: 'error', not 'errors'. It also publishes its own total,
			# which covers outcomes this table does not list (xfailed,
			# xpassed) -- summing the four below under-reports the run.
			passed=$(jq '.summary.passed // 0' "${root_dir}/report.json")
			failed=$(jq '.summary.failed // 0' "${root_dir}/report.json")
			skipped=$(jq '.summary.skipped // 0' "${root_dir}/report.json")
			errors=$(jq '.summary.error // 0' "${root_dir}/report.json")
			total=$(jq '.summary.total // 0' "${root_dir}/report.json")
			echo '| Status | Count |'
			echo '| ------ | ----- |'
			printf '| Passed | %s |\n| Failed | %s |\n| Error | %s |\n| Skipped | %s |\n\n' \
				"$passed" "$failed" "$errors" "$skipped"
			printf '**Total Tests:** %s\n\n' "$total"
			if ((failed > 0 || errors > 0)); then
				echo '**Some tests failed.** Please check the detailed report.'
			else
				echo '**All tests passed.**'
			fi
			echo
			printf '[Download Full HTML Report](https://github.com/%s/actions/runs/%s/artifacts/%s)\n' \
				"${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}" \
				"${GITHUB_RUN_ID:?GITHUB_RUN_ID is required}" \
				"${ARTIFACT_ID:?ARTIFACT_ID is required}"
		else
			echo 'No report.json file was generated'
		fi
	} >>"${GITHUB_STEP_SUMMARY:?GITHUB_STEP_SUMMARY is required}"
	;;
*)
	echo "Usage: $0 {status|performance|flatten-pytest|flatten-gtest|list-system-info|install-dependencies|combine-pytest|combine-all|smoke-summary}" >&2
	exit 2
	;;
esac
