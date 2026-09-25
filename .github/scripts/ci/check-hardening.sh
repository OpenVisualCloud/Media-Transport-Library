#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Fails when an x86-64 executable or shared object under the given directories
# lacks a mark the Intel SDL compiler flags leave, see doc/build.md: full RELRO
# (-z relro -z now), a non-executable stack, PIE and CET IBT and SHSTK
# (-fcf-protection=full, except for libopenh264). The stack protector and
# _FORTIFY_SOURCE leave no reliable mark in a binary, so they are not checked.
# Every check needs a positive match, so a readelf that prints something else
# fails the file instead of passing it.

set -euo pipefail

(($#)) || {
	echo "usage: check-hardening.sh DIR..." >&2
	exit 2
}

checked=0
failed=0
while IFS= read -r -d '' file; do
	elf=$(readelf -W -h -l -d -n "$file" 2>/dev/null) || continue
	grep -q 'Machine: *Advanced Micro Devices X86-64' <<<"$elf" || continue
	grep -qE 'Type: *(DYN|EXEC) ' <<<"$elf" || continue
	checked=$((checked + 1))
	missing=()
	grep -q ' GNU_RELRO ' <<<"$elf" || missing+=(RELRO)
	grep -qE '\(FLAGS\) .*BIND_NOW|\(FLAGS_1\) .*Flags:.* NOW' <<<"$elf" || missing+=(BIND_NOW)
	grep -qE ' GNU_STACK .* RW +0x' <<<"$elf" || missing+=(NX)
	grep -qE 'Type: *DYN ' <<<"$elf" || missing+=(PIE)
	# The openh264 assembly has no CET mark, see doc/build.md
	if [[ ${file##*/} != libopenh264.so* ]]; then
		grep -q 'x86 feature: IBT, SHSTK' <<<"$elf" || missing+=(IBT/SHSTK)
	fi
	if ((${#missing[@]})); then
		echo "::error::${file} is not hardened: no ${missing[*]}" >&2
		failed=$((failed + 1))
	fi
done < <(find "$@" -type f -print0)

echo "hardening: ${checked} ELF files checked under $*, ${failed} failed"
((checked)) || {
	echo "::error::no x86-64 ELF file found under $*" >&2
	exit 1
}
((failed == 0))
