#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The .github/actions/validate-host composite action, as executed inside the
# simulated bare-metal runner. Each section below is one step of that action,
# in order. smoke-tests, gtest-bare-metal and the pytest workflows all begin
# by running it, and when it is wrong every one of them fails identically and
# unhelpfully -- so it is worth being able to run on its own.
#
# Inputs come from .github/ci-local/run-job.sh:
#
#   CI_LOCAL_WORKDIR        where the checkout is mounted
#   CI_LOCAL_OUT            directory collected back to the host afterwards
#   CI_LOCAL_MISS_<COMP>    1 when the cache did not supply the component
#   CI_LOCAL_NIC            the matrix NIC being simulated
#   PCI_DEVICE              the PCI IDs that NIC maps to
#
# Two steps of the real action cannot run in a container and are reported as
# skipped rather than faked: the ICE driver alignment, which would replace a
# module in the host kernel, and the shadow-host rsync, which needs a second
# machine.

set -uo pipefail

WORKDIR="${CI_LOCAL_WORKDIR:-/github/workspace}"
OUT_DIR="${CI_LOCAL_OUT:-/github/out}"
NIC="${CI_LOCAL_NIC:-}"
LOCAL_INSTALL="${WORKDIR}/.local_install"
ENV_FILE="${OUT_DIR}/validate-host.env"
PATH_FILE="${OUT_DIR}/validate-host.path"
export GITHUB_ENV="$ENV_FILE"
export GITHUB_PATH="$PATH_FILE"

cd "${WORKDIR}" || exit 1
mkdir -p "${OUT_DIR}"
: >"${ENV_FILE}"
: >"${PATH_FILE}"

rc=0
failed_step=""
step_failed() {
	rc=1
	failed_step="${failed_step}${failed_step:+, }$1"
	echo "::error::$1"
}

# ── step: system: Check eBPF/XDP prerequisites ──────────────────────────────
# The action runs this first and strict, so a host that cannot serve AF_XDP
# fails before any setup. Non-strict here: a container has no say over the
# kernel it was given, so enforcing kernel CONFIG_* would fail every local run.
echo "::group::system: Check eBPF/XDP prerequisites"
bash "${WORKDIR}/script/build_ebpf_xdp.sh" --check --mode all
echo "::endgroup::"

# ── step: cache: Restore dependency artifacts ───────────────────────────────
# The action restores each with fail-on-cache-miss: true, except plugins,
# whose absence only disables the codec tests. A miss here is the local
# equivalent of that hard failure -- it means the build job never published
# an artifact for these sources.
echo "::group::cache: Restore build artifacts"
for comp in dpdk mtl jpegxs ffmpeg gstreamer plugins ice; do
	upper="${comp^^}"
	miss_var="CI_LOCAL_MISS_${upper}"
	if [ "${!miss_var:-1}" = "1" ]; then
		step_failed "cache: ${comp} missing -- run the build job first (fail-on-cache-miss: true)"
	else
		echo "  restored ${comp}"
	fi
done
echo "::endgroup::"

if [ "${rc}" -ne 0 ]; then
	echo "::error::validate-host cannot continue without the build artifacts"
	echo "failed steps: ${failed_step}"
	exit 1
fi

# ── step: Make artifacts executable ─────────────────────────────────────────
task ci:validate-dependencies || step_failed "cache: structural validation failed"
task ci:configure-host -- make-executable || step_failed "cache: executable alignment failed"

# ── step: kahawai: Generate CI plugin registry from cache ───────────────────
echo "::group::kahawai: Generate CI plugin registry from cache"
RUNNER_TEMP="$OUT_DIR" task ci:configure-host -- registry || step_failed "kahawai: registry generation failed"
echo "::endgroup::"

# ── step: activation: Model ICE ordering without host mutation ──────────────
echo "::group::activation: ICE dry run"
ICE_ACTIVATION_STAMP="${OUT_DIR}/ice.state" \
	ICE_COMMAND_LOG="${OUT_DIR}/ice-activation.log" \
	task ci:activate-ice -- --dry-run || step_failed "activation: ICE dry run failed"
echo "::endgroup::"

# ── step: Configure LD_LIBRARY_PATH, PATH, GST_PLUGIN_PATH ──────────────────
# Written to a file, which is what $GITHUB_ENV and $GITHUB_PATH are.
echo "::group::Configure environment"
LI="${LOCAL_INSTALL}"
task ci:configure-host -- environment || step_failed "environment: configuration failed"
# shellcheck disable=SC1090 # generated above
set -a && . "${ENV_FILE}" && set +a
PATH="$(paste -sd: "$PATH_FILE"):${PATH}"
export PATH
echo "::notice::Host environment configured from validated caches"
echo "::endgroup::"

# ── verification ────────────────────────────────────────────────────────────
# Not a step of the action: the action configures an environment and never
# checks that it works. Every consumer then fails on its own symptom. These
# are the resolutions the test jobs depend on, asserted once, here.
echo "::group::verify: the environment the tests will inherit"

check() {
	# check <required:0|1> <label> <command...>
	local required="$1" label="$2"
	shift 2
	if "$@" >/dev/null 2>&1; then
		echo "  ok       ${label}"
	elif [ "${required}" -eq 1 ]; then
		step_failed "verify: ${label}"
	else
		echo "::warning::verify: ${label} (optional)"
	fi
}

check 1 "pkg-config finds mtl >= 22.12.0" pkg-config --exists 'mtl >= 22.12.0'
check 1 "pkg-config finds libdpdk" pkg-config --exists libdpdk
check 1 "pkg-config finds SvtJpegxs" pkg-config --exists SvtJpegxs
check 1 "RxTxApp is present and executable" test -x "${LI}/mtl/bin/RxTxApp"
check 0 "ffmpeg carries the mtl device" bash -c "'${LI}/ffmpeg/bin/ffmpeg' -hide_banner -devices 2>/dev/null | grep -q mtl"
check 0 "gstreamer plugin is loadable" bash -c "GST_PLUGIN_PATH='${LI}/gstreamer/gstreamer-1.0' gst-inspect-1.0 mtl_st20p_tx"
check 1 "st22 JPEG XS plugin present" bash -c "find '${LI}/jpegxs' -name libst_plugin_st22_svt_jpeg_xs.so -type f -print -quit | grep -q ."
check 1 "st22 avcodec plugin present" bash -c "find '${LI}/plugins' -name libst_plugin_st22_avcodec.so -type f -print -quit | grep -q ."

# RxTxApp links against DPDK and MTL; an unresolved symbol here is the failure
# the test jobs would otherwise hit minutes later, with a NIC in the way.
if [ -x "${LI}/mtl/bin/RxTxApp" ]; then
	if ldd "${LI}/mtl/bin/RxTxApp" 2>/dev/null | grep -q 'not found'; then
		ldd "${LI}/mtl/bin/RxTxApp" 2>/dev/null | grep 'not found' | sed 's/^/  /'
		step_failed "verify: RxTxApp has unresolved shared libraries"
	else
		echo "  ok       RxTxApp resolves all shared libraries"
	fi
fi
echo "::endgroup::"

# ── what this simulation cannot do ──────────────────────────────────────────
echo "::group::not simulated"
cat <<EOF
The steps below need the real host and are skipped here. Everything the test
jobs do *before* touching hardware has been exercised above.

  nic ${NIC:-<none>}${PCI_DEVICE:+, pci ${PCI_DEVICE}}
  - PF/VF binding and PCI passthrough  (no devices in the container)
	- Kahawai ICE driver mutation        (ordering validated via dry run)
  - hugepages, PTP, MtlManager         (host-wide state)
EOF
echo "::endgroup::"

{
	echo "when: $(date -u +%FT%TZ)"
	echo "nic: ${NIC:-<none>}"
	echo "pci_device: ${PCI_DEVICE:-<none>}"
	echo
	echo "== environment handed to the tests =="
	cat "${ENV_FILE}"
	echo
	echo "== .local_install =="
	find "${LOCAL_INSTALL}" -maxdepth 2 -mindepth 1 2>/dev/null | sort
} >"${OUT_DIR}/validate-host-diagnostics.txt"

if [ "${rc}" -ne 0 ]; then
	echo "::error::validate-host failed: ${failed_step}"
fi
exit "${rc}"
