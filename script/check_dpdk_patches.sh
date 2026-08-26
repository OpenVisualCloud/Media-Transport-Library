#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

# GNU patch recovers a stale @@ header by searching for the hunk, applying it at
# an offset, and still exiting 0. So the exit code cannot detect a patch that has
# drifted from the tarball it was written against; the absence of an "offset" or
# "fuzz" line can.

set -e
# Without nullglob an empty patches/dpdk would be reported as a version named "*".
shopt -s nullglob

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
repo_root=$(cd "${script_folder}/.." && pwd)
patch_root="${repo_root}/patches/dpdk"
# shellcheck disable=SC1091 # versions.env is data, not shell input
. "${repo_root}/versions.env"

usage() {
	cat <<EOF
Usage: $script_name ARCHIVE_DIR

Verify each patches/dpdk/<version>/*.patch applies to its pinned upstream
tarball with no hunk offset and no fuzz. ARCHIVE_DIR holds the archives, named
v<version>.zip as script/build_dpdk.sh downloads them. A version with no archive
in ARCHIVE_DIR is skipped, except the version versions.env pins: the run fails
unless the pin contributed at least one verified patch, so that a skip can never
read as a pass. Prints nothing on stdout.

Only that flat series is covered, the same glob build_dpdk.sh applies. The
windows/ and hdr_split/ subdirectories are applied by other flows and are not
checked here, so drift in them goes unreported.
EOF
}

if [ $# -ne 1 ]; then
	usage >&2
	exit 1
fi

if ! archive_dir=$(readlink -qe "$1") || [ ! -d "${archive_dir}" ]; then
	echo "$script_name: no such directory: $1" >&2
	exit 1
fi

if [ ! -f "${archive_dir}/v${DPDK_VER}.zip" ]; then
	echo "$script_name: versions.env pins '${DPDK_VER}', but ${archive_dir}/v${DPDK_VER}.zip is missing" >&2
	exit 1
fi

work_dir=$(mktemp -d)
trap 'rm -rf "${work_dir}"' EXIT
# bash resumes after a trap handler, so an interrupt must exit or it misreports.
trap 'exit 130' INT TERM

pinned_checked=0
failed=0
skipped=""

for version_dir in "${patch_root}"/*/; do
	version=$(basename "${version_dir}")
	archive="${archive_dir}/v${version}.zip"
	if [ ! -f "${archive}" ]; then
		skipped="${skipped} ${version}"
		continue
	fi

	tree="${work_dir}/${version}"
	mkdir -p "${tree}"
	if ! unzip -q "${archive}" -d "${tree}"; then
		echo "FAIL ${archive}: not a readable zip archive" >&2
		failed=1
		rm -rf "${tree}"
		continue
	fi
	if [ ! -d "${tree}/dpdk-${version}" ]; then
		echo "FAIL ${archive}: holds no dpdk-${version} directory" >&2
		failed=1
		rm -rf "${tree}"
		continue
	fi

	for patch_file in "${version_dir}"*.patch; do
		if [ "${version}" = "${DPDK_VER}" ]; then
			pinned_checked=$((pinned_checked + 1))
		fi
		patch_name="${version}/$(basename "${patch_file}")"
		# Without --forward, --batch answers "Assume -R?" yes: silent pass, reversed tree.
		if ! output=$(patch --batch --forward -p1 -d "${tree}/dpdk-${version}" -i "${patch_file}" 2>&1); then
			echo "FAIL ${patch_name}: does not apply" >&2
			printf '%s\n' "${output}" >&2
			failed=1
			continue
		fi
		# Only a +1 offset prints "line" singular; the ^Hunk anchor keeps file paths out.
		drift=$(grep -E '^Hunk #[0-9]+ .*(\(offset -?[0-9]+ lines?\)|with fuzz [0-9]+)' <<<"${output}" || true)
		if [ -n "${drift}" ]; then
			echo "FAIL ${patch_name}: hunks do not land where the patch says" >&2
			printf '%s\n' "${drift}" >&2
			failed=1
		fi
	done

	# Each tree is ~129 MB, and every pinned version could be present at once.
	rm -rf "${tree}"
done

if [ -n "${skipped}" ]; then
	echo "$script_name: skipped, no archive in ${archive_dir}:${skipped}" >&2
fi

if [ "${pinned_checked}" -eq 0 ]; then
	echo "$script_name: versions.env pins '${DPDK_VER}', but no patch under ${patch_root}/${DPDK_VER} was verified" >&2
	exit 1
fi

[ "${failed}" -eq 0 ]
