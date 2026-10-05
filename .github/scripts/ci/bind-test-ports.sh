#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# The only gtest step that changes NIC state; doc/ci_runner_setup.md says why.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
nicctl="${root_dir}/script/nicctl.sh"
[ ! -d "${root_dir}/.local_install/dpdk/bin" ] || export PATH="${root_dir}/.local_install/dpdk/bin:${PATH}"

: "${MIN_VFIO_PORTS:=4}"
: "${VF_COUNT:=6}"
: "${SIBLING_VF_COUNT:=2}"
: "${DMA_CHANNELS:=2}"
: "${MIN_HUGEPAGES:=2048}"
: "${HOST_OP_TIMEOUT:=180}"

work_dir=$(mktemp -d)
trap 'rm -rf "${work_dir}"' EXIT
ports="${work_dir}/ports"
dma="${work_dir}/dma"

err() {
	printf '%s\n' "$@" >&2
}

# A faulted ICE driver hangs its callers in uninterruptible sleep, so every NIC call is bounded.
bounded() {
	local label=$1 retval=0
	shift
	timeout --foreground --signal=SIGTERM --kill-after=30 "${HOST_OP_TIMEOUT}" "$@" || retval=$?
	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		err "host fault: ${label} did not answer within ${HOST_OP_TIMEOUT}s" \
			"The card has to be recovered before it can be prepared again:" \
			"  echo 1 | sudo tee /sys/bus/pci/devices/<pf-bdf>/remove" \
			"  echo 1 | sudo tee /sys/bus/pci/rescan"
		exit 3
	fi
	return "${retval}"
}

module_loaded() {
	lsmod | awk '{print $1}' | grep -qx "$1"
}

# Without disable_denylist=1, vfio-pci never probes Intel DSA.
load_vfio_pci() {
	modprobe vfio-pci disable_denylist=1 2>/dev/null || modprobe vfio-pci
}

# DMA channels on NUMA node $1 (any node if empty) in state $2: bound (vfio-pci), free or kernel.
dma_channels() {
	awk -v want_numa="${1:-}" -v want_state="$2" \
		'$1 !~ /^[0-9a-f]+:[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+$/ {next}
		want_numa != "" && $0 !~ ("numa_node=" want_numa "([^0-9]|$)") {next}
		{state = ($0 ~ /drv=vfio-pci/) ? "bound" : (($0 ~ /drv=/) ? "kernel" : "free")}
		state == want_state {print $1}' "${dma}"
}

channels_for() {
	{
		dma_channels "$1" bound
		dma_channels "$1" free
		dma_channels "$1" kernel
	} | head -n "${DMA_CHANNELS}"
}

channel_bound() {
	dma_channels "" bound | grep -qFx "$1"
}

needs_bind() {
	local channel
	for channel in "${channels[@]}"; do
		channel_bound "${channel}" || return 0
	done
	return 1
}

report_dma_shortfall() {
	cat "${dma}"
	err "This host serves $1 DMA channel(s) on NUMA node ${numa}, where the ports are," \
		"and the suite would use ${DMA_CHANNELS}; a channel on another node does not count." \
		"The suite runs without DMA offload: its DMA cases skip themselves."
	if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
		echo "No DMA offload in gtest: $(hostname) serves $1 of ${DMA_CHANNELS} channels." >>"${GITHUB_STEP_SUMMARY}"
	fi
	if [ "${MTL_CI_REQUIRE_DMA:-0}" = 1 ]; then
		err "MTL_CI_REQUIRE_DMA=1 on this host, so this is a failure; see doc/dma.md." \
			"A platform that lists no DMA device needs its DSA or CBDMA engines enabled in the BIOS."
		exit 1
	fi
}

bound_vf_count() {
	local driver count=0
	for driver in "/sys/bus/pci/devices/$1"/virtfn*/driver; do
		[ "$(basename "$(readlink -f "${driver}" 2>/dev/null)")" != vfio-pci ] || count=$((count + 1))
	done
	echo "${count}"
}

[ "$(id -u)" -eq 0 ] || {
	err "preparing the NIC needs root: sudo task ci:bind-test-ports"
	exit 1
}
command -v dpdk-devbind.py >/dev/null || {
	err "dpdk-devbind.py not found: build DPDK with 'script/build_dpdk.sh', or install it with" \
		"  python3 -m pip install --user dpdk-devbind"
	exit 1
}
module_loaded ice || {
	err "the ice driver is not loaded: sudo task ci:activate-ice"
	exit 1
}
module_loaded vfio_pci || load_vfio_pci

# Per NUMA node, because EAL allocates a port's pools on the port's node. Raised, never lowered.
for pool in /sys/devices/system/node/node[0-9]*/hugepages/hugepages-2048kB/nr_hugepages; do
	node=${pool#/sys/devices/system/node/}
	node=${node%%/*}
	if [ ! -w "${pool}" ]; then
		err "no 2 MB hugepage pool at ${pool}; every case's EAL needs one."
		continue
	fi
	have=$(cat "${pool}")
	if [ "${have}" -ge "${MIN_HUGEPAGES}" ]; then
		echo "Hugepages: ${have} x 2 MB reserved on ${node}, ${MIN_HUGEPAGES} needed"
		continue
	fi
	echo "Reserving ${MIN_HUGEPAGES} x 2 MB hugepages on ${node} (it had ${have})"
	echo "${MIN_HUGEPAGES}" >"${pool}"
	have=$(cat "${pool}")
	[ "${have}" -ge "${MIN_HUGEPAGES}" ] ||
		err "the kernel served ${have} of ${MIN_HUGEPAGES} hugepages on ${node}, so memory is fragmented." \
			"EAL takes what there is; free memory or reboot the host if a case fails on it."
done
grep -q hugetlbfs /proc/mounts ||
	err "no hugetlbfs mounted; EAL looks for one at /dev/hugepages:" \
		"  sudo mkdir -p /dev/hugepages && sudo mount -t hugetlbfs nodev /dev/hugepages"

bounded "nicctl.sh list up" "${nicctl}" list up >"${ports}"
bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma >"${dma}"

# An ice PF with link up, preferring one with two DMA channels on its own node:
# the library only grants a session a channel of the port's socket.
pf=""
numa=""
while read -r candidate candidate_numa; do
	[ -n "${pf}" ] || {
		pf=${candidate}
		numa=${candidate_numa}
	}
	if [ "$(channels_for "${candidate_numa}" | wc -l)" -ge 2 ]; then
		pf=${candidate}
		numa=${candidate_numa}
		break
	fi
done < <(awk '$3 == "ice" {print $2, $4}' "${ports}")

if [ -z "${pf}" ]; then
	cat "${ports}"
	err "no ice PF has its link up, and the suite needs one to run on." \
		"Load the driver with 'sudo task ci:activate-ice' and connect the port;" \
		"'${nicctl} list all' shows what this host has."
	exit 1
fi

mapfile -t channels < <(channels_for "${numa}")
[ "${#channels[@]}" -ge "${DMA_CHANNELS}" ] || report_dma_shortfall "${#channels[@]}"

# Reloading vfio-pci drops every device it holds, so it goes before the VFs and the DMA list is re-read.
if needs_bind && [ "$(cat /sys/module/vfio_pci/parameters/disable_denylist 2>/dev/null)" != Y ]; then
	echo "vfio-pci was loaded with its denylist on, which hides Intel DSA; reloading it"
	if modprobe -r vfio-pci; then
		load_vfio_pci && bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma >"${dma}"
	else
		err "could not unload vfio-pci: something on this host is holding it." \
			"Then it takes a boot to allow DSA, see doc/dma.md:" \
			"  echo 'options vfio-pci disable_denylist=1' | sudo tee /etc/modprobe.d/vfio-pci.conf"
	fi
fi

echo "Preparing ${pf} (NUMA ${numa}) with ${VF_COUNT} trusted VFs"
bounded "nicctl.sh create_tvf ${pf}" "${nicctl}" create_tvf "${pf}" "${VF_COUNT}" || {
	err "nicctl.sh create_tvf ${pf} failed; the listing above says what state it left"
	exit 1
}

# NoCtx strict pacing needs RX on another port of this card: VF-to-VF traffic on one PF gets no RX timestamp.
sibling=$(awk -v pf="${pf}" -v dev="${pf%.*}." \
	'$3 == "ice" && $2 != pf && index($2, dev) == 1 {print $2; exit}' "${ports}")
if [ -z "${sibling}" ]; then
	err "no other port of ${pf}'s card has its link up: the NoCtx strict pacing cases will fail"
else
	echo "Preparing ${sibling} with ${SIBLING_VF_COUNT} trusted VFs for NoCtx"
	bounded "nicctl.sh create_tvf ${sibling}" "${nicctl}" create_tvf "${sibling}" "${SIBLING_VF_COUNT}" ||
		err "nicctl.sh create_tvf ${sibling} failed: the NoCtx strict pacing cases will fail"
fi

served=()
for channel in "${channels[@]}"; do
	if channel_bound "${channel}"; then
		echo "DMA channel ${channel} is already on vfio-pci"
		served+=("${channel}")
		continue
	fi
	echo "Binding DMA channel ${channel} to vfio-pci"
	if bounded "dpdk-devbind.py -b vfio-pci ${channel}" dpdk-devbind.py -b vfio-pci "${channel}"; then
		served+=("${channel}")
	else
		err "could not bind ${channel} to vfio-pci; the listing below says who holds it." \
			"vfio-pci never probes Intel DSA (8086:0b25) while its denylist is on; see doc/dma.md."
	fi
done
if [ "${#served[@]}" -lt "${DMA_CHANNELS}" ] && [ "${#served[@]}" -lt "${#channels[@]}" ]; then
	report_dma_shortfall "${#served[@]}"
fi

bound=$(bound_vf_count "${pf}")
if [ "${bound}" -lt "${MIN_VFIO_PORTS}" ]; then
	err "${pf} came back with ${bound} vfio-pci VF(s), and the suite needs ${MIN_VFIO_PORTS}." \
		"A VF that does not bind is usually a missing IOMMU: check that the kernel" \
		"command line has intel_iommu=on iommu=pt and that VT-d is enabled in the BIOS."
	exit 1
fi

bounded "nicctl.sh list all" "${nicctl}" list all
bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma
echo "Prepared ${pf}: ${bound} vfio-pci VFs, DMA channels: ${served[*]:-none}"
if [ -n "${sibling}" ]; then
	bound=$(bound_vf_count "${sibling}")
	echo "Prepared ${sibling} for NoCtx: ${bound} vfio-pci VFs"
	[ "${bound}" -ge "${SIBLING_VF_COUNT}" ] ||
		err "${sibling} came back with ${bound} vfio-pci VF(s), and NoCtx needs ${SIBLING_VF_COUNT}."
fi
