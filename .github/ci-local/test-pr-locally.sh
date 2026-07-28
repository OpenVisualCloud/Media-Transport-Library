#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Run everything a pull request triggers, locally, in the order GitHub runs it.
#
#   Lint Code Base        linter.yml           (opt-in: --with-lint)
#     -> build            build.yml            produces .local_install/*
#          -> pr-gate     pr-gate.yml          would the test workflows run?
#               -> validate-host per NIC       smoke-tests.yml, gtest-bare-metal.yml
#
# The point is the feedback loop. Pushing a commit to find out costs a runner
# queue, a 60 minute build and a log you have to download; this costs the time
# of whatever actually changed, and stops at the first thing that breaks.
#
# What it cannot do is run the tests themselves -- those need a NIC, VFs, the
# Kahawai ICE driver and hugepages. It takes each matrix NIC as far as a
# container can go, which is every step up to the hardware, and says plainly
# where it stopped.
#
#   test-pr-locally.sh [OPTIONS]
#
#   --nic LIST        NICs to simulate, comma separated, or "none"
#                     (default: e810,e830,e835 -- the workflow matrix)
#   --with-lint       also run super-linter, as linter.yml does
#   --skip-build      assume .local_install is already populated
#   --force LIST      rebuild these components (see run-job.sh --force)
#   --clean           discard the working copy first
#   -h, --help        this help

set -uo pipefail

script_path="$(readlink -f "${BASH_SOURCE[0]}")"
CI_LOCAL_DIR="$(dirname "${script_path}")"
REPO_ROOT="$(cd "${CI_LOCAL_DIR}/../.." && pwd)"
RUN_JOB="${CI_LOCAL_DIR}/run-job.sh"

NICS="e810,e830,e835"
WITH_LINT=0
SKIP_BUILD=0
FORCE=""
CLEAN=0

while [ $# -gt 0 ]; do
	case "$1" in
	--nic)
		NICS="$2"
		shift 2
		;;
	--with-lint)
		WITH_LINT=1
		shift
		;;
	--skip-build)
		SKIP_BUILD=1
		shift
		;;
	--force)
		FORCE="$2"
		shift 2
		;;
	--clean)
		CLEAN=1
		shift
		;;
	-h | --help)
		sed -n '5,33p' "${script_path}" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "unknown option: $1 (try --help)" >&2
		exit 2
		;;
	esac
done

cd "${REPO_ROOT}" || exit 1

RESULTS=()
overall=0

record() {
	# record <name> <rc> <note>
	local name="$1" rc="$2" note="${3:-}"
	RESULTS+=("$([ "${rc}" -eq 0 ] && echo PASS || echo FAIL)|${name}|${note}")
	[ "${rc}" -eq 0 ] || overall=1
}

banner() {
	echo
	echo "═══ $1 ═══"
}

# ── Lint Code Base (linter.yml) ─────────────────────────────────────────────
# build.yml waits for this check before it starts, so it comes first. Opt-in:
# it pulls a large image and the repository's own hooks already cover most of
# what it reports.
if [ "${WITH_LINT}" -eq 1 ]; then
	banner "Lint Code Base  (linter.yml)"
	docker run --rm \
		--env RUN_LOCAL=true \
		--env DEFAULT_BRANCH=main \
		--env VALIDATE_CPP=false \
		--env VALIDATE_JSCPD=false \
		--env VALIDATE_JSON=false \
		--env VALIDATE_CHECKOV=false \
		--env VALIDATE_PYTHON_MYPY=false \
		--env VALIDATE_DOCKERFILE_HADOLINT=false \
		--env VALIDATE_PYTHON_PYLINT=false \
		--env VALIDATE_TYPESCRIPT_STANDARD=false \
		--env LOG_LEVEL=WARN \
		--volume "${REPO_ROOT}:/tmp/lint" \
		ghcr.io/super-linter/super-linter:slim-v6
	record "linter / Lint Code Base" $? ""
fi

# ── build (build.yml) ───────────────────────────────────────────────────────
if [ "${SKIP_BUILD}" -eq 0 ]; then
	banner "build  (build.yml)"
	build_args=()
	[ -n "${FORCE}" ] && build_args+=(--force "${FORCE}")
	[ "${CLEAN}" -eq 1 ] && build_args+=(--clean)
	"${RUN_JOB}" build "${build_args[@]}"
	build_rc=$?
	record "build / build" "${build_rc}" ""
	if [ "${build_rc}" -ne 0 ]; then
		echo
		echo "build failed; the test workflows consume its artifacts, so they are skipped."
		NICS="none"
	fi
else
	echo "skipping build (--skip-build)"
fi

# ── pr-gate (pr-gate.yml) ───────────────────────────────────────────────────
# The gate asks whether the change touches anything the bare-metal tests
# cover. dorny/paths-filter compares against the merge base; do the same.
banner "pr-gate  (pr-gate.yml)"
base="$(git merge-base HEAD origin/main 2>/dev/null || git rev-parse HEAD~1 2>/dev/null || echo HEAD)"
changed_files="$(git diff --name-only "${base}" 2>/dev/null)"
if [ -z "${changed_files}" ]; then
	echo "no changes against ${base}; the gate would report changed=false"
else
	echo "changed files against ${base}:"
	echo "${changed_files}" | sed 's/^/  /' | head -20
	[ "$(echo "${changed_files}" | wc -l)" -gt 20 ] && echo "  ... $(($(echo "${changed_files}" | wc -l) - 20)) more"
fi
record "pr-gate / check-for-changes" 0 "$(echo "${changed_files}" | grep -c . ) files"

# ── validate-host, per matrix NIC (smoke-tests.yml, gtest-bare-metal.yml) ───
if [ "${NICS}" != "none" ]; then
	IFS=',' read -ra nic_list <<<"${NICS}"
	for nic in "${nic_list[@]}"; do
		banner "validate-host  nic=${nic}  (smoke-tests.yml, gtest-bare-metal.yml)"
		"${RUN_JOB}" validate-host --nic "${nic}"
		record "validate-host / ${nic}" $? "nic ${nic}"
	done
fi

# ── summary ─────────────────────────────────────────────────────────────────
echo
echo "═══ test-pr-locally summary ═══"
printf '%-6s  %-34s  %s\n' "RESULT" "JOB" "NOTE"
for entry in "${RESULTS[@]}"; do
	IFS='|' read -r result name note <<<"${entry}"
	printf '%-6s  %-34s  %s\n' "${result}" "${name}" "${note}"
done

echo
if [ "${overall}" -eq 0 ]; then
	echo "everything that can run locally passed."
else
	echo "something failed; logs are under .ci-local/logs/"
fi
cat <<'EOF'

still only reproducible on a real host:
  smoke-tests   pytest -m smoke      needs a NIC, VFs and hugepages
  gtest         .github/scripts/gtest.sh   needs the Kahawai ICE driver
EOF

exit "${overall}"
