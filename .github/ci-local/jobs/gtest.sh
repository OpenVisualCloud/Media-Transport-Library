#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The run-gtest-tests job of .github/workflows/gtest-bare-metal.yml, one section
# per step, in order. NIGHTLY=1 makes it the run-gtest job of
# .github/workflows/nightly-gtest.yml, which is the same steps with the full
# suite and without the fail-fast.
#
# Like smoke-tests, this one only runs with --runner host: every step after
# validate-host touches the card, and a container cannot bind a PCI device or
# load a driver, so simulating it would prove nothing. On the machine that owns
# the NIC this is not a simulation of the job -- it is the job, with run-job.sh
# standing in for GitHub.
#
# Inputs come from .github/ci-local/run-job.sh:
#
#   CI_LOCAL_WORKDIR   the checkout, which in host mode is the real one
#   CI_LOCAL_OUT       where the log is collected
#   CI_LOCAL_NIC       the NIC label, i.e. which matrix leg this is
#   CI_LOCAL_RUNNER    must be `host`
#
# NIGHTLY, EXIT_ON_FAILURE and TEST_CASE_TIMEOUT are read from the environment
# so a local run can pick either workflow's settings.

set -uo pipefail

WORKDIR="${CI_LOCAL_WORKDIR:-/github/workspace}"
OUT_DIR="${CI_LOCAL_OUT:-/github/out}"
NIC="${CI_LOCAL_NIC:-}"
RUNNER="${CI_LOCAL_RUNNER:-runner}"
CI_LOCAL_DIR="${WORKDIR}/.github/ci-local"

cd "${WORKDIR}" || exit 1
mkdir -p "${OUT_DIR}"

die() {
	echo "::error::$*"
	exit 1
}

[ -n "${NIC}" ] ||
	die "no NIC label: run-job.sh gtest --nic e810 (this is runs-on)"
[ "${RUNNER}" = "host" ] ||
	die "gtest needs --runner host: a ${RUNNER} runner cannot bind a NIC"

# GitHub's per-step channels are files. A step writes them, and the runner
# carries what it wrote into the following steps -- which is what sync_env does.
export GITHUB_ENV="${OUT_DIR}/gtest.env"
export GITHUB_PATH="${OUT_DIR}/gtest.path"
export GITHUB_STEP_SUMMARY="${OUT_DIR}/step-summary.md"
: >"${GITHUB_ENV}"
: >"${GITHUB_PATH}"
: >"${GITHUB_STEP_SUMMARY}"

sync_env() {
	# sync_env [env_file [path_file]] -- GITHUB_ENV and GITHUB_PATH, applied the
	# way the runner applies them between steps. Both are append-only, so this
	# is safe to repeat.
	local env_file="${1:-${GITHUB_ENV}}" path_file="${2:-${GITHUB_PATH}}"
	if [ -s "${env_file}" ]; then
		set -a
		# shellcheck disable=SC1090 # written by the steps above
		. "${env_file}"
		set +a
	fi
	if [ -s "${path_file}" ]; then
		PATH="$(paste -sd: "${path_file}"):${PATH}"
		export PATH
	fi
}

step() {
	echo "::group::$*"
}

# The `cleanup: Kill test processes` step is `if: always()`, so it belongs to the
# exit path rather than to the sequence below.
trap 'echo "::group::cleanup: Kill test processes"
	bash "${WORKDIR}/.github/scripts/ci/cleanup.sh" || true
	echo "::endgroup::"' EXIT

: "${NIGHTLY:=0}"
: "${EXIT_ON_FAILURE:=1}"
export NIGHTLY EXIT_ON_FAILURE
export LOG_FILE="${OUT_DIR}/gtest.log"
echo "matrix leg: nic=${NIC} nightly=${NIGHTLY} exit_on_failure=${EXIT_ON_FAILURE}"

# ── step: Validate host and download artifacts ──────────────────────────────
# ./.github/actions/validate-host, which the local runner already mirrors step
# for step, including the privileged ICE activation.
step "Validate host and download artifacts"
bash "${CI_LOCAL_DIR}/jobs/validate-host.sh" || die "validate-host failed"
echo "::endgroup::"
sync_env "${OUT_DIR}/validate-host.env" "${OUT_DIR}/validate-host.path"

# ── step: cleanup: Kill stale processes before test ─────────────────────────
step "cleanup: Kill stale processes before test"
bash "${WORKDIR}/.github/scripts/ci/cleanup.sh" || die "cleanup failed"
echo "::endgroup::"

# ── step: Bind the test ports ───────────────────────────────────────────────
# The one step that changes NIC state. The suite that follows only reads it.
# env -u BASH_XTRACEFD as in the workflow: the descriptor that variable names
# belongs to the shell that set it, and is closed in the privileged one.
step "Bind the test ports"
sudo -E env -u BASH_XTRACEFD "$(command -v task)" ci:bind-test-ports ||
	die "the NIC could not be prepared for the suite"
echo "::endgroup::"

# ── step: Run gtest bare metal ──────────────────────────────────────────────
step "Run gtest bare metal"
rc=0
sudo -E env -u BASH_XTRACEFD "${WORKDIR}/.github/scripts/gtest.sh" || rc=$?
echo "::endgroup::"

[ "${rc}" -eq 0 ] || echo "::error::the gtest suite failed (exit ${rc}), log in ${LOG_FILE}"
exit "${rc}"
