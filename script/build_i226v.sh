#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

DPDK_VER="25.11"
DPDK_SRC_DIR=""
BUILDTYPE="release"
FORCE_DPDK_RECLONE=false
SKIP_DPDK=false

usage() {
	cat <<USAGE
Usage: $(basename "$0") [OPTIONS]

Build Media Transport Library for Intel I226-V deployments.
This builds DPDK (with MTL patches, if present) and then runs ./build.sh.

Options:
  --dpdk-ver <version>        DPDK version tag to use (default: ${DPDK_VER})
  --dpdk-src-dir <path>       Existing/new DPDK source dir (default: ../dpdk-<version>)
  --buildtype <type>          build.sh type: debug|debugonly|debugoptimized|plain|release
                              (default: ${BUILDTYPE})
  --force-dpdk-reclone        Remove DPDK source directory and clone again
  --skip-dpdk                 Skip DPDK build/install and only run ./build.sh
  -h, --help                  Show this help
USAGE
}

run_as_root() {
	if [[ "${EUID}" -eq 0 ]]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	else
		echo "Need root privileges to run: $*" >&2
		exit 1
	fi
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--dpdk-ver)
		[[ $# -ge 2 ]] || { echo "--dpdk-ver requires a value" >&2; exit 1; }
		DPDK_VER="$2"
		shift 2
		;;
	--dpdk-src-dir)
		[[ $# -ge 2 ]] || { echo "--dpdk-src-dir requires a value" >&2; exit 1; }
		DPDK_SRC_DIR="$2"
		shift 2
		;;
	--buildtype)
		[[ $# -ge 2 ]] || { echo "--buildtype requires a value" >&2; exit 1; }
		BUILDTYPE="$2"
		shift 2
		;;
	--force-dpdk-reclone)
		FORCE_DPDK_RECLONE=true
		shift
		;;
	--skip-dpdk)
		SKIP_DPDK=true
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage
		exit 1
		;;
	esac
done

case "${BUILDTYPE}" in
	debug | debugonly | debugoptimized | plain | release) ;;
	*)
		echo "Invalid --buildtype '${BUILDTYPE}'" >&2
		exit 1
		;;
esac

if [[ -z "${DPDK_SRC_DIR}" ]]; then
	DPDK_SRC_DIR="${REPO_DIR}/../dpdk-${DPDK_VER}"
fi

if [[ "${SKIP_DPDK}" == "false" ]]; then
	if [[ "${FORCE_DPDK_RECLONE}" == "true" ]]; then
		rm -rf "${DPDK_SRC_DIR}"
	fi

	if [[ ! -d "${DPDK_SRC_DIR}" ]]; then
		git -C "$(dirname "${DPDK_SRC_DIR}")" clone https://github.com/DPDK/dpdk.git "$(basename "${DPDK_SRC_DIR}")"
	fi

	if [[ ! -d "${DPDK_SRC_DIR}/.git" ]]; then
		echo "DPDK source directory is not a git repository: ${DPDK_SRC_DIR}" >&2
		exit 1
	fi

	pushd "${DPDK_SRC_DIR}" >/dev/null
	git fetch --tags origin
	git checkout "v${DPDK_VER}"
	git reset --hard "v${DPDK_VER}"

	if compgen -G "${REPO_DIR}/patches/dpdk/${DPDK_VER}/*.patch" >/dev/null; then
		git am "${REPO_DIR}"/patches/dpdk/"${DPDK_VER}"/*.patch
	fi

	meson setup build --wipe
	ninja -C build
	run_as_root ninja install -C build
	popd >/dev/null
fi

pushd "${REPO_DIR}" >/dev/null
./build.sh "${BUILDTYPE}"
popd >/dev/null

echo "I226-V build flow complete."
