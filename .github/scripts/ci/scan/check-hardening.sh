#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Fails when an x86-64 executable or shared object under the given paths lacks
# a mark that the compiler hardening of doc/getting_started/build.md leaves: full RELRO
# (-z relro -z now), a non-executable stack, PIE, CET IBT and SHSTK
# (-fcf-protection=full in each object linked in), and the stack protector, as
# an import of __stack_chk_fail, which needs one protected function in the file.
# _FORTIFY_SOURCE leaves no reliable mark: a library without a fortified call
# imports no __*_chk function. meson builds its configure checks (meson-private)
# without the project flags, so they are skipped. Every check needs a positive
# match, so a readelf that prints something else fails the file instead of
# passing it, an ELF file that readelf can't read fails, and so does a path
# without an x86-64 ELF file.

set -euo pipefail

(($#)) || {
	echo "usage: check-hardening.sh PATH..." >&2
	exit 2
}

checked=0
failed=0
for path; do
	before=$checked
	while IFS= read -r -d '' file; do
		[[ $(head -c 4 "$file" | tr -d '\0') == $'\177ELF' ]] || continue
		if ! elf=$(readelf -W -h -l -d -n --dyn-syms "$file" 2>&1); then
			echo "::error::readelf cannot read ${file}: ${elf}" >&2
			checked=$((checked + 1))
			failed=$((failed + 1))
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
		grep -q 'x86 feature: IBT, SHSTK' <<<"$elf" || missing+=(IBT/SHSTK)
		grep -q ' UND __stack_chk_fail@' <<<"$elf" || missing+=(SSP)
		if ((${#missing[@]})); then
			echo "::error::${file} is not hardened: no ${missing[*]}" >&2
			failed=$((failed + 1))
		fi
	done < <(find "$path" -name meson-private -prune -o -type f -print0)
	((checked > before)) || {
		echo "::error::no x86-64 ELF file found under $path" >&2
		exit 1
	}
done

echo "hardening: ${checked} ELF files checked under $*, ${failed} failed"
((failed == 0))
