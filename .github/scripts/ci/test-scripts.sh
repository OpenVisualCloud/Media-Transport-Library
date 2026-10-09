#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
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
	grep -qx 'script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE\[0\]}")" \&\& pwd)"' "$file" ||
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
	git ls-files 'script/*.sh' | grep -vx 'script/common.sh'
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
	bash -c '. script/build_jpegxs.sh; register_plugin "$1" "$2" >/dev/null' \
		_ "${config}" /opt/jpegxs/lib/libst_plugin_st22_svt_jpeg_xs.so ||
		fail script/build_jpegxs.sh "cannot update the plugin registry"
	python3 - "${config}" <<'PY' || fail script/build_jpegxs.sh "plugin registry update is incorrect"
import json
import sys

plugins = [item for item in json.load(open(sys.argv[1]))["plugins"] if item["name"] == "st22_svt_jpegxs"]
assert sum(item["enabled"] for item in plugins) == 1
assert next(item for item in plugins if item["enabled"])["path"] == "/opt/jpegxs/lib/libst_plugin_st22_svt_jpeg_xs.so"
PY
	[ "$(stat -c '%a' "${config}")" = 644 ] || fail script/build_jpegxs.sh "plugin registry mode changed"

	mkdir "${source_dir}"
	touch "${source_dir}/sentinel"
	if bash script/build_jpegxs.sh --source-dir "${source_dir}" >"${work}/source.log" 2>&1; then
		fail script/build_jpegxs.sh "accepted a source directory without CMakeLists.txt"
	fi
	grep -q 'Source directory does not contain CMakeLists.txt' "${work}/source.log" ||
		fail script/build_jpegxs.sh "did not validate the explicit source directory"
	[ -f "${source_dir}/sentinel" ] || fail script/build_jpegxs.sh "deleted the explicit source directory"
	if bash script/build_jpegxs.sh --ci --version test-revision >"${work}/version.log" 2>&1; then
		fail script/build_jpegxs.sh "accepted --version with --ci"
	fi
	grep -q -- '--version cannot be used with --ci' "${work}/version.log" ||
		fail script/build_jpegxs.sh "did not explain the CI version constraint"
	rm -rf "${work:?}"
}

test_jpegxs_helpers

bash script/check_dpdk_patches.sh || fail script/check_dpdk_patches.sh "the DPDK patches do not apply clean"
bash script/build_ebpf_xdp.sh --check build || echo "build_ebpf_xdp.sh --check build: the host does not have each package"

if [ "$failures" -ne 0 ]; then
	echo "${failures} fault(s)." >&2
	exit 1
fi
echo "All scripts have the common structure."
