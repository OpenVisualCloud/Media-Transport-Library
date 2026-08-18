#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
venv_python="${acceptance_dir}/.venv/bin/python3"

# Host facts (EBU LIST credentials, shadow/SUT addresses, the account the tests
# run as) live on the runner that owns the hardware, not in GitHub secrets: the
# jobs that need them only ever run on that hardware, and a secret is a second
# copy of lab configuration that has to be kept in sync by hand.
runner_env=${MTL_CI_RUNNER_ENV:-/etc/mtl-ci/runner.env}

load_runner_env() {
	if [[ -r ${runner_env} ]]; then
		echo "Loading runner configuration from ${runner_env}"
		set -a
		# shellcheck source=/dev/null
		source "${runner_env}"
		set +a
	else
		echo "No runner configuration at ${runner_env}, using defaults"
	fi
}

# Candidate PCI device IDs per NIC label, most common first. A label is a claim
# about hardware, so it is resolved against the hardware that is actually in the
# host instead of being trusted: a runner advertising a label for a card it does
# not have used to fail deep inside a test.
nic_device_ids() {
	case "${1}" in
	e810) echo "1592 1593 159b" ;;
	e830) echo "12d2 12d3" ;;
	e825) echo "579d 579e" ;;
	e835) echo "1249 124a" ;;
	i225) echo "15f2 15f3 15f8 0d9f 3100" ;;
	i226) echo "125b 125c 125d 3102" ;;
	*) return 1 ;;
	esac
}

count_pci_functions() {
	lspci -Dn -d "8086:${1}" 2>/dev/null | wc -l
}

resolve_pci_device() {
	local nic=$1 candidates first_candidate="" found="" ports=0
	if ! candidates=$(nic_device_ids "${nic}"); then
		echo "Unsupported NIC: ${nic}" >&2
		return 1
	fi

	if ! command -v lspci >/dev/null 2>&1; then
		first_candidate=${candidates%% *}
		echo "Warning: lspci unavailable, assuming 8086:${first_candidate} for ${nic}" >&2
		printf '8086:%s,8086:%s\n' "${first_candidate}" "${first_candidate}"
		return 0
	fi

	for candidate in ${candidates}; do
		ports=$(count_pci_functions "${candidate}")
		if [[ ${ports} -gt 0 ]]; then
			found=${candidate}
			break
		fi
	done

	if [[ -z ${found} ]]; then
		echo "No ${nic} NIC found on this host (looked for 8086:{${candidates// /,}})." >&2
		echo "Either the runner label is wrong or the card is missing:" >&2
		lspci -nn -d '8086::0200' >&2 || true
		return 1
	fi

	if [[ ${ports} -lt 2 ]]; then
		echo "Found only ${ports} port(s) of 8086:${found}; the single-host tests need two." >&2
		return 1
	fi

	echo "Resolved ${nic} to 8086:${found} (${ports} ports)" >&2
	printf '8086:%s,8086:%s\n' "${found}" "${found}"
}

case "${1:-}" in
verify)
	# CI installs nothing on a bare-metal runner: the acceptance virtualenv is
	# part of the host image, so a missing one is a host problem to fix on the
	# host, not something a job should paper over by installing at test time.
	if [[ ! -x ${venv_python} ]]; then
		echo "Missing acceptance virtualenv at ${venv_python}." >&2
		echo "Provision it on the runner once with: task ci:pytest-setup -- install" >&2
		exit 1
	fi
	"${venv_python}" -m pytest --version
	"${venv_python}" -m pip check || true
	;;
install)
	# Provisioning path, for a developer machine or a one-off runner rebuild.
	# No CI job calls this.
	python3 -m venv "${acceptance_dir}/.venv"
	"${venv_python}" -m pip install -r "${acceptance_dir}/requirements.txt"
	;;
session)
	: "${RUNNER_NAME:?RUNNER_NAME is required}"
	printf 'SESSION_ID=%s\n' "${RUNNER_NAME##*-}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci)
	pci_device=$(resolve_pci_device "${NIC:?NIC is required}")
	printf 'PCI_DEVICE=%s\n' "$pci_device" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci-env)
	# For runners that carry no NIC label (the perf SUT pair): the host states
	# which ports the perf rig owns, and an E830 pair is the default.
	load_runner_env
	printf 'PCI_DEVICE=%s\n' "${PCI_DEVICE:-8086:12d2,8086:12d2}" \
		>>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
config-single)
	load_runner_env
	: "${SESSION_ID:?SESSION_ID is required}"
	: "${PCI_DEVICE:?PCI_DEVICE is required}"
	args=(
		--session_id "$SESSION_ID"
		--mtl_path "${MTL_PATH:-$root_dir}"
		--pci_device "$PCI_DEVICE"
		--ip_address 127.0.0.1
		--username "${RUNNER_USERNAME:-$(id -un)}"
		--key_path "/home/${USER}/.ssh/id_ed25519"
	)
	if [[ -n ${TEST_TIME:-} ]]; then
		args+=(--test_time "$TEST_TIME")
	fi
	if [[ -n ${INTERFACE_TYPE:-} ]]; then
		args+=(--interface_type "$INTERFACE_TYPE")
	fi
	if [[ ${NO_CAPTURE:-0} == 1 ]]; then
		args+=(--no_capture)
	elif [[ -n ${EBU_IP:-} ]]; then
		args+=(--ebu_ip "$EBU_IP" --ebu_user "${EBU_USER:-}" --ebu_password "${EBU_PASSWORD:-}")
	fi
	(cd "$acceptance_dir/configs" && "${venv_python}" gen_config.py "${args[@]}")
	;;
config-perf)
	load_runner_env
	(cd "$acceptance_dir/configs" && "${venv_python}" gen_config.py \
		--session_id "${SESSION_ID:?SESSION_ID is required}" \
		--mtl_path "$root_dir" "$root_dir" \
		--pci_device "${PCI_DEVICE:?PCI_DEVICE is required}" "$PCI_DEVICE" \
		--ip_address "${SHADOW_IP:?SHADOW_IP is required}" "${SUT_IP:?SUT_IP is required}" \
		--username "${SHADOW_USER:?SHADOW_USER is required}" \
		--key_path "/home/${USER}/.ssh/id_ed25519" \
		--test_time "${TEST_TIME:-120}" --no_capture)
	;;
tag)
	: "${WORKFLOW_TAG:?WORKFLOW_TAG is required}"
	printf 'MTL_GITHUB_WORKFLOW=%s\n' "$WORKFLOW_TAG" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
*)
	echo "Usage: $0 {verify|install|session|pci|pci-env|config-single|config-perf|tag}" >&2
	exit 2
	;;
esac
