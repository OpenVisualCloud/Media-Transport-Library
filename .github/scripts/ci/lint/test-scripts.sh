#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../../../.."
root_dir=$PWD

failures=0
fail() {
	echo "::error file=$1::$2"
	failures=$((failures + 1))
}

# Print "<line number>:<text>" of each line of $1 that is not a comment or in a here-document.
code_lines() {
	awk '
		heredoc != "" { if ($0 ~ "^\t*" heredoc "$") heredoc = ""; next }
		/^[[:space:]]*#/ { next }
		{
			print NR ":" $0
			if (match($0, /<<-?[[:space:]]*["\047]?[A-Za-z_]+/)) {
				heredoc = substr($0, RSTART, RLENGTH)
				sub(/^<<-?[[:space:]]*["\047]?/, "", heredoc)
			}
		}
	' "$1"
}

# shellcheck disable=SC2016
check_static() {
	local file="$1" name line out rc=0
	name=$(basename "$file")

	[ "$(head -n 1 "$file")" = "#!/bin/bash" ] || fail "$file" "the first line is not #!/bin/bash"
	grep -q '^# SPDX-License-Identifier: BSD-3-Clause$' "$file" || fail "$file" "no SPDX line"
	grep -qx 'set -euo pipefail' "$file" || fail "$file" "no 'set -euo pipefail' line"
	grep -qx 'script_name="$(basename "${BASH_SOURCE\[0\]}")"' "$file" || fail "$file" "no script_name line"
	# An installed script reached through a link (nicctl.sh) resolves it first.
	grep -qx -e 'script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE\[0\]}")" \&\& pwd)"' \
		-e 'script_folder="$(dirname -- "$(readlink -f -- "${BASH_SOURCE\[0\]}")")"' "$file" ||
		fail "$file" "no script_folder line"
	grep -qE '^\. "\$\{(script|root)_folder\}/(script/)?common\.sh"$' "$file" || fail "$file" "does not source common.sh"
	grep -q '^show_help() {$' "$file" || fail "$file" "no show_help()"
	grep -q '^main() {$' "$file" || fail "$file" "no main()"
	grep -qx '(return 0 2>/dev/null) && sourced=1 || sourced=0' "$file" || fail "$file" "no sourced guard"
	if [ "$(tail -n 1 "$file")" != "fi" ] || [ "$(tail -n 2 "$file" | head -n 1)" != '	main "$@"' ]; then
		fail "$file" "does not end with the main call of the sourced guard"
	fi

	# A quoted message that tells the user to use sudo is not a sudo command.
	while IFS=: read -r line _; do
		fail "$file" "line ${line}: raw sudo, use as_root"
	done < <(code_lines "$file" | grep -E '^[0-9]+:([^#"'\'']*[;&|(`[:space:]])?sudo\s')
	while IFS=: read -r line _; do
		fail "$file" "line ${line}: message to stderr, use log_*"
	done < <(code_lines "$file" | grep -E '^[0-9]+:\s*(echo|printf)\b.*>&2')
	while IFS=: read -r line _; do
		fail "$file" "line ${line}: local copy of a common.sh helper"
	done < <(grep -nE '^(function\s+)?(run_as_root|as_root|log_info|log_error|log_warning|command_exists)\s*\(\)' "$file")

	out=$(bash "$file" -h 2>/dev/null) || rc=$?
	[ "$rc" -eq 0 ] || fail "$file" "-h exits ${rc}"
	[[ "$(head -n 1 <<<"$out")" == "Usage: ${name}"* ]] || fail "$file" "-h does not print 'Usage: ${name}'"

	rc=0
	bash "$file" --no-such-option >/dev/null 2>&1 </dev/null || rc=$?
	[ "$rc" -ne 0 ] || fail "$file" "an unknown option exits 0"

	out=$(bash -c '. "$1" && echo sourced-ok' _ "$file" 2>&1) || true
	[ "$out" = "sourced-ok" ] || fail "$file" "sourcing the script prints or exits: ${out:0:200}"
}

while read -r file; do
	echo "--- ${file}"
	check_static "$file"
done < <(
	find script -maxdepth 1 -type f -name '*.sh' ! -name common.sh | sort
	echo .github/scripts/setup_environment.sh
)

out=$(mktemp -d)
bash script/hash_sources.sh -o "${out}/root"
(cd /tmp && bash "${root_dir}/script/hash_sources.sh" -o "${out}/tmp")
grep -q '=' "${out}/root" || fail script/hash_sources.sh "-o writes no key=value line"
cmp -s "${out}/root" "${out}/tmp" || fail script/hash_sources.sh "the hashes change with the current directory"
# The cache key computed before a build must equal the hash after it writes untracked files.
mkdir -p tests/tools/RxTxApp/build_hash_test
echo untracked >tests/tools/RxTxApp/build_hash_test/output.o
bash script/hash_sources.sh -o "${out}/build"
rm -rf tests/tools/RxTxApp/build_hash_test
cmp -s "${out}/root" "${out}/build" || fail script/hash_sources.sh "an untracked build output changes the hashes"
rm -rf "${out:?}"

test_jpegxs_helpers() {
	local work config source_dir
	work=$(mktemp -d)
	config="${work}/kahawai.json"
	source_dir="${work}/source"
	cp kahawai.json "${config}"
	chmod 0644 "${config}"
	bash -c '. script/common.sh; register_plugin "$1" st22_svt_jpegxs "$2" >/dev/null' \
		_ "${config}" /opt/jpegxs/lib/libst_plugin_st22_svt_jpeg_xs.so ||
		fail script/common.sh "cannot update the plugin registry"
	python3 - "${config}" <<'PY' || fail script/common.sh "plugin registry update is incorrect"
import json
import sys

plugins = [item for item in json.load(open(sys.argv[1]))["plugins"] if item["name"] == "st22_svt_jpegxs"]
assert sum(item["enabled"] for item in plugins) == 1
assert next(item for item in plugins if item["enabled"])["path"] == "/opt/jpegxs/lib/libst_plugin_st22_svt_jpeg_xs.so"
PY
	[ "$(stat -c '%a' "${config}")" = 644 ] || fail script/common.sh "plugin registry mode changed"
	# build.sh creates the registry of a local install.
	bash -c '. script/common.sh; register_plugin "$1" st22_sample "$2" >/dev/null' \
		_ "${work}/conf/kahawai.json" /opt/mtl/lib/libst_plugin_st22_sample.so ||
		fail script/common.sh "cannot create the plugin registry"
	python3 - "${work}/conf/kahawai.json" <<'PY' || fail script/common.sh "created plugin registry is incorrect"
import json
import sys

assert json.load(open(sys.argv[1])) == {
    "plugins": [{"enabled": 1, "name": "st22_sample", "path": "/opt/mtl/lib/libst_plugin_st22_sample.so"}]
}
PY

	mkdir "${source_dir}"
	touch "${source_dir}/sentinel"
	if bash script/build_jpegxs.sh --source-dir "${source_dir}" >"${work}/source.log" 2>&1; then
		fail script/build_jpegxs.sh "accepted a source directory without CMakeLists.txt"
	fi
	grep -q 'Source directory does not contain CMakeLists.txt' "${work}/source.log" ||
		fail script/build_jpegxs.sh "did not validate the explicit source directory"
	[ -f "${source_dir}/sentinel" ] || fail script/build_jpegxs.sh "deleted the explicit source directory"
	rm -rf "${work:?}"
}

test_jpegxs_helpers

# The ICE version rule that build_drivers.sh, mtl_host_common.sh and
# validate-ice.sh share: "<driver version> <ICE version> <accepted: yes or no>".
test_ice_version_rule() {
	local driver ver want got refusal
	while read -r driver ver want; do
		got=no
		if bash -c '. script/common.sh; ice_driver_version_ok "$1" "$2"' _ "${driver}" "${ver}"; then
			got=yes
		fi
		[ "${got}" = "${want}" ] ||
			fail script/common.sh "ice_driver_version_ok '${driver}' '${ver}' is ${got}, not ${want}"
	done <<'CASES'
2.7.6 2.7.6 yes
Kahawai_2.7.6 2.7.6 no
2.10.1 2.10.1 yes
Kahawai_2.6.7 2.6.7 yes
Kahawai_1.13.7_20240220 1.13.7 yes
2.6.7 2.6.7 no
Kahawai_2.6.6 2.6.7 no
Kahawai_2.6.8 2.6.8 yes
2.6.8 2.6.8 no
CASES
	# The message, not any failure: a host without kernel headers fails as well.
	refusal=$(bash script/build_drivers.sh --driver ice --build-only --ice-version 2.6.8 2>&1 || true)
	grep -q 'No ICE patches for version 2.6.8' <<<"${refusal}" ||
		fail script/build_drivers.sh "did not refuse ICE 2.6.8, which is before 2.7.6 and has no patch folder"
}

test_ice_version_rule

# Prints the flags that hardening_flags of common.sh sets for CC $1, CFLAGS $2,
# CPPFLAGS $3 and MTL_HARDENING $4 (default: 1, whatever the caller exports).
dependency_flags() {
	CC=$1 CFLAGS=$2 CPPFLAGS=$3 MTL_HARDENING=${4-1} bash -c '. script/common.sh && hardening_flags && echo "${HARDENING_CFLAGS}|${HARDENING_LDFLAGS}"' 2>&1
}

# The stand-in compiler takes each flag and applies level 3, or 2 with -DMTL_GCC11.
test_dependency_hardening_flags() {
	local work out cc max value
	local compile='-fstack-protector-strong -fstack-clash-protection -fcf-protection=full'
	local link='-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack'
	work=$(mktemp -d)
	printf '#!/bin/sh\ncase " $* " in *" -DMTL_GCC11 "*) echo "#define __USE_FORTIFY_LEVEL 2" ;; *) echo "#define __USE_FORTIFY_LEVEL 3" ;; esac\n' >"${work}/cc"
	chmod +x "${work}/cc"
	out=$(dependency_flags "${work}/cc -m64" "" "") || true
	[ "${out}" = "${compile} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3|${link}" ] ||
		fail script/common.sh "hardening_flags with a CC of two words: ${out}"
	out=$(dependency_flags "${work}/cc" "-DMTL_GCC11" "") || true
	[ "${out}" = "${compile} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2|${link}" ] ||
		fail script/common.sh "hardening_flags takes level 3 where the compiler applies 2: ${out}"
	out=$(dependency_flags "${work}/cc" "-O2 -D_FORTIFY_SOURCE=2" "") || true
	[ "${out}" = "${compile}|${link}" ] || fail script/common.sh "hardening_flags adds a level to the one of CFLAGS: ${out}"
	out=$(dependency_flags "${work}/cc" "-O2" "-Wp,-D_FORTIFY_SOURCE=3") || true
	[ "${out}" = "${compile}|${link}" ] || fail script/common.sh "hardening_flags adds a level to the one of CPPFLAGS: ${out}"
	out=$(dependency_flags "${work}/cc" "-O2 -g -O0" "") || true
	[ "${out}" = "${compile}|${link}" ] || fail script/common.sh "hardening_flags adds a level to an -O0 build: ${out}"
	out=$(dependency_flags "${work}/cc" "-O1 -fsanitize=address" "") || true
	[ "${out}" = "${compile}|${link}" ] || fail script/common.sh "hardening_flags adds a level to a sanitizer build: ${out}"
	out=$(dependency_flags "${work}/cc" "-O0" "-Os") || true
	[ "${out}" = "${compile} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3|${link}" ] ||
		fail script/common.sh "hardening_flags takes no -O of CPPFLAGS after CFLAGS: ${out}"
	if dependency_flags "${work}/missing" "" "" >/dev/null; then
		fail script/common.sh "hardening_flags passes without a compiler"
	fi
	out=$(dependency_flags "${work}/cc" "" "" 0) || true
	if [ "${out##*$'\n'}" != "|" ] || [[ ${out} != *"MTL_HARDENING=0: the dependency is built without the hardening flags"* ]]; then
		fail script/common.sh "hardening_flags sets flags, or doesn't warn, with MTL_HARDENING=0: ${out}"
	fi
	for value in 1 ""; do
		out=$(dependency_flags "${work}/cc" "" "" "${value}") || true
		[ "${out}" = "${compile} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3|${link}" ] ||
			fail script/common.sh "hardening_flags with MTL_HARDENING='${value}': ${out}"
	done
	if dependency_flags "${work}/cc" "" "" 2 >/dev/null; then
		fail script/common.sh "hardening_flags takes MTL_HARDENING=2"
	fi
	rm -rf "${work:?}"
	# A real compiler gets the highest level that it applies.
	for cc in gcc clang; do
		command -v "${cc}" >/dev/null || continue
		max=$("${cc}" -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -include string.h -dM -E -x c /dev/null |
			sed -n 's/^#define __USE_FORTIFY_LEVEL //p')
		out=$(dependency_flags "${cc}" "" "") || true
		[[ ${out} == *" -D_FORTIFY_SOURCE=${max}|"* ]] || fail script/common.sh "hardening_flags gives ${cc} another level than ${max}: ${out}"
	done
}

test_dependency_hardening_flags

# check-hardening.sh passes a hardened program, names each mark of an unhardened
# one and of one with IBT only, and fails a path that holds no ELF file.
test_check_hardening() {
	local gate=.github/scripts/ci/scan/check-hardening.sh work out rc=0
	work=$(mktemp -d)
	mkdir "${work}/good" "${work}/bad" "${work}/ibt" "${work}/empty"
	printf '#include <string.h>\nint main(int c, char **v) { char b[64]; strcpy(b, v[0]); return b[c]; }\n' >"${work}/t.c"
	gcc -O2 -fstack-protector-strong -fcf-protection=full -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/good/t"
	gcc -O2 -fno-stack-protector -fcf-protection=none -no-pie -Wl,-z,norelro,-z,lazy,-z,execstack \
		"${work}/t.c" -o "${work}/bad/t" 2>/dev/null || fail "${gate}" "cannot link the unhardened program"
	gcc -O2 -fstack-protector-strong -fcf-protection=branch -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack \
		"${work}/t.c" -o "${work}/ibt/t"
	bash "${gate}" "${work}/good" >/dev/null 2>&1 || fail "${gate}" "fails a hardened program"
	out=$(bash "${gate}" "${work}/bad" 2>&1) || rc=$?
	if [ "${rc}" -eq 0 ] ||
		! grep -qxF "::error::${work}/bad/t is not hardened: no RELRO BIND_NOW NX PIE IBT/SHSTK SSP" <<<"${out}"; then
		fail "${gate}" "does not name each mark an unhardened program lacks: ${out}"
	fi
	out=$(bash "${gate}" "${work}/ibt" 2>&1) || true
	grep -qxF "::error::${work}/ibt/t is not hardened: no IBT/SHSTK" <<<"${out}" ||
		fail "${gate}" "passes a program with IBT but no SHSTK: ${out}"
	if bash "${gate}" "${work}/empty" >/dev/null 2>&1; then
		fail "${gate}" "passes a path without an ELF file"
	fi
	rm -rf "${work:?}"
}

test_check_hardening

bash script/check_dpdk_patches.sh || fail script/check_dpdk_patches.sh "the DPDK patches do not apply clean"
bash script/build_ebpf_xdp.sh --check build || echo "build_ebpf_xdp.sh --check build: the host does not have each package"

if [ "$failures" -ne 0 ]; then
	echo "${failures} fault(s)." >&2
	exit 1
fi
echo "All scripts have the common structure."
