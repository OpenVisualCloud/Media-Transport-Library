#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Tests check-hardening.sh with small programs that it builds: the marks it
# checks, its exemption table and its report. The hardening-scan job runs it
# before it scans the build.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../../../.."

failures=0
fail() {
	echo "::error file=$1::$2"
	failures=$((failures + 1))
}

# check-hardening.sh passes a hardened program, names each mark of an unhardened
# one and of one with a single CET mark, and fails a path without an ELF file.
test_check_hardening() {
	local gate=.github/scripts/ci/scan/check-hardening.sh work out rc=0
	work=$(mktemp -d)
	mkdir "${work}/good" "${work}/bad" "${work}/ibt" "${work}/shstk" "${work}/empty"
	printf '#include <string.h>\nint main(int c, char **v) { char b[64]; strcpy(b, v[0]); return b[c]; }\n' >"${work}/t.c"
	gcc -O2 -fstack-protector-strong -fcf-protection=full -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/good/t"
	gcc -O2 -fno-stack-protector -fcf-protection=none -no-pie -Wl,-z,norelro,-z,lazy,-z,execstack \
		"${work}/t.c" -o "${work}/bad/t" 2>/dev/null || fail "${gate}" "cannot link the unhardened program"
	gcc -O2 -fstack-protector-strong -fcf-protection=branch -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/ibt/t"
	gcc -O2 -fstack-protector-strong -fcf-protection=return -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/shstk/t"
	bash "${gate}" "${work}/good" >/dev/null 2>&1 || fail "${gate}" "fails a hardened program"
	out=$(bash "${gate}" "${work}/bad" 2>&1) || rc=$?
	if [ "${rc}" -eq 0 ] ||
		! grep -qxF "::error::${work}/bad/t is not hardened: no RELRO BIND_NOW NX PIE IBT SHSTK SSP" <<<"${out}"; then
		fail "${gate}" "does not name each mark an unhardened program lacks: ${out}"
	fi
	out=$(bash "${gate}" "${work}/ibt" 2>&1) || true
	grep -qxF "::error::${work}/ibt/t is not hardened: no SHSTK" <<<"${out}" ||
		fail "${gate}" "passes a program with IBT but no SHSTK: ${out}"
	out=$(bash "${gate}" "${work}/shstk" 2>&1) || true
	grep -qxF "::error::${work}/shstk/t is not hardened: no IBT" <<<"${out}" ||
		fail "${gate}" "passes a program with SHSTK but no IBT: ${out}"
	if bash "${gate}" "${work}/empty" >/dev/null 2>&1; then
		fail "${gate}" "passes a path without an ELF file"
	fi
	# Its table accepts SSP missing in a DPDK library without a protected
	# function, not in EAL, and both CET marks in libopenh264, not in libswscale.
	mkdir "${work}/dpdk" "${work}/ffmpeg" "${work}/broken"
	gcc -O2 -fno-stack-protector -fcf-protection=full -fPIC -shared -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/dpdk/librte_kvargs.so.26"
	cp "${work}/dpdk/librte_kvargs.so.26" "${work}/dpdk/librte_eal.so.26"
	out=$(bash "${gate}" "${work}/dpdk" 2>&1) || true
	if grep -qF "librte_kvargs.so.26 is not hardened" <<<"${out}" ||
		! grep -qxF "::error::${work}/dpdk/librte_eal.so.26 is not hardened: no SSP" <<<"${out}"; then
		fail "${gate}" "does not accept SSP missing in librte_kvargs only: ${out}"
	fi
	gcc -O2 -fstack-protector-strong -fcf-protection=none -fPIC -shared -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/ffmpeg/libopenh264.so.8"
	cp "${work}/ffmpeg/libopenh264.so.8" "${work}/ffmpeg/libswscale.so.8"
	out=$(bash "${gate}" "${work}/ffmpeg" 2>&1) || true
	if grep -qF "libopenh264.so.8 is not hardened" <<<"${out}" ||
		! grep -qxF "::error::${work}/ffmpeg/libswscale.so.8 is not hardened: no IBT SHSTK" <<<"${out}"; then
		fail "${gate}" "does not accept CET missing in libopenh264 only: ${out}"
	fi
	# The report: the totals, a row with the marks of each file, the failing
	# ones first, an ELF file that readelf can't read among them, a path without
	# an ELF file, and the same text on the summary page.
	printf '\177ELF' >"${work}/broken/libt.so"
	HARDENING_REPORT=${work}/report.md GITHUB_STEP_SUMMARY=${work}/summary.md \
		bash "${gate}" "${work}/good" "${work}/dpdk" "${work}/broken" "${work}/empty" >/dev/null 2>&1 || true
	out=$(grep '^| `' "${work}/report.md" 2>&1) || true
	if [ "${out}" != "| \`${work}/broken/libt.so\` | readelf can't read it | | | | | | |
| \`${work}/dpdk/librte_eal.so.26\` | yes | yes | yes | yes | yes | yes | no |
| \`${work}/dpdk/librte_kvargs.so.26\` | yes | yes | yes | yes | yes | yes | accepted |
| \`${work}/good/t\` | yes | yes | yes | yes | yes | yes | yes |" ] ||
		! grep -qxF "| 4 | 2 | \`${work}/good ${work}/dpdk ${work}/broken ${work}/empty\` |" "${work}/report.md" ||
		! grep -qxF "No x86-64 ELF file found under \`${work}/empty\`." "${work}/report.md" ||
		! cmp -s "${work}/report.md" "${work}/summary.md"; then
		fail "${gate}" "reports another table: $(cat "${work}/report.md" 2>&1)"
	fi
	rm -rf "${work:?}"
}

test_check_hardening

if [ "${failures}" -ne 0 ]; then
	echo "${failures} fault(s)." >&2
	exit 1
fi
echo "check-hardening.sh passes its tests."
