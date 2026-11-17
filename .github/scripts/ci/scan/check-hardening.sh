#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Fails when an x86-64 executable or shared object under the given paths lacks
# a mark that the compiler hardening of doc/build.md leaves: full RELRO
# (-z relro -z now), a non-executable stack, PIE, CET IBT and SHSTK
# (-fcf-protection=full in each object linked in), and the stack protector, as
# an import of __stack_chk_fail, which needs one protected function in the file.
# _FORTIFY_SOURCE leaves no reliable mark: a library without a fortified call
# imports no __*_chk function. meson builds its configure checks (meson-private)
# without the project flags, so they are skipped. Every check needs a positive
# match, so a readelf that prints something else fails the file instead of
# passing it, an ELF file that readelf can't read fails, and so does a path
# without an x86-64 ELF file. The table below exempts a dependency, by file
# name, from a mark that it can't carry. With HARDENING_TOOLS_DIR set, the
# scanners that hardening-tools.sh fetches there check the same files, with the
# same table. With HARDENING_REPORT set, the marks of each file go to that file
# as Markdown, the failing files first, then the verdict of the scanners, and
# also to GITHUB_STEP_SUMMARY when that is set.

set -euo pipefail

(($#)) || {
	echo "usage: check-hardening.sh PATH..." >&2
	exit 2
}

checked=0
failed=0
failing=()
passing=()
empty=()
scanned=0
declare -A groups=()
for path; do
	before=$checked
	while IFS= read -r -d '' file; do
		[[ $(head -c 4 "$file" | tr -d '\0') == $'\177ELF' ]] || continue
		if ! elf=$(readelf -W -h -l -d -n --dyn-syms "$file" 2>&1); then
			echo "::error::readelf cannot read ${file}: ${elf}" >&2
			checked=$((checked + 1))
			failed=$((failed + 1))
			failing+=("| \`${file}\` | readelf can't read it | | | | | | |")
			continue
		fi
		grep -q 'Machine: *Advanced Micro Devices X86-64' <<<"$elf" || continue
		grep -qE 'Type: *(DYN|EXEC) ' <<<"$elf" || continue
		checked=$((checked + 1))
		missing=()
		grep -q ' GNU_RELRO ' <<<"$elf" || missing+=(RELRO)
		grep -qE '\(FLAGS\) .*BIND_NOW|\(FLAGS_1\) .*Flags:.* NOW' <<<"$elf" || missing+=(BIND_NOW)
		grep -qE ' GNU_STACK .* RW ' <<<"$elf" || missing+=(NX)
		grep -qE 'Type: *DYN ' <<<"$elf" || missing+=(PIE)
		grep -q 'x86 feature: .*IBT' <<<"$elf" || missing+=(IBT)
		grep -q 'x86 feature: .*SHSTK' <<<"$elf" || missing+=(SHSTK)
		grep -q ' UND __stack_chk_fail@' <<<"$elf" || missing+=(SSP)
		# What a dependency can't carry, by file name
		case ${file##*/} in
		# No function that the stack protector covers
		librte_kvargs.so.* | librte_meter.so.* | librte_jobstats.so.* | librte_common_cpt.so.*) accepted=(SSP) ;;
		# The assembly has no CET mark
		libopenh264.so.*) accepted=(IBT SHSTK) ;;
		*) accepted=() ;;
		esac
		row="| \`${file}\` |"
		for mark in RELRO BIND_NOW NX PIE IBT SHSTK SSP; do
			if [[ " ${missing[*]} " != *" ${mark} "* ]]; then
				row+=" yes |"
			elif [[ " ${accepted[*]} " == *" ${mark} "* ]]; then
				row+=" accepted |"
			else
				row+=" no |"
			fi
		done
		for i in "${!missing[@]}"; do
			[[ " ${accepted[*]} " != *" ${missing[i]} "* ]] || unset 'missing[i]'
		done
		if ((${#missing[@]})); then
			echo "::error::${file} is not hardened: no ${missing[*]}" >&2
			failed=$((failed + 1))
			failing+=("${row}")
		else
			passing+=("${row}")
		fi
		# For the scanners, by the checks that apply. checksec has no PIE for a
		# library, and both check IBT and SHSTK as one.
		if [[ -n ${HARDENING_TOOLS_DIR:-} ]]; then
			hc="-B --nofortify" keys="relro,nx,canary,cfi"
			grep -q ' INTERP ' <<<"$elf" && keys+=",pie"
			[[ " ${accepted[*]} " != *" SSP "* ]] || hc+=" -s" keys=${keys/,canary/}
			[[ " ${accepted[*]} " != *" IBT "* && " ${accepted[*]} " != *" SHSTK "* ]] || hc+=" -x" keys=${keys/,cfi/}
			groups["${hc}|${keys}"]+="${file}"$'\n'
			scanned=$((scanned + 1))
		fi
	done < <(find "$path" -name meson-private -prune -o -type f -print0)
	((checked > before)) || {
		echo "::error::no x86-64 ELF file found under $path" >&2
		empty+=("${path}")
	}
done

# Without -B and --nofortify, hardening-check also wants AArch64 branch
# protection, and fails a file with a libc call of a size it doesn't know.
tools=ok
errors=
for group in "${!groups[@]}"; do
	read -ra hc <<<"${group%|*}"
	if ! out=$(printf '%s' "${groups[$group]}" | tr '\n' '\0' |
		xargs -0 -n 1 -P "$(nproc)" perl "${HARDENING_TOOLS_DIR}/hardening-check.pl" --quiet "${hc[@]}" 2>&1); then
		tools=failed
		errors+=$(awk '/^[^ ].*:$/ { file = substr($0, 1, length($0) - 1); next }
			/\(ignored\)$/ { next }
			/^ .*: no/ { sub(/^ +/, ""); sub(/:.*/, ""); print "::error::hardening-check: " file ": " $0; next }
			{ print "::error::hardening-check: " $0 }' <<<"${out}")$'\n'
	fi
	if ! out=$(printf '%s' "${groups[$group]}" |
		"${HARDENING_TOOLS_DIR}/checksec" listfile - --no-banner --color never --fail-if "${group#*|}" 2>&1 >/dev/null); then
		tools=failed
		if grep -q '^  .*: [a-z_]* = ' <<<"${out}"; then
			errors+=$(sed -nE 's/^  (.*): ([a-z_]+ = .*)$/::error::checksec: \1: \2/p' <<<"${out}")$'\n'
		else
			errors+="::error::checksec: ${out//$'\n'/$'\n'::error::checksec: }"$'\n'
		fi
	fi
done
printf '%s' "${errors}" >&2

echo "hardening: ${checked} ELF files checked under $*, ${failed} failed${HARDENING_TOOLS_DIR:+, hardening-check and checksec ${tools}}"
if [[ -n ${HARDENING_REPORT:-} ]]; then
	{
		printf '### Compiler hardening\n\n| Files checked | Failed in readelf | Under |\n|---:|---:|---|\n'
		printf '| %s | %s | %s |\n\n' "${checked}" "${failed}" "\`$*\`"
		for path in "${empty[@]}"; do
			printf "No x86-64 ELF file found under \`%s\`.\n\n" "${path}"
		done
		printf '| File | RELRO | BIND_NOW | NX | PIE | IBT | SHSTK | SSP |\n|---|---|---|---|---|---|---|---|\n'
		{
			printf '%s\n' "${failing[@]}" | LC_ALL=C sort
			printf '%s\n' "${passing[@]}" | LC_ALL=C sort
		} | grep . || true
		printf '\nno: missing. accepted: missing, and accepted for that dependency by the table of check-hardening.sh.\n'
		if [[ -n ${HARDENING_TOOLS_DIR:-} ]]; then
			printf '\n### hardening-check and checksec\n\n'
			if [[ ${tools} == ok ]]; then
				printf 'Both pass the %s files they checked, with the gaps the table accepts.\n' "${scanned}"
			else
				printf 'They fail:\n\n'
				sed -E 's/^::error::/- /' <<<"${errors}" | grep .
			fi
		fi
	} >"${HARDENING_REPORT}"
	[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || cat "${HARDENING_REPORT}" >>"${GITHUB_STEP_SUMMARY}"
fi
((failed == 0 && ${#empty[@]} == 0)) && [[ ${tools} == ok ]]
