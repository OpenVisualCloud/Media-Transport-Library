#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

DPDK_VER="25.11"
BUILDTYPE="release"
FORCE_DPDK_RECLONE=false
SKIP_DPDK=false

usage() {
	cat <<USAGE
Usage: $(basename "$0") [OPTIONS]

Build Media Transport Library for Intel I226-V oriented deployments.
The script builds DPDK (with MTL patches) and then builds MTL via ./build.sh.

Options:
  --dpdk-ver <version>        DPDK version to use (default: ${DPDK_VER})
  --buildtype <type>          build.sh type: debug|debugonly|debugoptimized|plain|release
                              (default: ${BUILDTYPE})
  --force-dpdk-reclone        Remove local DPDK source folder and clone again
  --skip-dpdk                 Skip DPDK clone/build/install and only run ./build.sh
  -h, --help                  Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--dpdk-ver)
		DPDK_VER="$2"
		shift 2
		;;
	--buildtype)
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

if [[ "${SKIP_DPDK}" == "false" ]]; then
	DPDK_SRC_DIR="${REPO_DIR}/../dpdk-${DPDK_VER}"

	if [[ "${FORCE_DPDK_RECLONE}" == "true" ]]; then
		rm -rf "${DPDK_SRC_DIR}"
	fi

	if [[ ! -d "${DPDK_SRC_DIR}" ]]; then
		git -C "$(dirname "${DPDK_SRC_DIR}")" clone https://github.com/DPDK/dpdk.git "$(basename "${DPDK_SRC_DIR}")"
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
	sudo ninja install -C build
	popd >/dev/null
fi

pushd "${REPO_DIR}" >/dev/null
./build.sh "${BUILDTYPE}"
popd >/dev/null

echo "I226-V build flow complete."
