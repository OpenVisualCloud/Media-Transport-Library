#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root_dir"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

hash_output=$(bash script/hash_sources.sh)
grep -q '^  jpegxs:' <<<"$hash_output" || fail "jpegxs source hash is missing"
grep -q '^  ice:' <<<"$hash_output" || fail "ice source hash is missing"

for script in \
	.github/scripts/ci/build-jpegxs.sh \
	.github/scripts/ci/validate-jpegxs.sh \
	.github/scripts/ci/validate-cache.sh \
	.github/scripts/ci/build-ice.sh \
	.github/scripts/ci/validate-ice.sh \
	.github/scripts/ci/activate-ice.sh \
	.github/scripts/ci/bind-test-ports.sh \
	.github/scripts/ci/configure-host.sh; do
	test -x "$script" || fail "$script is missing or not executable"
done

bash .github/scripts/ci/check-yaml-policy.sh

task_path_literal="TASK_BIN=\$(command -v task)"
grep -Fq "$task_path_literal" .github/actions/validate-host/action.yml ||
	fail "validate-host does not preserve the absolute Task binary path"
if grep -q 'sudo -E task ' .github/actions/validate-host/action.yml; then
	fail "validate-host resolves Task through sudo secure_path"
fi

if grep -Eq 'build-ice|make install|modprobe|rmmod' .github/workflows/gtest-bare-metal.yml; then
	fail "gtest-bare-metal still builds or activates ICE"
fi

# Loading a driver and building the ports belong to the job, not to the suite.
# A NIC rebuilt under a suite that is already running is how a bare-metal runner
# gets wedged, and a retry that only passes after its card was rebuilt is not a
# pass worth reporting.
if grep -Eq 'modprobe|rmmod|create_tvf|create_vf|sriov_numvfs|devbind\.py (-b|--bind)' \
	.github/scripts/gtest.sh; then
	fail "gtest.sh still changes NIC state instead of only reading it"
fi
for workflow in .github/workflows/gtest-bare-metal.yml .github/workflows/nightly-gtest.yml; do
	grep -Fq 'ci:bind-test-ports' "$workflow" ||
		fail "$workflow does not prepare the test ports before running the suite"
done

# Every privileged hop on the fleet keeps the environment (-E) and so inherits
# BASH_XTRACEFD, whose value is a descriptor of the shell that set it and is
# closed in the one sudo starts. The step then opens with an error line about a
# trace descriptor, which is not what went wrong in any job that has ever been
# read as failing here.
for privileged in .github/workflows/gtest-bare-metal.yml \
	.github/workflows/nightly-gtest.yml \
	.github/actions/validate-host/action.yml \
	.github/ci-local/jobs/gtest.sh; do
	while read -r line; do
		case "$line" in
		*'sudo -E env -u BASH_XTRACEFD'*) ;;
		*) fail "$privileged: privileged step keeps BASH_XTRACEFD: ${line# }" ;;
		esac
	done < <(grep -F 'sudo -E' "$privileged" | grep -Ev '^[[:space:]]*#')
done

if grep -Eq 'sudo|modprobe|rmmod|depmod|make install' .github/scripts/ci/build-ice.sh; then
	fail "ICE producer still mutates the host"
fi
# The producer must not carry its own copy of the download, patch and compile
# steps: build_drivers.sh is what a developer runs, and --build-only is the only
# thing that keeps it off the running driver.
grep -Fq 'build_drivers.sh" --build-only' .github/scripts/ci/build-ice.sh ||
	fail "ICE producer does not delegate the compile to build_drivers.sh --build-only"

grep -Fq "run_command(find_program('cc'), '-dumpmachine'" manager/meson.build ||
	fail "manager XDP build does not derive the compiler multiarch tuple"
grep -Fq "'-I/usr/include/' + multiarch" manager/meson.build ||
	fail "manager XDP build does not include Ubuntu's multiarch headers"

# shellcheck disable=SC2016
[ "$(grep -Fc 'cd "$acceptance_dir/configs"' .github/scripts/ci/pytest-setup.sh)" -eq 2 ] ||
	fail "both pytest config modes must run in the configs directory"

# The acceptance virtualenv is a cache built from requirements.txt, so a test job
# builds it when the host has none -- `verify`, which only reports the gap, left
# every hardware leg on the fleet red until someone opened an SSH session, and
# `workflow_dispatch` cannot reach a host from a branch. The provisioning
# workflow keeps `verify`: what it is for is answering whether a host is ready.
for suite in .github/workflows/smoke-tests.yml \
	.github/workflows/nightly-pytest.yml \
	.github/workflows/custom-pytest.yml \
	.github/workflows/perf-pytest.yml \
	.github/ci-local/jobs/smoke-tests.sh; do
	grep -Fq 'ci:pytest-setup -- ensure' "$suite" ||
		fail "$suite does not ensure the acceptance virtualenv"
	if grep -Fq 'ci:pytest-setup -- verify' "$suite"; then
		fail "$suite still fails instead of building the virtualenv cache"
	fi
done
grep -Fq 'ci:pytest-setup -- verify' .github/workflows/provision-runner.yml ||
	fail "provisioning does not end with the pure check"
# Two legs can reach that step on one host at the same time.
grep -Fq 'flock 9' .github/scripts/ci/pytest-setup.sh ||
	fail "virtualenv provisioning is not serialised between jobs"
# Activation must decide before it touches anything: the "already the cached
# module" answer is the whole point of the script, and it is worthless if the
# host has been torn down by the time it is reached.
activation=.github/scripts/ci/activate-ice.sh
decision_line=$(grep -n '^if is_current; then' "$activation" | cut -d: -f1)
mutation_line=$(grep -nE '^(pkill|modprobe|install|depmod) ' "$activation" | head -n1 | cut -d: -f1)
if [ -z "$decision_line" ] || [ -z "$mutation_line" ] || [ "$decision_line" -gt "$mutation_line" ]; then
	fail "ICE activation mutates the host before deciding it has to"
fi
# --check answers the same question without being allowed to act on it, which is
# what the local harness asks: it reports the driver a suite needs rather than
# installing it. It is only that if it returns before the first mutation.
# shellcheck disable=SC2016
check_line=$(grep -n '^\[ "\$check_only" -eq 0 \] ||' "$activation" | cut -d: -f1)
if [ -z "$check_line" ] || [ "$check_line" -gt "$mutation_line" ]; then
	fail "ICE activation --check can reach the code that replaces the module"
fi
# The local runner skips activation on purpose, so it has to say when the driver
# it skipped is the one the suite needs -- an unaligned E8xx host otherwise fails
# as a SIGSEGV inside the DPDK iavf PMD, minutes later and with no hint.
grep -Fq 'activate-ice.sh" --check' .github/ci-local/jobs/validate-host.sh ||
	fail "local validate-host does not check the running ice driver"

# shellcheck disable=SC2016
grep -Fq 'rm -rf "${source_dir}/Build/ci"' .github/scripts/ci/build-jpegxs.sh ||
	fail "JPEG XS producer can reuse stale CMake compiler state"
# shellcheck disable=SC2016
cleanup_line=$(grep -nF 'rm -rf "${source_dir}/Build/ci"' .github/scripts/ci/build-jpegxs.sh | cut -d: -f1)
# shellcheck disable=SC2016
configure_line=$(grep -nF 'cmake -S "$source_dir"' .github/scripts/ci/build-jpegxs.sh | cut -d: -f1)
[ "$cleanup_line" -lt "$configure_line" ] ||
	fail "JPEG XS CMake cleanup occurs after configuration"

echo "prebuilt dependency contracts: PASS"
