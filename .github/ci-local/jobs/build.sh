#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The `build` job of .github/workflows/build.yml, as executed inside the
# simulated runner. Each section below is one step of that job, in order.
#
# Inputs come from .github/ci-local/run-job.sh, which has already played the
# part of the runner and of actions/cache:
#
#   CI_LOCAL_WORKDIR        where the checkout is mounted
#   CI_LOCAL_OUT            directory collected back to the host afterwards
#   CI_LOCAL_MISS_<COMP>    1 when the component must be rebuilt
#   CI_LOCAL_HASH_<COMP>    the cache key the workflow would have used
#   CI_LOCAL_SHELL          1 to stop before the build and hand over a shell

set -uo pipefail

WORKDIR="${CI_LOCAL_WORKDIR:-/github/workspace}"
OUT_DIR="${CI_LOCAL_OUT:-/github/out}"
LOCAL_INSTALL="${WORKDIR}/.local_install"
DIAG_FILE="${OUT_DIR}/diagnostics.txt"

cd "${WORKDIR}" || exit 1

# ── step: system: Check eBPF/XDP prerequisites ──────────────────────────────
# Mirrors .github/actions/check-ebpf, which the build job runs first.
echo "::group::system: Check eBPF/XDP prerequisites"
if ! bash "${WORKDIR}/script/build_ebpf_xdp.sh" --check --mode build --strict; then
	echo "::error::build job failed its eBPF/XDP prerequisite check"
	exit 1
fi
echo "::endgroup::"

# ── step: Evaluate cache results ────────────────────────────────────────────
echo "::group::Evaluate cache results"
export SETUP_BUILD_AND_INSTALL_DPDK="${CI_LOCAL_MISS_DPDK:-1}"
export MTL_BUILD_AND_INSTALL="${CI_LOCAL_MISS_MTL:-1}"
export ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN="${CI_LOCAL_MISS_FFMPEG:-1}"
export ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN="${CI_LOCAL_MISS_GSTREAMER:-1}"
export PLUGIN_BUILD_AND_INSTALL_AVCODEC="${CI_LOCAL_MISS_PLUGINS:-1}"

state() { [ "$1" = "1" ] && echo MISS || echo HIT; }
echo "::notice::DPDK=$(state "${SETUP_BUILD_AND_INSTALL_DPDK}")" \
	"MTL=$(state "${MTL_BUILD_AND_INSTALL}")" \
	"FFmpeg=$(state "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}")" \
	"GStreamer=$(state "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}")" \
	"plugins=$(state "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}")"

any_miss=0
for miss in "${SETUP_BUILD_AND_INSTALL_DPDK}" "${MTL_BUILD_AND_INSTALL}" \
	"${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" \
	"${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" \
	"${PLUGIN_BUILD_AND_INSTALL_AVCODEC}"; do
	[ "${miss}" = "1" ] && any_miss=1
done
echo "::endgroup::"

# ── step: Setup environment and build ───────────────────────────────────────
# Environment copied verbatim from the workflow step of the same name.
export MTL_INSTALL_PREFIX="${LOCAL_INSTALL}/mtl"
export PKG_CONFIG_PATH="${LOCAL_INSTALL}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${LOCAL_INSTALL}/mtl/lib/x86_64-linux-gnu/pkgconfig"
export LD_LIBRARY_PATH="${LOCAL_INSTALL}/dpdk/lib/x86_64-linux-gnu:${LOCAL_INSTALL}/mtl/lib/x86_64-linux-gnu"
export TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET=1
export PLUGIN_BUILD_AND_INSTALL_JPEGXS=1
# Capture PHC is disciplined to TAI at runtime via phc2sys -O <live_offset>,
# so the kernel TAI offset is left untouched.
export TOOLS_RUN_SET_TAI_OFFSET=0
# The workflow's runner is long-lived and already provisioned; a fresh
# container is not, so it takes the same path CI takes on a clean machine.
export SETUP_ENVIRONMENT=1
export CICD_BUILD=1

dump_diagnostics() {
	mkdir -p "${OUT_DIR}"
	{
		echo "=== when ==="
		date -u +%Y-%m-%dT%H:%M:%SZ
		echo
		echo "=== os ==="
		uname -a
		# shellcheck disable=SC1091
		(. /etc/os-release && echo "${PRETTY_NAME}")
		echo
		echo "=== build environment ==="
		env | grep -E '^(MTL_|SETUP_|ECOSYSTEM_|PLUGIN_|TOOLS_|CICD_|PKG_CONFIG|LD_LIBRARY)' | sort
		echo
		echo "=== pkg-config: what the build can see ==="
		pkg-config --list-all 2>/dev/null | grep -iE 'mtl|dpdk' || echo "(nothing)"
		echo
		echo "=== pkg-config: the check that fails in CI ==="
		pkg-config --print-errors --exists 'mtl >= 22.12.0' 2>&1 &&
			echo "mtl >= 22.12.0 OK" || echo "mtl >= 22.12.0 NOT FOUND"
		echo
		echo "=== .pc files under .local_install ==="
		find "${LOCAL_INSTALL}" -name '*.pc' 2>/dev/null || echo "(none)"
		echo
		echo "=== .local_install tree (depth 3) ==="
		find "${LOCAL_INSTALL}" -maxdepth 3 2>/dev/null || echo "(missing)"
		echo
		echo "=== cache stamps ==="
		for f in "${LOCAL_INSTALL}"/.stamps/*; do
			[ -e "$f" ] && echo "$(basename "$f")=$(cat "$f")"
		done
		echo
		echo "=== disk ==="
		df -h "${WORKDIR}" 2>/dev/null
	} >"${DIAG_FILE}" 2>&1
	echo "diagnostics written to ${DIAG_FILE}"
}

if [ "${CI_LOCAL_SHELL:-0}" = "1" ]; then
	echo "Runner ready. The build environment is exported; run:"
	echo "  bash .github/scripts/setup_environment.sh"
	exec bash -i
fi

if [ "${any_miss}" = "0" ]; then
	echo "::notice::every component cached, nothing to build"
	dump_diagnostics
	exit 0
fi

echo "::group::Setup environment and build"
rc=0
bash "${WORKDIR}/.github/scripts/setup_environment.sh" || rc=$?
echo "::endgroup::"

dump_diagnostics

if [ "${rc}" -ne 0 ]; then
	echo "::error::build job failed with exit code ${rc}"
fi
exit "${rc}"
