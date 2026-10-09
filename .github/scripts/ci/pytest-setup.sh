#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
# shellcheck disable=SC2154 # acceptance_venv/venv_python come from the sourced lib

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"
stamp="${acceptance_venv}/.mtl-requirements-sha256"
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
	user=${RUNNER_USERNAME:-$(id -un)}
	home=$(getent passwd "${user}" | cut -d: -f6) || true
	home=${home:-${HOME}}
	key=${RUNNER_SSH_KEY:-${home}/.ssh/id_ed25519}
}

# Prints "<pci_device> <interface_type>"; two-port cards list the device twice.
resolve_nic() {
	local nic=$1 ids id ports=2 datapath=VF
	case "${nic}" in
	e810) ids="1592 1593 159b" ;;
	e830) ids="12d2 12d3" ;;
	e825) ids="579d 579e" ;;
	e835) ids="1249 124a" ;;
	i225) ids="15f2 15f3 15f8 0d9f 3100" ;;
	i226) ids="125b 125c 125d 3102" ;;
	*)
		echo "Unsupported NIC: ${nic}" >&2
		return 1
		;;
	esac
	if ! command -v lspci >/dev/null 2>&1; then
		id=${ids%% *}
		echo "Warning: lspci unavailable, assuming 8086:${id} for ${nic}" >&2
	else
		for id in ${ids}; do
			ports=$(lspci -Dn -d "8086:${id}" 2>/dev/null | wc -l)
			((ports == 0)) || break
		done
		if ((ports == 0)); then
			echo "No ${nic} NIC found on this host (looked for 8086:{${ids// /,}})." >&2
			echo "Either the runner label is wrong or the card is missing:" >&2
			lspci -nn -d '8086::0200' >&2 || true
			return 1
		fi
	fi
	# No SR-IOV on i225/i226: two PFs, or the kernel socket datapath on one port.
	if [[ ${nic} == i22[56] ]]; then
		datapath=KERNEL
		((ports < 2)) || datapath=PF
	elif ((ports < 2)); then
		echo "Found only ${ports} port(s) of 8086:${id}; the ${datapath} datapath needs two." >&2
		return 1
	fi
	echo "Resolved ${nic} to 8086:${id} (${ports} ports, ${datapath} datapath)" >&2
	id=8086:${id}
	((ports < 2)) || id+=",${id}"
	echo "${id} ${datapath}"
}

# Expands a BDF or vendor:device to one entry per port, capped at two for ST2022-7.
perf_card_ports() {
	local declared=$1 dev=$1 ports=0
	if [[ ${declared} == *,* ]]; then
		echo "${declared}"
		return 0
	fi
	if ! command -v lspci >/dev/null 2>&1; then
		echo "lspci is needed to resolve the perf card (${declared}); install pciutils." >&2
		return 1
	fi
	[[ ${declared} != *.* ]] || dev=$(lspci -Dn -s "${declared}" 2>/dev/null | awk 'NR==1 {print $3}')
	# `lspci -d ""` matches every device.
	[[ -z ${dev} ]] || ports=$(lspci -Dn -d "${dev}" 2>/dev/null | wc -l)
	if ((ports < 1)); then
		echo "No port of the declared perf card (${declared}) is on this host:" >&2
		lspci -Dnn -d '::0200' >&2 || true
		return 1
	fi
	echo "Resolved perf card ${dev} (${ports} ports)" >&2
	((ports < 2)) || dev+=",${dev}"
	echo "${dev}"
}

requirements_sha() { sha256sum "${acceptance_dir}/requirements.txt" | cut -d' ' -f1; }
# A venv whose system python was upgraded keeps bin/python3 but cannot start.
venv_usable() { [[ -x ${venv_python} ]] && "${venv_python}" -c pass 2>/dev/null; }
venv_current() { venv_usable && [[ $(cat "${stamp}" 2>/dev/null) == "$(requirements_sha)" ]]; }

provision_venv() {
	mkdir -p "$(dirname "${acceptance_venv}")"
	exec 9>"${acceptance_venv}.lock"
	flock 9
	if venv_current; then
		echo "Another job provisioned ${acceptance_venv} while this one waited"
		return 0
	fi
	echo "Provisioning ${acceptance_venv} (${1})"
	if ! venv_usable; then
		# virtualenv and uv carry their own pip, so they work without python3-venv.
		local builder=()
		if python3 -c 'import ensurepip' 2>/dev/null; then
			builder=(python3 -m venv)
		elif command -v virtualenv >/dev/null 2>&1; then
			builder=(virtualenv --python python3)
		elif command -v uv >/dev/null 2>&1; then
			builder=(uv venv --seed --python python3)
		else
			echo "python3 cannot create virtualenvs (no ensurepip), and neither virtualenv nor uv is installed." >&2
			echo "Install it once on the runner: sudo apt-get install -y python3-venv" >&2
			return 1
		fi
		echo "Building it with: ${builder[*]}"
		rm -rf "${acceptance_venv}"
		"${builder[@]}" "${acceptance_venv}"
	fi
	"${venv_python}" -m pip install --disable-pip-version-check -r "${acceptance_dir}/requirements.txt"
	requirements_sha >"${stamp}"
	echo "Provisioned ${acceptance_venv}"
}

key_hint() {
	echo "Authorise it once -- same account, same host, so this needs no login:" >&2
	echo "  install -d -m 700 ${home}/.ssh" >&2
	echo "  cat ${key}.pub >> ${home}/.ssh/authorized_keys" >&2
	echo "  chmod 600 ${home}/.ssh/authorized_keys" >&2
	echo "sshd ignores the file when ${home} or ${home}/.ssh is group-writable (StrictModes)." >&2
}

# The suite reaches even this host over SSH; paramiko misreports a bad key as a refused password.
verify_self_login() {
	local output
	if [[ ! -r ${key} ]]; then
		echo "No readable SSH key at ${key}; the generated config tells the framework to use it." >&2
		echo "Create it on the host once with: ssh-keygen -t ed25519 -N '' -f ${key}" >&2
		key_hint
		echo "A host that keeps its key elsewhere sets RUNNER_SSH_KEY in ${runner_env}." >&2
		return 1
	fi
	if ! output=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
		-i "${key}" "${user}@127.0.0.1" true 2>&1); then
		echo "The login the suite makes does not work: ssh -i ${key} ${user}@127.0.0.1" >&2
		printf '%s\n' "${output}" >&2
		key_hint
		return 1
	fi
	echo "SSH to 127.0.0.1 as ${user} with ${key} works, so the framework can reach this host."
}

case "${1:-}" in
verify)
	if [[ ! -x ${venv_python} ]]; then
		echo "Missing acceptance virtualenv at ${venv_python}." >&2
		echo "Provision it on the runner once with: task ci:pytest-setup -- install" >&2
		exit 1
	fi
	"${venv_python}" -m pytest --version
	"${venv_python}" -m pip check || true
	load_runner_env
	verify_self_login
	;;
connection)
	load_runner_env
	verify_self_login
	;;
ensure)
	if ! venv_usable; then
		provision_venv "no usable interpreter at ${venv_python}"
	elif ! venv_current; then
		provision_venv "requirements.txt differs from the one it was built from"
	fi
	"${venv_python}" -m pytest --version
	"${venv_python}" -m pip check || true
	;;
install)
	provision_venv "requested explicitly"
	;;
workspace)
	# Raw video recordings (~250 MB/s) left by an interrupted run would fill the disk mid-case.
	shopt -s nullglob
	for artifact in "${root_dir}"/tests/*_out_*.* "${root_dir}"/tests/*_ref_*.*; do
		printf 'Removing %s (%s) left behind by an interrupted run\n' \
			"${artifact##*/}" "$(du -h "${artifact}" | cut -f1)"
		rm -f "${artifact}"
	done
	required_gib=${MIN_FREE_GIB:-$((${TEST_TIME:-30} * 250 * 3 / 2 / 1024 + 1))}
	free_gib=$(df --block-size=1G --output=avail "${root_dir}" | tail -n1 | tr -d ' ')
	echo "Workspace filesystem: ${free_gib} GiB free, ${required_gib} GiB needed"
	if ((free_gib < required_gib)); then
		echo "Not enough room where the suite records raw video (${root_dir}/tests)." >&2
		echo "Free ${required_gib} GiB on this host, or lower the leg's test_time." >&2
		df -h "${root_dir}" >&2
		exit 1
	fi
	;;
session)
	: "${RUNNER_NAME:?RUNNER_NAME is required}"
	printf 'SESSION_ID=%s\n' "${RUNNER_NAME##*-}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci)
	read -r pci_device interface_type < <(resolve_nic "${NIC:?NIC is required}")
	# Kernel socket RX asks SO_RCVBUF for 4 MiB, capped by rmem_max (doc/kernel_socket.md).
	rmem_max=$(sysctl -n net.core.rmem_max)
	if ((rmem_max >= 4194304)); then
		echo "net.core.rmem_max is ${rmem_max}, enough for the kernel socket datapath"
	else
		sudo sysctl -q -w net.core.rmem_max=4194304
		echo "net.core.rmem_max: raised ${rmem_max} -> 4194304 for the kernel socket datapath"
	fi
	printf 'PCI_DEVICE=%s\nINTERFACE_TYPE=%s\n' "${pci_device}" "${interface_type}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci-env)
	load_runner_env
	# A separate assignment so set -e stops on a card the host does not have.
	pci_device=$(perf_card_ports "${PCI_DEVICE:-${PERF_PCI_DEVICE:-8086:12d2}}")
	printf 'PCI_DEVICE=%s\n' "${pci_device}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
config-single)
	load_runner_env
	args=(
		--session_id "${SESSION_ID:?SESSION_ID is required}"
		--mtl_path "${MTL_PATH:-${root_dir}}"
		--pci_device "${PCI_DEVICE:?PCI_DEVICE is required}"
		--ip_address 127.0.0.1
		--username "${user}"
		--key_path "${key}"
	)
	[[ -z ${TEST_TIME:-} ]] || args+=(--test_time "${TEST_TIME}")
	[[ -z ${INTERFACE_TYPE:-} ]] || args+=(--interface_type "${INTERFACE_TYPE}")
	if [[ ${NO_CAPTURE:-0} == 1 ]]; then
		args+=(--no_capture)
	elif [[ -n ${EBU_IP:-} ]]; then
		args+=(--ebu_ip "${EBU_IP}" --ebu_user "${EBU_USER:-}" --ebu_password "${EBU_PASSWORD:-}")
	fi
	cd "${acceptance_dir}/configs"
	"${venv_python}" gen_config.py "${args[@]}"
	;;
config-perf)
	load_runner_env
	cd "${acceptance_dir}/configs"
	"${venv_python}" gen_config.py \
		--session_id "${SESSION_ID:?SESSION_ID is required}" \
		--mtl_path "${root_dir}" "${root_dir}" \
		--pci_device "${PCI_DEVICE:?PCI_DEVICE is required}" "${PCI_DEVICE}" \
		--ip_address "${SHADOW_IP:?SHADOW_IP is required}" "${SUT_IP:?SUT_IP is required}" \
		--username "${SHADOW_USER:?SHADOW_USER is required}" \
		--key_path "${key}" \
		--test_time "${TEST_TIME:-120}" --no_capture
	;;
tag)
	printf 'MTL_GITHUB_WORKFLOW=%s\n' "${WORKFLOW_TAG:?WORKFLOW_TAG is required}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
*)
	echo "Unknown mode: ${1:-}" >&2
	exit 2
	;;
esac
