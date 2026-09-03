#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"

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

# How the tests attach to a card, given how many of its ports this host has.
#
# A DPDK port belongs to exactly one process, so a test with a transmitter and
# a receiver needs two of them: two VFs of one PF on an SR-IOV card, or the two
# PFs of a card without SR-IOV. A single-port card has neither, and its only
# remaining datapath is MTL's kernel socket (kernel:<ifname>), where TX and RX
# are two sockets on the one interface. That is slower than DPDK, which is why
# it is the last resort and never chosen for a card that has two ports.
nic_datapath() {
	local nic=$1 ports=$2
	case "${nic}" in
	i225 | i226)
		# No SR-IOV on these, so there is no VF to hand to DPDK.
		if [[ ${ports} -ge 2 ]]; then echo PF; else echo KERNEL; fi
		;;
	*) echo VF ;;
	esac
}

# Prints "<pci_device> <interface_type>": the PCI IDs the tests bind, and the
# datapath they bind them through.
resolve_nic() {
	local nic=$1 candidates first_candidate="" found="" ports=0 datapath
	if ! candidates=$(nic_device_ids "${nic}"); then
		echo "Unsupported NIC: ${nic}" >&2
		return 1
	fi

	if ! command -v lspci >/dev/null 2>&1; then
		first_candidate=${candidates%% *}
		echo "Warning: lspci unavailable, assuming 8086:${first_candidate} for ${nic}" >&2
		printf '8086:%s,8086:%s %s\n' "${first_candidate}" "${first_candidate}" \
			"$(nic_datapath "${nic}" 2)"
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

	datapath=$(nic_datapath "${nic}" "${ports}")
	if [[ ${ports} -lt 2 && ${datapath} != KERNEL ]]; then
		echo "Found only ${ports} port(s) of 8086:${found}; the ${datapath} datapath needs two." >&2
		return 1
	fi

	# One entry per port the tests may use, capped at the two a single-host test
	# needs. On a two-port card the second entry is also what gen_config.py
	# takes as the capture device unless --no_capture says otherwise.
	echo "Resolved ${nic} to 8086:${found} (${ports} ports, ${datapath} datapath)" >&2
	if [[ ${ports} -ge 2 ]]; then
		printf '8086:%s,8086:%s %s\n' "${found}" "${found}" "${datapath}"
	else
		printf '8086:%s %s\n' "${found}" "${datapath}"
	fi
}

# Raw video is enormous, and the suite records it into the workspace: an FFmpeg
# RX case writes what it receives to tests/<case>_<stamp>_out_<n>.yuv for as long
# as the case runs -- 1080p yuv422p10le is 8.3 MB a frame, so about 250 MB/s --
# and removes it once the checks are done. A run that never reaches that point (a
# cancelled job, a fired timeout) leaves the file behind, and the next run then
# fills the filesystem in the middle of a case and reports failing tests instead
# of a full disk.
prepare_workspace() {
	local artifact test_time free_gib required_gib
	shopt -s nullglob
	for artifact in "${root_dir}"/tests/*_out_*.* "${root_dir}"/tests/*_ref_*.*; do
		printf 'Removing %s (%s) left behind by an interrupted run\n' \
			"${artifact##*/}" "$(du -h "${artifact}" | cut -f1)"
		rm -f "${artifact}"
	done
	shopt -u nullglob

	# Room for one case's recording, half again for its logs and for the
	# reference copy an integrity case transcodes beside it. TEST_TIME is the
	# traffic duration the matrix leg asks for; without one the framework's own
	# default is well under a minute.
	test_time=${TEST_TIME:-30}
	required_gib=${MIN_FREE_GIB:-$((test_time * 250 * 3 / 2 / 1024 + 1))}
	free_gib=$(df --block-size=1G --output=avail "${root_dir}" | tail -n1 | tr -d ' ')
	echo "Workspace filesystem: ${free_gib} GiB free, ${required_gib} GiB needed"
	if ((free_gib < required_gib)); then
		echo "Not enough room where the suite records raw video (${root_dir}/tests)." >&2
		echo "Free ${required_gib} GiB on this host, or lower the leg's test_time." >&2
		df -h "${root_dir}" >&2
		return 1
	fi
}

# The requirements the virtualenv was built from, recorded inside it. A cache
# that cannot say what it holds goes stale silently: requirements.txt changes in
# a pull request and the host keeps running the environment of an older one.
#
# shellcheck disable=SC2154 # acceptance_venv/venv_python come from the sourced lib
requirements_stamp() {
	printf '%s\n' "${acceptance_venv}/.mtl-requirements-sha256"
}

requirements_sha() {
	sha256sum "${acceptance_dir}/requirements.txt" | cut -d' ' -f1
}

# Whether the virtualenv's interpreter still runs. A virtualenv records the
# system python it was built against, so an image whose python moves on -- an
# apt upgrade from 3.10 to 3.12 -- leaves one whose bin/python3 is present and
# cannot start. That is not "missing", and it fails several steps later.
#
# shellcheck disable=SC2154 # acceptance_venv/venv_python come from the sourced lib
venv_usable() {
	[[ -x ${venv_python} ]] && "${venv_python}" -c pass 2>/dev/null
}

venv_current() {
	venv_usable && [[ $(cat "$(requirements_stamp)" 2>/dev/null) == "$(requirements_sha)" ]]
}

# Build the virtualenv, under a lock so two jobs on the same host cannot race
# each other into a half-installed one. flock is the whole mechanism: the loser
# waits, then finds the winner's work and keeps it.
#
# shellcheck disable=SC2154 # acceptance_venv/venv_python come from the sourced lib
provision_acceptance_venv() {
	local reason=$1
	mkdir -p "$(dirname "${acceptance_venv}")"
	if command -v flock >/dev/null 2>&1; then
		exec 9>"${acceptance_venv}.lock"
		flock 9
		if venv_current; then
			echo "Another job provisioned ${acceptance_venv} while this one waited"
			return 0
		fi
	fi
	echo "Provisioning ${acceptance_venv} (${reason})"
	if ! venv_usable; then
		# Debian and Ubuntu ship the venv module in a separate package, and without
		# it `python3 -m venv` stops on "ensurepip is not available" -- a message
		# about a module nobody asked for, in the middle of a test job.
		#
		# `virtualenv` and `uv` build the same thing without it, because they carry
		# their own copy of pip instead of asking python for one. Using one that
		# the host already has is not a job repairing host state: nothing is
		# installed, and the outcome is the virtualenv either way. A host with none
		# of the three gets the package to install, which is the case a job cannot
		# fix for itself.
		local builder=()
		if python3 -c 'import ensurepip' 2>/dev/null; then
			builder=(python3 -m venv)
		elif command -v virtualenv >/dev/null 2>&1; then
			builder=(virtualenv --python python3)
		elif command -v uv >/dev/null 2>&1; then
			# --seed: uv leaves pip out of a virtualenv by default, and the pip
			# install below is what fills this one.
			builder=(uv venv --seed --python python3)
		else
			echo "This host's python3 cannot create virtualenvs (no ensurepip)," >&2
			echo "and it has neither virtualenv nor uv to do it instead." >&2
			echo "Install it once on the runner: sudo apt-get install -y python3-venv" >&2
			return 1
		fi
		echo "Building it with: ${builder[*]}"
		rm -rf "${acceptance_venv}"
		"${builder[@]}" "${acceptance_venv}"
	fi
	"${venv_python}" -m pip install --disable-pip-version-check \
		-r "${acceptance_dir}/requirements.txt"
	requirements_sha >"$(requirements_stamp)"
	echo "Provisioned ${acceptance_venv}"
}

# The account the framework logs in as, and the key it authenticates with.
#
# The suite reaches even the host it runs on over SSH: gen_config.py writes
# `ip_address: 127.0.0.1` with `connection_type: SSHConnection`, and mfd_connect
# opens a paramiko session to it before the first case. So sshd, this key and
# this account's authorized_keys are part of every runner's contract, not just
# the perf pair's. Both are derived here rather than at each use, so the check
# below cannot look at a different key than the config names.
acceptance_user() { echo "${RUNNER_USERNAME:-$(id -un)}"; }

# The account's real home rather than /home/<user>: $USER is unset in a
# non-interactive shell, and a runner account does not have to live under /home.
acceptance_home() {
	local home
	home=$(getent passwd "$(acceptance_user)" | cut -d: -f6)
	echo "${home:-${HOME}}"
}

acceptance_key() {
	# A host that keeps its key elsewhere sets RUNNER_SSH_KEY.
	echo "${RUNNER_SSH_KEY:-$(acceptance_home)/.ssh/id_ed25519}"
}

# What actually authorises the key, printed whenever the login fails.
#
# Not `ssh-copy-id`: it logs in before it copies, and these hosts offer publickey
# only -- which is the failure being reported -- so on the host that needs it it
# cannot get in to do the work. The DUT here is the runner itself and the account
# is its own, so appending the line is the whole act and it needs no login. Two
# rounds of fleet work were spent on the advice that could not run.
authorise_key_hint() {
	local key=$1 home
	home=$(acceptance_home)
	echo "Authorise it once -- same account, same host, so this needs no login:" >&2
	echo "  install -d -m 700 ${home}/.ssh" >&2
	echo "  cat ${key}.pub >> ${home}/.ssh/authorized_keys" >&2
	echo "  chmod 600 ${home}/.ssh/authorized_keys" >&2
	echo "sshd ignores the file when ${home} or ${home}/.ssh is group-writable (StrictModes)." >&2
}

# Prove that login before the suite tries it.
#
# paramiko is handed the key *and* the empty password gen_config.py always
# writes, and it tries the password last -- so a key it cannot use is reported as
# the password being refused, `BadAuthenticationType: allowed types:
# ['publickey']`, two minutes into the job and naming the wrong thing. One second
# of ssh(1) here says which of the three it is, in its own step.
verify_self_login() {
	local user key output
	user=$(acceptance_user)
	key=$(acceptance_key)
	if [[ ! -r ${key} ]]; then
		echo "No readable SSH key at ${key}; the generated config tells the framework to use it." >&2
		echo "Create it on the host once with: ssh-keygen -t ed25519 -N '' -f ${key}" >&2
		authorise_key_hint "${key}"
		echo "A host that keeps its key elsewhere sets RUNNER_SSH_KEY in ${runner_env}." >&2
		return 1
	fi
	# BatchMode so a host that would prompt fails instead of hanging, and a
	# throwaway known_hosts because a check must not change the host it checks.
	if ! output=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
		-i "${key}" "${user}@127.0.0.1" true 2>&1); then
		echo "The login the suite makes does not work: ssh -i ${key} ${user}@127.0.0.1" >&2
		printf '%s\n' "${output}" >&2
		authorise_key_hint "${key}"
		return 1
	fi
	echo "SSH to 127.0.0.1 as ${user} with ${key} works, so the framework can reach this host."
}

case "${1:-}" in
verify)
	# The pure check, for the provisioning workflow and for anyone asking
	# whether a host is ready. It installs nothing and fails with the command
	# that provisions.
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
	# The same check on its own, so a test job spends a second on it in a named
	# step instead of finding out inside the first case.
	load_runner_env
	verify_self_login
	;;
ensure)
	# What the test jobs run. "Jobs install nothing" is about host state -- apt
	# packages, kernel modules, DMA bindings, the media share -- where a job that
	# repairs what it finds hides drift in the host image and races with every
	# other job on the machine. This virtualenv is not host state: it is built
	# from requirements.txt in this checkout, it lives in the runner user's cache
	# outside anything git touches, and it is the same for every job on the host.
	# So it is treated as a cache. It is created once per host, refreshed when
	# the requirements change, and every one of those events is a line in the log
	# rather than a silent repair -- while a host that has one already, which is
	# every host after the first run, spends a second on `pytest --version`.
	if [[ ! -x ${venv_python} ]]; then
		provision_acceptance_venv "no virtualenv at this path yet"
	elif ! venv_usable; then
		provision_acceptance_venv "its interpreter no longer runs, so this host's python has moved"
	elif [[ $(cat "$(requirements_stamp)" 2>/dev/null) != "$(requirements_sha)" ]]; then
		provision_acceptance_venv "requirements.txt differs from the one it was built from"
	fi
	"${venv_python}" -m pytest --version
	"${venv_python}" -m pip check || true
	;;
install)
	# Provisioning by hand, on a developer machine or a runner being rebuilt,
	# and what the Provision runner workflow calls.
	provision_acceptance_venv "requested explicitly"
	;;
workspace)
	prepare_workspace
	;;
session)
	: "${RUNNER_NAME:?RUNNER_NAME is required}"
	printf 'SESSION_ID=%s\n' "${RUNNER_NAME##*-}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
nic-ids)
	# The label table on its own, for tooling that has to check a NIC label
	# without hardware in front of it. Keeping it here means there is one list
	# of labels in the repository, not two.
	nic_device_ids "${2:?NIC label is required}" ||
		{ echo "Unsupported NIC: ${2}" >&2 && exit 1; }
	;;
nic-labels)
	# Every label the table knows, for tooling that offers a choice rather than
	# checking one -- the MCP server's nic argument. Derived from the table
	# itself so a new card is added in one place.
	sed -n '/^nic_device_ids()/,/^}/p' "${BASH_SOURCE[0]}" |
		sed -n 's/^\t\([a-z0-9 |]*\)) echo .*/\1/p' | tr -d ' ' | tr '|' '\n'
	;;
pci)
	read -r pci_device interface_type < <(resolve_nic "${NIC:?NIC is required}")
	{
		printf 'PCI_DEVICE=%s\n' "$pci_device"
		printf 'INTERFACE_TYPE=%s\n' "$interface_type"
	} >>"${GITHUB_ENV:?GITHUB_ENV is required}"
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
		--username "$(acceptance_user)"
		--key_path "$(acceptance_key)"
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
		--key_path "$(acceptance_key)" \
		--test_time "${TEST_TIME:-120}" --no_capture)
	;;
tag)
	: "${WORKFLOW_TAG:?WORKFLOW_TAG is required}"
	printf 'MTL_GITHUB_WORKFLOW=%s\n' "$WORKFLOW_TAG" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
*)
	echo "Usage: $0 {verify|connection|ensure|install|workspace|session|nic-ids|nic-labels|pci|pci-env|config-single|config-perf|tag}" >&2
	exit 2
	;;
esac
