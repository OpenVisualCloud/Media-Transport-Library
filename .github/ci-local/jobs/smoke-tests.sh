#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The run-smoke-tests job of .github/workflows/smoke-tests.yml, one section per
# step, in order.
#
# Unlike the other job scripts here, this one only runs with --runner host.
# Every step after validate-host touches the card -- binding ports, reserving
# hugepages, starting MtlManager, moving ST 2110 packets -- and none of that is
# possible in a container, so simulating it would prove nothing. On the machine
# that owns the NIC this is not a simulation of the job: it is the job, with
# run-job.sh standing in for GitHub.
#
# Inputs come from .github/ci-local/run-job.sh:
#
#   CI_LOCAL_WORKDIR   the checkout, which in host mode is the real one
#   CI_LOCAL_OUT       where the report and the step summary are collected
#   CI_LOCAL_NIC       the NIC label, i.e. which matrix leg this is
#   CI_LOCAL_RUNNER    must be `host`
#
# The lab facts the workflow reads from the runner instead of from GitHub
# secrets are read the same way here, from $MTL_CI_RUNNER_ENV (default
# /etc/mtl-ci/runner.env).

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
	die "no NIC label: run-job.sh smoke-tests --nic i225 (this is runs-on)"
[ "${RUNNER}" = "host" ] ||
	die "smoke-tests needs --runner host: a ${RUNNER} runner cannot bind a NIC"

# GitHub's per-step channels are files. A step writes them, and the runner
# carries what it wrote into the following steps -- which is what sync_env does.
export GITHUB_ENV="${OUT_DIR}/smoke.env"
export GITHUB_PATH="${OUT_DIR}/smoke.path"
export GITHUB_STEP_SUMMARY="${OUT_DIR}/step-summary.md"
: >"${GITHUB_ENV}"
: >"${GITHUB_PATH}"
: >"${GITHUB_STEP_SUMMARY}"
# Only the report link in the step summary reads these, and there is no run and
# no uploaded artifact here for it to point at.
export GITHUB_REPOSITORY="${GITHUB_REPOSITORY:-OpenVisualCloud/Media-Transport-Library}"
export GITHUB_RUN_ID="${GITHUB_RUN_ID:-0}"
export ARTIFACT_ID="${ARTIFACT_ID:-0}"
# `runner.name` on the fleet is <host>-<session>; the session ID the tests
# derive from it namespaces one runner's addresses against its neighbours', and
# on a single local host any value in range will do.
export RUNNER_NAME="${RUNNER_NAME:-ci-local-1}"

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

# ── the matrix leg ──────────────────────────────────────────────────────────
# Read out of the workflow rather than copied from it: the suite, the traffic
# duration and whether the leg captures are what make an i225 run differ from an
# e810 one, and a local copy of those values would drift the moment the matrix
# changes.
leg=$(
	python3 - "${NIC}" <<-'PY'
		import sys, yaml
		nic = sys.argv[1]
		wf = yaml.safe_load(open(".github/workflows/smoke-tests.yml"))
		legs = wf["jobs"]["run-smoke-tests"]["strategy"]["matrix"]["include"]
		leg = next((leg for leg in legs if leg["nic"] == nic), None)
		if leg is None:
		    sys.exit(f"no matrix leg for nic {nic}")
		# "-" stands for an empty test_time, i.e. the framework's own default.
		print(leg["suite"], leg["no_capture"], leg.get("test_time") or "-")
	PY
) || die "could not read the ${NIC} leg of smoke-tests.yml: ${leg}"
read -r SUITE NO_CAPTURE TEST_TIME <<<"${leg}"
[ "${TEST_TIME}" = "-" ] && TEST_TIME=""
echo "matrix leg: nic=${NIC} suite=${SUITE} no_capture=${NO_CAPTURE} test_time=${TEST_TIME:-<default>}"

# ── step: preparation: Validate host and download artifacts ─────────────────
# ./.github/actions/validate-host, which the local runner already mirrors step
# for step. It also configures the environment the tests inherit, into its own
# GITHUB_ENV/GITHUB_PATH files, so those are imported here the way the runner
# carries a composite action's exports into the next step.
step "preparation: Validate host and download artifacts"
bash "${CI_LOCAL_DIR}/jobs/validate-host.sh" || die "validate-host failed"
echo "::endgroup::"
sync_env "${OUT_DIR}/validate-host.env" "${OUT_DIR}/validate-host.path"

# ── step: cleanup: Kill stale processes before test ─────────────────────────
step "cleanup: Kill stale processes before test"
bash "${WORKDIR}/.github/scripts/ci/cleanup.sh" || die "cleanup failed"
echo "::endgroup::"

# ── step: preparation: Ensure the acceptance virtualenv ─────────────────────
step "preparation: Ensure the acceptance virtualenv"
task ci:pytest-setup -- ensure || die "the acceptance virtualenv is not usable"
echo "::endgroup::"

# ── step: Create session ID ─────────────────────────────────────────────────
step "Create session ID"
task ci:pytest-setup -- session || die "session ID"
echo "::endgroup::"
sync_env

# ── step: Set PCI device env variable ───────────────────────────────────────
# The label is resolved against the cards actually in this host, which is also
# what decides the datapath: VF on an SR-IOV card, PF on a two-port i225/i226,
# and MTL's kernel socket on a single-port one.
step "Set PCI device env variable"
NIC="${NIC}" task ci:pytest-setup -- pci || die "no usable ${NIC} in this host"
echo "::endgroup::"
sync_env

# ── step: Generate test framework config files ──────────────────────────────
step "Generate test framework config files"
SESSION_ID="${SESSION_ID:?the session step exported no SESSION_ID}" \
	PCI_DEVICE="${PCI_DEVICE:?the pci step exported no PCI_DEVICE}" \
	INTERFACE_TYPE="${INTERFACE_TYPE:-}" NO_CAPTURE="${NO_CAPTURE}" \
	TEST_TIME="${TEST_TIME}" \
	task ci:pytest-setup -- config-single || die "config generation"
echo "::endgroup::"
cp -f "${WORKDIR}/tests/acceptance/configs/test_config.yaml" \
	"${WORKDIR}/tests/acceptance/configs/topology_config.yaml" "${OUT_DIR}/" 2>/dev/null || true

# ── step: Export workflow tag for PCAP naming ───────────────────────────────
step "Export workflow tag for PCAP naming"
WORKFLOW_TAG="ci-local:${NIC}" task ci:pytest-setup -- tag || die "workflow tag"
echo "::endgroup::"
sync_env

# ── step: preparation: Make room for the raw video the suite records ─────────
step "preparation: Make room for the raw video the suite records"
TEST_TIME="${TEST_TIME}" task ci:pytest-setup -- workspace ||
	die "not enough free space for a ${SUITE} run, or leftovers could not be removed"
echo "::endgroup::"

# ── step: preparation: Verify the media the suite reads ──────────────────────
step "preparation: Verify the media the suite reads"
task ci:media-assets -- verify ||
	die "the suite would skip its media cases and report a green run"
echo "::endgroup::"

# ── step: preparation: Verify the compliance analyser ────────────────────────
if [ "${NO_CAPTURE}" != "1" ]; then
	step "preparation: Verify the compliance analyser"
	task ci:ebu-list -- verify ||
		die "no compliance verdict is reachable, and every capture case needs one"
	echo "::endgroup::"
fi

# ── step: execution: Run the <suite> suite ──────────────────────────────────
echo "::group::execution: Run the ${SUITE} suite"
rc=0
task ci:pytest-run -- "${SUITE}" || rc=$?
echo "::endgroup::"

# ── step: upload report / Add report to summary ─────────────────────────────
# There is no artifact store here, so the report is copied where the rest of
# this run's output is collected.
step "upload report"
for artifact in report.html report.json; do
	[ -f "${WORKDIR}/${artifact}" ] && cp -f "${WORKDIR}/${artifact}" "${OUT_DIR}/"
done
echo "::endgroup::"
step "Add report to summary"
task ci:report -- smoke-summary || echo "::warning::no summary produced"
cat "${GITHUB_STEP_SUMMARY}"
echo "::endgroup::"

[ "${rc}" -eq 0 ] || echo "::error::the ${SUITE} suite failed (exit ${rc})"
exit "${rc}"
