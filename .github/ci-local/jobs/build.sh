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

# ── step: system: Check host prerequisites ──────────────────────────────────
# Mirrors `task ebpf:check-build`, which the build job runs first.
echo "::group::system: Check host prerequisites"
if ! bash "${WORKDIR}/script/build_ebpf_xdp.sh" --check build; then
	echo "::error::build job failed its host prerequisite check"
	exit 1
fi
echo "::endgroup::"

# ── step: Evaluate cache results ────────────────────────────────────────────
echo "::group::Evaluate cache results"
export CI_BUILD_DPDK="${CI_LOCAL_MISS_DPDK:-1}"
export CI_BUILD_MTL="${CI_LOCAL_MISS_MTL:-1}"
export CI_BUILD_JPEGXS="${CI_LOCAL_MISS_JPEGXS:-1}"
export CI_BUILD_FFMPEG="${CI_LOCAL_MISS_FFMPEG:-1}"
export CI_BUILD_GSTREAMER="${CI_LOCAL_MISS_GSTREAMER:-1}"
export CI_BUILD_PLUGINS="${CI_LOCAL_MISS_PLUGINS:-1}"
export CI_BUILD_ICE="${CI_LOCAL_MISS_ICE:-1}"

state() { [ "$1" = "1" ] && echo MISS || echo HIT; }
echo "::notice::DPDK=$(state "${CI_BUILD_DPDK}")" \
	"MTL=$(state "${CI_BUILD_MTL}")" \
	"JPEGXS=$(state "${CI_BUILD_JPEGXS}")" \
	"FFmpeg=$(state "${CI_BUILD_FFMPEG}")" \
	"GStreamer=$(state "${CI_BUILD_GSTREAMER}")" \
	"plugins=$(state "${CI_BUILD_PLUGINS}")" \
	"ICE=$(state "${CI_BUILD_ICE}")"

any_miss=0
for miss in "${CI_BUILD_DPDK}" "${CI_BUILD_MTL}" "${CI_BUILD_JPEGXS}" \
	"${CI_BUILD_FFMPEG}" "${CI_BUILD_GSTREAMER}" "${CI_BUILD_PLUGINS}" \
	"${CI_BUILD_ICE}"; do
	[ "${miss}" = "1" ] && any_miss=1
done
echo "::endgroup::"

# ── step: Setup environment and build ───────────────────────────────────────
# Environment copied verbatim from the workflow step of the same name.
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
		env | grep -E '^(CI_BUILD_|MTL_|SETUP_|ECOSYSTEM_|PLUGIN_|TOOLS_|CICD_|PKG_CONFIG|LD_LIBRARY)' | sort
		echo
		echo "=== pkg-config: what the build can see ==="
		pkg-config --list-all 2>/dev/null | grep -iE 'mtl|dpdk|jpeg' || echo "(nothing)"
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
	echo "  task ci:build-dependencies"
	exec bash -i
fi

if [ "${any_miss}" = "0" ]; then
	echo "::notice::every component cached, nothing to build"
	dump_diagnostics
	exit 0
fi

echo "::group::Setup environment and build"
rc=0
task ci:build-dependencies || rc=$?
if [ "$rc" -eq 0 ]; then
	task ci:validate-dependencies || rc=$?
fi
echo "::endgroup::"

dump_diagnostics

if [ "${rc}" -ne 0 ]; then
	echo "::error::build job failed with exit code ${rc}"
fi
exit "${rc}"
