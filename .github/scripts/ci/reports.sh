#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."

flatten_reports() {
	local directory=$1 pattern=$2 source_name=$3 extension=$4
	cd "$directory"
	for report_dir in ./${pattern}; do
		[[ -f $report_dir/$source_name ]] || continue
		mv "$report_dir/$source_name" "${report_dir}.${extension}"
		rm -rf "$report_dir"
	done
	ls -lh ./*."$extension" || echo "No ${extension} reports found"
}

case "${1:-}" in
status)
	script/status_report.sh
	for status_dir in mtl_system_status_*/; do
		[[ -d $status_dir ]] && mv "$status_dir" "${STATUS_NAME:?}"
		break
	done
	;;
performance)
	# shellcheck source=../lib/mtl_acceptance_venv.sh disable=SC1091
	. .github/scripts/lib/mtl_acceptance_venv.sh
	cd tests/acceptance
	if [[ ! -d logs/performance ]]; then
		echo "::warning::No performance logs found; skipping report generation"
		exit 0
	fi
	# shellcheck disable=SC2154 # set by mtl_acceptance_venv.sh
	"$venv_python" common/generate_report.py logs/performance -o performance_report.html ||
		echo "::warning::Performance report generation failed"
	# upload-artifact rejects the colons in the timestamped log directory names.
	tar -czf perf-logs.tar.gz logs/performance || echo "::warning::Packing the performance logs failed"
	;;
flatten-pytest) flatten_reports "${REPORT_DIR:-python-reports}" 'nightly-test-report-*' report.html html ;;
flatten-gtest) flatten_reports "${REPORT_DIR:-gtest-reports}" 'nightly-gtest-report-*' gtest.log log ;;
list-system-info) ls -lah system-info-reports || echo "No system info reports found (this is optional)" ;;
install-dependencies)
	python3 -m pip install --upgrade pip
	python3 -m pip install pandas beautifulsoup4 openpyxl ${REPORT_REQUIRE_LXML:+lxml}
	;;
combine-pytest)
	output_path="$PWD/python-reports/nightly_pytest_report_$(date -u +%Y%m%d_%H%M%S).html"
	temporary_dir=$(mktemp -d)
	trap 'rm -rf "$temporary_dir"' EXIT
	python3 .github/scripts/combine_all_reports.py --pytest-dir python-reports --gtest-dir "$temporary_dir" \
		--output-excel "$temporary_dir/pytest-report.xlsx" --output-html "$output_path"
	[[ -f $output_path ]]
	echo "report_path=$output_path" >>"$GITHUB_OUTPUT"
	;;
combine-all)
	args=(--system-info-dir system-info-reports
		--output-excel combined_nightly_report.xlsx --output-html combined_nightly_report.html)
	for kind in pytest gtest baseline-pytest baseline-gtest; do
		[[ $kind != baseline-* || -d $kind-reports ]] || continue
		args+=("--$kind-dir" "$kind-reports")
		for field in run-id run-date run-number branch run-url; do
			var=${kind^^}_${field^^}
			var=${var//-/_}
			[[ $kind == baseline-* || -n ${!var:-} ]] || { echo "$var is required" >&2 && exit 1; }
			args+=("--$kind-$field" "${!var:-}")
		done
	done
	python3 .github/scripts/combine_all_reports.py "${args[@]}"
	[[ -f combined_nightly_report.xlsx && -f combined_nightly_report.html ]]
	echo 'reports_generated=true' >>"$GITHUB_OUTPUT"
	;;
smoke-summary)
	{
		echo '## Smoke Tests Report'
		echo
		if [[ ! -f report.json ]]; then
			echo 'No report.json file was generated'
			exit 0
		fi
		# pytest-json-report keys are outcome names ('error'), and its total also counts xfail/xpass.
		jq -r '({passed: 0, failed: 0, error: 0, skipped: 0, total: 0} + .summary) |
			"| Status | Count |\n| ------ | ----- |\n| Passed | \(.passed) |\n| Failed | \(.failed) |\n| Error | \(.error) |\n| Skipped | \(.skipped) |\n",
			"**Total Tests:** \(.total)\n",
			if .failed > 0 or .error > 0 then "**Some tests failed.** Please check the detailed report." else "**All tests passed.**" end,
			""' report.json
		if [[ -n ${ARTIFACT_ID:-} ]]; then
			echo "[Download Full HTML Report](https://github.com/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/artifacts/$ARTIFACT_ID)"
		fi
	} >>"$GITHUB_STEP_SUMMARY"
	;;
*) exit 2 ;;
esac
