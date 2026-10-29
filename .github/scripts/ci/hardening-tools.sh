#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Runs one of the two binary-hardening scanners, hardening-check (devscripts) or
# checksec (slimm609/checksec), over every x86-64 executable and shared object
# under the given directories, and fails when a file lacks a protection or a
# directory holds no such file.
# --nostackprotector drops the stack canary check for a tree whose small
# libraries have no function that needs a canary (DPDK). hardening-check's
# FORTIFY heuristic is always skipped: it fails a correctly built file whose
# libc calls have no known size. libopenh264 is scanned without the CET check,
# as in check-hardening.sh: its assembly carries no CET mark.

set -euo pipefail

usage() {
	echo "usage: hardening-tools.sh hardening-check|checksec [--nostackprotector] DIR..." >&2
	exit 2
}

tool=${1:-}
shift || usage
canary=1
if [[ ${1:-} == --nostackprotector ]]; then
	canary=0
	shift
fi
(($#)) || usage

executables=()
libraries=()
no_cet=()
checked=0
for dir; do
	before=$checked
	while IFS= read -r -d '' file; do
		elf=$(readelf -W -h -l "$file" 2>/dev/null) || continue
		grep -q 'Machine: *Advanced Micro Devices X86-64' <<<"$elf" || continue
		grep -qE 'Type: *(DYN|EXEC) ' <<<"$elf" || continue
		checked=$((checked + 1))
		if grep -q ' INTERP ' <<<"$elf"; then
			executables+=("$file")
		elif [[ ${file##*/} == libopenh264.so* ]]; then
			no_cet+=("$file")
		else
			libraries+=("$file")
		fi
	done < <(find "$dir" -type f -print0)
	((checked > before)) || {
		echo "::error::no x86-64 ELF file found under $dir" >&2
		exit 1
	}
done

# The report goes to stdout and, in CI, to the job summary; every file that
# fails a check gets a ::error:: annotation on stderr.
report() {
	cat "$1"
	[[ -n ${GITHUB_STEP_SUMMARY:-} ]] && cat "$1" >>"$GITHUB_STEP_SUMMARY"
	return 0
}
report_file=$(mktemp)
trap 'rm -f "$report_file"' EXIT

failed=0
case "$tool" in
hardening-check)
	opts=(--quiet --nofortify)
	((canary)) || opts+=(--nostackprotector)
	out=$(hardening-check "${opts[@]}" "${executables[@]}" "${libraries[@]}" 2>&1) || failed=1
	if ((${#no_cet[@]})); then
		no_cet_out=$(hardening-check "${opts[@]}" --nocfprotection "${no_cet[@]}" 2>&1) || failed=1
		out+=${no_cet_out:+$'\n'$no_cet_out}
	fi
	# --quiet prints a file only when a check failed: "path:" then " Check: no, ..."
	awk '/^[^ ].*:$/ { file = substr($0, 1, length($0) - 1) }
	     /^ .*: no, / && !/\(ignored\)/ { sub(/^ +/, ""); sub(/:.*/, ""); print "::error::" file ": " $0 }' <<<"$out" >&2
	{
		echo "### hardening-check: ${checked} files under $*"
		echo
		if [[ -n $out ]]; then
			printf '~~~\n%s\n~~~\n' "$out"
		else
			echo "All checks pass."
		fi
	} >"$report_file"
	;;
checksec)
	checks=relro,nx
	((canary)) && checks+=,canary
	rows=$(mktemp)
	scan() { # scan <fail-if keys> FILE...
		local keys=$1 json
		shift
		json=$(printf '%s\n' "$@" | checksec listfile - --no-banner --color never -o json --fail-if "$keys" 2>/dev/null) || failed=1
		jq -r --arg keys "$keys" '.[] | . as $f | [$keys | split(",")[] | select($f.checks[.].status != "green")]
			| select(length > 0) | "::error::\($f.name): \(join(", "))"' <<<"$json" >&2
		jq -r '.[] | "| \(.checks.relro.value) | \(.checks.canary.value) | \(.checks.cfi.value) | \(.checks.nx.value) | \(.checks.pie.value) | \(.name) |"' <<<"$json" >>"$rows"
	}
	((${#executables[@]})) && scan "${checks},cfi,pie" "${executables[@]}"
	((${#libraries[@]})) && scan "${checks},cfi" "${libraries[@]}"
	((${#no_cet[@]})) && scan "$checks" "${no_cet[@]}"
	{
		echo "### checksec: ${checked} files under $*"
		echo
		echo "| RELRO | Canary | CFI | NX | PIE | File |"
		echo "| --- | --- | --- | --- | --- | --- |"
		cat "$rows"
	} >"$report_file"
	rm -f "$rows"
	;;
*) usage ;;
esac
report "$report_file"

echo "${tool}: ${checked} ELF files checked under $*, failed=${failed}"
((failed == 0))
