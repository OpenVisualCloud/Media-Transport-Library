#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"

case "${1:-}" in
install)
	python3 -m venv "${acceptance_dir}/.venv"
	"${acceptance_dir}/.venv/bin/python3" -m pip install -r "${acceptance_dir}/requirements.txt"
	;;
session)
	: "${RUNNER_NAME:?RUNNER_NAME is required}"
	printf 'SESSION_ID=%s\n' "${RUNNER_NAME##*-}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci)
	case "${NIC:?NIC is required}" in
	e810) pci_device=8086:1592,8086:1592 ;;
	e830) pci_device=8086:12d2,8086:12d2 ;;
	e825) pci_device=8086:579d,8086:579d ;;
	e835) pci_device=8086:1249,8086:1249 ;;
	*)
		echo "Unsupported NIC: ${NIC}" >&2
		exit 1
		;;
	esac
	printf 'PCI_DEVICE=%s\n' "$pci_device" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
config-single)
	: "${SESSION_ID:?SESSION_ID is required}"
	: "${MTL_PATH:?MTL_PATH is required}"
	: "${PCI_DEVICE:?PCI_DEVICE is required}"
	args=(
		--session_id "$SESSION_ID"
		--mtl_path "$MTL_PATH"
		--pci_device "$PCI_DEVICE"
		--ip_address 127.0.0.1
		--username "${RUNNER_USERNAME:?RUNNER_USERNAME is required}"
		--key_path "/home/${USER}/.ssh/id_ed25519"
	)
	if [[ -n ${EBU_IP:-} ]]; then
		args+=(--ebu_ip "$EBU_IP" --ebu_user "${EBU_USER:-}" --ebu_password "${EBU_PASSWORD:-}")
	fi
	(cd "$acceptance_dir/configs" && "${acceptance_dir}/.venv/bin/python3" gen_config.py "${args[@]}")
	;;
config-perf)
	(cd "$acceptance_dir/configs" && "${acceptance_dir}/.venv/bin/python3" gen_config.py \
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
	echo "Usage: $0 {install|session|pci|config-single|config-perf|tag}" >&2
	exit 2
	;;
esac
