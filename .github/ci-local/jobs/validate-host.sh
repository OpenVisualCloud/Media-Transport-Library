#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The .github/actions/validate-host composite action, as executed by the local
# runner. Each section below is one step of that action, in order. smoke-tests,
# gtest-bare-metal and the pytest workflows all begin by running it, and when it
# is wrong every one of them fails identically and unhelpfully -- so it is worth
# being able to run on its own.
#
# Inputs come from .github/ci-local/run-job.sh:
#
#   CI_LOCAL_WORKDIR        where the checkout is
#   CI_LOCAL_OUT            directory collected back to the host afterwards
#   CI_LOCAL_MISS_<COMP>    1 when the cache did not supply the component
#   CI_LOCAL_NIC            the matrix NIC this runner carries
#   CI_LOCAL_RUNNER         runner, baremetal, or host for a real run here
#   PCI_DEVICE              the PCI IDs that NIC maps to (containers only)
#
# Two steps of the real action are reported as skipped rather than faked: the ICE
# driver alignment, which would replace a module in the running kernel, and the
# shadow-host rsync, which needs a second machine. In a container the hardware
# steps are skipped too -- see the closing section.

set -uo pipefail

WORKDIR="${CI_LOCAL_WORKDIR:-/github/workspace}"
OUT_DIR="${CI_LOCAL_OUT:-/github/out}"
NIC="${CI_LOCAL_NIC:-}"
RUNNER="${CI_LOCAL_RUNNER:-baremetal}"
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

# ── step: system: Check host prerequisites ──────────────────────────────────
# Part of the action's dependency validation. Advisory here: a container has no
# say over the kernel it was given, so a failure must not fail the local run.
echo "::group::system: Check host prerequisites"
bash "${WORKDIR}/script/build_ebpf_xdp.sh" --check || true
echo "::endgroup::"

# ── step: cache: Restore dependency artifacts ───────────────────────────────
# The action restores each with fail-on-cache-miss: true, except plugins,
# whose absence only disables the codec tests. A miss here is the local
# equivalent of that hard failure -- it means the build job never published
# an artifact for these sources.
echo "::group::cache: Restore build artifacts"
components=(dpdk mtl jpegxs ffmpeg gstreamer plugins)
# Same call the action makes: the ICE bundle is only part of this host's
# environment when its card is served by the ice driver.
ice_required=$(NIC="${NIC}" bash "${WORKDIR}/.github/scripts/ci/ice-required.sh")
if [ "${ice_required}" = "true" ]; then
	components+=(ice)
else
	echo "  skipped ice (a ${NIC} is not served by the ice driver)"
fi
for comp in "${components[@]}"; do
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
NIC="${NIC}" task ci:validate-dependencies ||
	step_failed "cache: structural validation failed"
task ci:configure-host -- make-executable || step_failed "cache: executable alignment failed"

# ── step: Align the DPDK driver plugin path with the restored tree ───────────
# The one step whose need is louder locally than in CI: the build job's DPDK was
# installed under the container's /github/workspace, and this run is on the host.
echo "::group::dpdk: Align the driver plugin path"
task ci:configure-host -- dpdk-plugins || step_failed "dpdk: plugin path alignment failed"
echo "::endgroup::"

# ── step: kahawai: Generate CI plugin registry from cache ───────────────────
echo "::group::kahawai: Generate CI plugin registry from cache"
RUNNER_TEMP="$OUT_DIR" task ci:configure-host -- registry || step_failed "kahawai: registry generation failed"
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
# The acceptance framework reaches the system under test over SSH even when the
# system under test is this machine, so an unauthorized key fails every test
# with a connection error and no hint about why. Only checkable where there is an
# sshd, which is not the container.
if [ "${RUNNER}" = "host" ]; then
	check 1 "ssh to 127.0.0.1 with the CI key" ssh -n -o BatchMode=yes \
		-o StrictHostKeyChecking=accept-new -i "${HOME}/.ssh/id_ed25519" \
		"$(id -un)@127.0.0.1" true
fi

# The one skipped step that the suite cannot survive. An E8xx VF gets its rate
# limiter from the PF, and the PF only offers it when ice is the Kahawai build
# the fleet installs: under the in-tree ice the VF negotiates no QoS capability,
# and MTL's TM pacing path then walks a NULL qos_cap inside DPDK's iavf PMD.
# That is a SIGSEGV a couple of minutes into the first test, with nothing in the
# log connecting it to the driver -- so ask here, where the answer is one line
# and names the command that fixes it.
if [ "${RUNNER}" = "host" ] && [ "${ice_required}" = "true" ]; then
	if bash "${WORKDIR}/.github/scripts/ci/activate-ice.sh" --check >/dev/null 2>&1; then
		echo "  ok       the running ice driver is the cached Kahawai module"
	else
		step_failed "verify: the running ice driver is not the cached Kahawai module -- align it once with: sudo -E \$(command -v task) ci:activate-ice"
	fi
fi

if [ -x "${LI}/mtl/bin/RxTxApp" ]; then
	if ldd "${LI}/mtl/bin/RxTxApp" 2>/dev/null | grep -q 'not found'; then
		ldd "${LI}/mtl/bin/RxTxApp" 2>/dev/null | grep 'not found' | sed 's/^/  /'
		step_failed "verify: RxTxApp has unresolved shared libraries"
	else
		echo "  ok       RxTxApp resolves all shared libraries"
	fi
fi
echo "::endgroup::"

# ── what is not done here ───────────────────────────────────────────────────
echo "::group::not simulated"
if [ "${RUNNER}" = "host" ]; then
	cat <<-EOF
		This is the real host, so the hardware steps are not simulated -- the test
		job that follows does them itself. One step of the action is still skipped:

		  nic ${NIC:-<none>}
		  - Kahawai ICE driver activation    (replacing a module in the running
		                                      kernel is not something a local run
		                                      should do to a developer's machine;
		                                      verified above instead, since an E8xx
		                                      suite cannot pass without it)
		  - shadow-host rsync                (needs a second machine)
	EOF
else
	cat <<-EOF
		The steps below need the real host and are skipped here. Everything the test
		jobs do *before* touching hardware has been exercised above.

		  nic ${NIC:-<none>}${PCI_DEVICE:+, pci ${PCI_DEVICE}}
		  - PF/VF binding and PCI passthrough  (no devices in the container)
		  - Kahawai ICE driver activation      (host-wide state)
		  - hugepages, PTP, MtlManager         (host-wide state)
	EOF
fi
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
