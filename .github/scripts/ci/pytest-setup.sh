#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
acceptance_dir="${root_dir}/tests/acceptance"
# shellcheck source-path=SCRIPTDIR source=../lib/mtl_acceptance_venv.sh disable=SC1091
. "${root_dir}/.github/scripts/lib/mtl_acceptance_venv.sh"

# dpdk-devbind.py ships with the DPDK build, which on a test host is a restored
# cache and not an install into /usr, and this script runs under sudo, which
# replaces PATH even with -E -- same reasoning and same fix as
# bind-test-ports.sh's identical block, and the same bug 921aa1b1 already fixed
# once in mtl_engine/dma.py for perf-pytest.
if [ -d "${root_dir}/.local_install/dpdk/bin" ]; then
	export PATH="${root_dir}/.local_install/dpdk/bin:${PATH}"
fi

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
	lspci -Dn -d "${1}" 2>/dev/null | wc -l
}

# The same list resolve_nic prints, for a host that names its card instead of
# carrying a NIC label -- given a BDF or a vendor:device.
#
# The lab file names one PF, because that is all a non-redundant test needs. But
# the ST2022-7 cases need a second interface to put their second leg on, and
# gen_config.py numbers interface_index within a vendor:device group, so a single
# entry means a single interface: conftest.py then has no port to build
# host.vfs_r on and every redundant case skips with "Redundant requires VFs on TX
# port 1". Ask the card how many ports it has rather than asking the lab file to
# name each one, so the fleet's existing lab files keep working.
perf_card_ports() {
	local declared=$1 vendor_device ports
	# Already a list: whoever wrote it meant it.
	if [[ ${declared} == *,* ]]; then
		printf '%s\n' "${declared}"
		return 0
	fi
	# resolve_nic can carry on without lspci because a label already names the
	# card. Here the card is what we are trying to learn, so there is nothing to
	# fall back to -- and guessing would only move the failure into gen_config.py,
	# which resolves the same BDF with the same tool.
	if ! command -v lspci >/dev/null 2>&1; then
		echo "lspci is needed to resolve the perf card (${declared}); install pciutils." >&2
		return 1
	fi
	if [[ ${declared} == *.* ]]; then
		vendor_device=$(lspci -Dn -s "${declared}" 2>/dev/null | awk 'NR==1 {print $3}')
	else
		vendor_device=${declared}
	fi
	# An unresolved BDF leaves this empty, and `lspci -d ""` matches every device.
	if [[ -n ${vendor_device} ]]; then
		ports=$(count_pci_functions "${vendor_device}")
	else
		ports=0
	fi
	if [[ ${ports} -lt 1 ]]; then
		echo "No port of the declared perf card (${declared}) is on this host:" >&2
		lspci -Dnn -d '::0200' >&2 || true
		return 1
	fi
	# Capped at two for the reason resolve_nic caps it, and because ST2022-7 has
	# two legs: a third entry would only make VFs on a port nothing binds.
	echo "Resolved perf card ${vendor_device} (${ports} ports)" >&2
	if [[ ${ports} -ge 2 ]]; then
		printf '%s,%s\n' "${vendor_device}" "${vendor_device}"
	else
		printf '%s\n' "${vendor_device}"
	fi
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
		ports=$(count_pci_functions "8086:${candidate}")
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

# The ceiling on what a socket may ask to buffer, which the kernel socket
# datapath needs raised.
#
# rx_socket_init_fd() sizes every RX socket itself, asking for 4 MiB. It asks
# with SO_RCVBUFFORCE first, which ignores this ceiling -- but that needs
# CAP_NET_ADMIN, and the suite's apps reach the host over SSH as an ordinary
# user, so the ask falls back to SO_RCVBUF and the ceiling caps it. The symptom
# and the numbers: see doc/kernel_socket.md.
#
# The kernel clamps the ask against this ceiling before doubling it, so the
# ceiling has to be the size of the ask rather than twice it. A ceiling is not an
# allocation: a socket that does not ask still gets net.core.rmem_default, so
# this costs nothing on a leg that never opens one. Raised, never lowered, so a
# host already tuned higher keeps its value.
#
# Not conditional on the leg's datapath: tests/single/kernel_socket runs
# kernel:<ifname> on every host, whatever its own card resolved to.
RMEM_MAX_MIN=$((4 * 1024 * 1024))

ensure_socket_rcvbuf_ceiling() {
	local current
	current=$(sysctl -n net.core.rmem_max)
	if ((current >= RMEM_MAX_MIN)); then
		echo "net.core.rmem_max is ${current}, enough for the kernel socket datapath"
		return 0
	fi
	sudo sysctl -q -w "net.core.rmem_max=${RMEM_MAX_MIN}"
	echo "net.core.rmem_max: raised ${current} -> ${RMEM_MAX_MIN} for the kernel socket datapath"
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

# The DMA (Intel DSA/CBDMA) channels of one NUMA node, in state ${3}: "bound"
# when already on vfio-pci, "free" when no driver holds it, "kernel" when one
# does. Reads a `dpdk-devbind.py --status-dev dma` listing already captured to
# ${1}, mirroring bind-test-ports.sh's dma_channels(), which snapshots once for
# the same reason: a fresh shell-out per query risks an inconsistent view if
# host state changes between calls, on a runner other jobs also touch.
#
# dpdk-devbind.py only prints a numa_node= field at all when the kernel reports
# one for every device in the listing -- a single-socket host gets none, ever,
# even though every device on it is trivially "node 0". Treating an absent
# field as node 0 (like get_numa_node_from_pci does on the python side, via
# max(0, numa)) is what a single-socket host needs to match anything here.
dma_channels_of() {
	local listing=$1 numa=$2 want_state=$3
	awk -v want_numa="${numa}" -v want_state="${want_state}" \
		'$1 !~ /^[0-9a-f]+:[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+$/ {next}
		want_numa != "" {
			if ($0 ~ /numa_node=[0-9]+/) {
				if ($0 !~ ("numa_node=" want_numa "([^0-9]|$)")) next
			} else if (want_numa != "0") {
				next
			}
		}
		{state = ($0 ~ /drv=vfio-pci/) ? "bound" : (($0 ~ /drv=/) ? "kernel" : "free")}
		state == want_state {print $1}' "${listing}"
}

# vfio-pci denylists Intel DSA (8086:0b25) by default, so a device bound to it
# without disable_denylist=1 never appears as a dmadev. Same quirk
# bind-test-ports.sh works around for the gtest suite; see doc/dma.md.
vfio_pci_allows_dsa() {
	local denylist=/sys/module/vfio_pci/parameters/disable_denylist
	[[ -r ${denylist} ]] && [[ $(cat "${denylist}") == Y ]]
}

allow_dsa_probe() {
	vfio_pci_allows_dsa && return 0
	echo "vfio-pci was loaded with its denylist on, which hides Intel DSA; reloading it" >&2
	sudo modprobe -r vfio-pci || {
		echo "could not unload vfio-pci: something on this host is holding it." >&2
		return 1
	}
	sudo modprobe vfio-pci disable_denylist=1 2>/dev/null || sudo modprobe vfio-pci
}

: "${DMA_CHANNELS:=2}"

# Whether any of ${channels[@]} still needs a bind, reading the ${bound[@]}
# this call's snapshot already classified -- mirrors bind-test-ports.sh's
# needs_bind(), which the denylist reload below has to match: a channel in
# "free" state (no driver, so not in ${kernel[@]} either) still needs the
# reload if vfio-pci is already loaded without disable_denylist=1, and gating
# the reload on ${#kernel[@]} alone misses exactly that channel.
any_channel_needs_bind() {
	local channel
	for channel in "${channels[@]}"; do
		printf '%s\n' "${bound[@]-}" | grep -qFx "${channel}" || return 0
	done
	return 1
}

# Logs a DMA shortfall against ${DMA_CHANNELS}, ${numa} and ${first_bdf} of the
# caller (bind_dma), and turns it into a hard failure when this host is meant
# to serve DMA (MTL_CI_REQUIRE_DMA=1) -- same escape hatch bind-test-ports.sh's
# report_dma_shortfall offers on the gtest side, and the same two checkpoints:
# bind_dma calls this once for "found fewer than wanted" and, only when
# binding itself lost more ground, again for "bound fewer than found".
dma_shortfall() {
	local found=$1
	echo "This host serves ${found}/${DMA_CHANNELS} DMA channel(s) on NUMA ${numa} (where ${first_bdf} is)." >&2
	# Unlike gtest's DMA cases, which ask st_test_dma_available() and skip
	# themselves, the nightly st20p cases this serves have no such check: they
	# run on the CPU-copy fallback and can fail validation on throughput
	# instead, on 8K/12-bit formats it cannot sustain (rv_init_dma, dma.md).
	# This step never fails the job over that by default -- MTL_CI_REQUIRE_DMA=1
	# below is how a host meant to serve DMA turns it into a loud one instead
	# of a quiet CPU-bound failure inside the suite.
	echo "The suite runs without DMA offload; cases that need it may fail on throughput instead of skipping." >&2
	if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
		echo "No DMA offload for nightly st20p: $(hostname) serves ${found}/${DMA_CHANNELS} channel(s) on NUMA ${numa}." \
			>>"${GITHUB_STEP_SUMMARY}"
	fi
	if [[ ${MTL_CI_REQUIRE_DMA:-0} == 1 ]]; then
		echo "MTL_CI_REQUIRE_DMA=1 on this host, so this is a failure." >&2
		return 1
	fi
	return 0
}

# Binds up to DMA_CHANNELS DMA devices on the NIC's own NUMA node to vfio-pci,
# so the suite's RxTxApp sessions can be given one via --dma_dev instead of
# silently falling back to a per-packet CPU copy (rv_init_dma, doc/dma.md).
# Prints the comma-joined, actually-bound channel list to stdout (empty when
# none); every log line above goes to stderr so a caller can capture it.
#
# Run explicitly, once per host, from its own workflow step -- never from
# `ensure`: DMA bindings are host state, and a job that repairs host state on
# every run hides drift in the host image and races with whatever else uses the
# card, same reasoning as bind-test-ports.sh's header for the gtest side.
#
# Never fails the job over a missing DMA device by default: refusing here
# would cost every st20p case run on this leg, not just the ones that need
# DMA to hit their throughput target. Unlike gtest's DMA cases, none of these
# ask the library for a channel and skip themselves when there is none -- they
# just run on the CPU-copy fallback and may fail validation instead (see
# dma_shortfall()). A host that is meant to serve DMA sets MTL_CI_REQUIRE_DMA=1
# to turn that into a loud, immediate failure instead -- same escape hatch
# bind-test-ports.sh's report_dma_shortfall offers on the gtest side.
bind_dma() {
	local pci_device=${1:?PCI_DEVICE is required}
	local first_id=${pci_device%%,*}
	local first_bdf numa channel served=0 listing
	local -a bound=() free=() kernel=() channels=()

	# One lspci match for the NIC's own vendor:device, same "first one with any
	# port wins" simplicity resolve_nic() already uses to pick the NIC itself
	# (pytest-setup.sh's own convention, not a lower bar than the rest of this
	# file) -- link-state-aware selection would need nicctl.sh, which has no
	# role here since VF creation for pytest happens later, inside the suite.
	first_bdf=$(lspci -Dn -d "${first_id}" 2>/dev/null | head -n1 | cut -d' ' -f1)
	if [[ -z ${first_bdf} ]]; then
		first_bdf=${first_id}
		numa=0
		echo "No PCI device found for ${first_id}; can't tell its NUMA node." >&2
		dma_shortfall 0 || return 1
		echo ""
		return 0
	fi
	numa=$(cat "/sys/bus/pci/devices/${first_bdf}/numa_node" 2>/dev/null || echo -1)
	[[ ${numa} -lt 0 ]] && numa=0

	if ! command -v dpdk-devbind.py >/dev/null 2>&1; then
		echo "dpdk-devbind.py not found." >&2
		dma_shortfall 0 || return 1
		echo ""
		return 0
	fi

	# >/dev/null on both: nothing this prints belongs on stdout, which
	# bind_dma reserves for the channel list its caller captures.
	sudo modprobe vfio-pci disable_denylist=1 >/dev/null 2>/dev/null ||
		sudo modprobe vfio-pci >/dev/null || true

	listing=$(mktemp)
	trap 'rm -f "${listing}"' RETURN
	dpdk-devbind.py --status-dev dma >"${listing}" 2>/dev/null || true

	mapfile -t bound < <(dma_channels_of "${listing}" "${numa}" bound)
	mapfile -t free < <(dma_channels_of "${listing}" "${numa}" free)
	mapfile -t kernel < <(dma_channels_of "${listing}" "${numa}" kernel)
	mapfile -t channels < <(printf '%s\n' "${bound[@]-}" "${free[@]-}" "${kernel[@]-}" |
		awk 'NF' | head -n "${DMA_CHANNELS}")

	if [[ ${#channels[@]} -lt ${DMA_CHANNELS} ]]; then
		dma_shortfall "${#channels[@]}" || return 1
	fi
	if [[ ${#channels[@]} -eq 0 ]]; then
		echo ""
		return 0
	fi

	if any_channel_needs_bind && ! vfio_pci_allows_dsa; then
		if allow_dsa_probe; then
			# The reload drops every device vfio-pci held, DSA or not, so a
			# channel this call's earlier snapshot saw as already bound may
			# need re-binding too -- refresh before the loop below trusts it.
			dpdk-devbind.py --status-dev dma >"${listing}" 2>/dev/null || true
			mapfile -t bound < <(dma_channels_of "${listing}" "${numa}" bound)
		fi
	fi

	local -a served_list=()
	for channel in "${channels[@]}"; do
		if printf '%s\n' "${bound[@]-}" | grep -qFx "${channel}"; then
			served=$((served + 1))
			served_list+=("${channel}")
			continue
		fi
		echo "Binding DMA channel ${channel} to vfio-pci" >&2
		if sudo dpdk-devbind.py -b vfio-pci "${channel}" >/dev/null; then
			served=$((served + 1))
			served_list+=("${channel}")
		else
			echo "could not bind ${channel} to vfio-pci" >&2
		fi
	done

	echo "DMA: ${served}/${#channels[@]} channel(s) on NUMA ${numa} bound to vfio-pci" >&2
	if [[ ${served} -lt ${DMA_CHANNELS} ]] && [[ ${served} -lt ${#channels[@]} ]]; then
		dma_shortfall "${served}" || return 1
	fi

	local IFS=,
	echo "${served_list[*]-}"
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
	ensure_socket_rcvbuf_ceiling
	{
		printf 'PCI_DEVICE=%s\n' "$pci_device"
		printf 'INTERFACE_TYPE=%s\n' "$interface_type"
	} >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
dma)
	: "${PCI_DEVICE:?PCI_DEVICE is required}"
	dma_device=$(bind_dma "$PCI_DEVICE")
	printf 'DMA_DEVICE=%s\n' "$dma_device" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
pci-env)
	# For runners that carry no NIC label (the perf SUT pair): the host states
	# which card the perf rig owns, an E830 is the default, and perf_card_ports
	# expands it to the ports the card actually has.
	load_runner_env
	# Assigned rather than substituted straight into printf: set -e only aborts
	# on a failing command substitution when it is the whole assignment, and a
	# card the host does not have has to stop the job, not export an empty list.
	pci_device=$(perf_card_ports "${PCI_DEVICE:-${PERF_PCI_DEVICE:-8086:12d2}}")
	printf 'PCI_DEVICE=%s\n' "$pci_device" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
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
	if [[ -n ${DMA_DEVICE:-} ]]; then
		args+=(--dma_device "$DMA_DEVICE")
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
	echo "Usage: $0 {verify|connection|ensure|install|workspace|session|nic-ids|nic-labels|pci|dma|pci-env|config-single|config-perf|tag}" >&2
	exit 2
	;;
esac
