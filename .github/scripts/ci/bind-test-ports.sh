#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Prepares this host's NIC for the gtest suite: trusted VFs on one ICE PF and
# two DMA channels beside it, all bound to vfio-pci.
#
# This is the only step of a gtest job that changes NIC state. The suite reads
# what this leaves behind and runs the tests; it does not rebuild a NIC under
# itself, because a card that is rebuilt mid-suite is how a bare-metal runner
# ends up wedged for hours.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

# dpdk-devbind.py ships with the DPDK build, which on a test host is a restored
# cache and not an install into /usr. sudo replaces PATH, and nicctl.sh needs
# the tool as much as this script does, so put it back.
if [ -d "${root_dir}/.local_install/dpdk/bin" ]; then
	export PATH="${root_dir}/.local_install/dpdk/bin:${PATH}"
fi

: "${MIN_VFIO_PORTS:=4}"    # Ports of one PF the suite runs on
: "${VF_COUNT:=6}"          # VFs to create; the suite uses MIN_VFIO_PORTS of them
: "${DMA_CHANNELS:=2}"      # DMA channels to serve when the host has them
: "${MIN_HUGEPAGES:=2048}"  # 2 MB pages: 4 GiB, what one case's EAL reserves
: "${HOST_OP_TIMEOUT:=180}" # Hard bound for one NIC operation
: "${HOST_FAULT_EXIT:=3}"   # "the host needs recovery", not a test failure
# Host state the contract tests point at a fixture instead of the live kernel.
: "${SYSFS_PCI_DEVICES:=/sys/bus/pci/devices}"
: "${SYSFS_HUGEPAGES:=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages}"
: "${SYSFS_VFIO_DENYLIST:=/sys/module/vfio_pci/parameters/disable_denylist}"

work_dir=$(mktemp -d)
trap 'rm -rf "${work_dir}"' EXIT
ports="${work_dir}/ports"
dma="${work_dir}/dma"

log_error() {
	echo "$*" >&2
}

# A faulted ICE driver answers nothing and cannot be waited out: the processes
# that ask it questions go into uninterruptible sleep, where not even SIGKILL
# reclaims them. Every command below asks that driver something, so every
# command gets a bound. Without one, a wedged card holds a fleet runner for
# GitHub's 360-minute default.
bounded() {
	local label=$1 retval=0
	shift
	timeout --foreground --signal=SIGTERM --kill-after=30 "${HOST_OP_TIMEOUT}" "$@" || retval=$?
	if [ "${retval}" -eq 124 ] || [ "${retval}" -eq 137 ]; then
		log_error "host fault: ${label} did not answer within ${HOST_OP_TIMEOUT}s"
		log_error "The card has to be recovered before it can be prepared again:"
		log_error "  echo 1 | sudo tee /sys/bus/pci/devices/<pf-bdf>/remove"
		log_error "  echo 1 | sudo tee /sys/bus/pci/rescan"
		exit "${HOST_FAULT_EXIT}"
	fi
	return "${retval}"
}

# The DMA channels of one NUMA node in one state, or of every node when no node
# is named. A channel is "bound" when it is already on vfio-pci, "free" when no
# driver holds it, and "kernel" when one does.
dma_channels() {
	awk -v want_numa="${1:-}" -v want_state="$2" \
		'$1 !~ /^[0-9a-f]+:[0-9a-f]+:[0-9a-f]+\.[0-9a-f]+$/ {next}
		want_numa != "" && $0 !~ ("numa_node=" want_numa "([^0-9]|$)") {next}
		{state = ($0 ~ /drv=vfio-pci/) ? "bound" : (($0 ~ /drv=/) ? "kernel" : "free")}
		state == want_state {print $1}' "${dma}"
}

# The channels to run on, cheapest to take first, and fewer than DMA_CHANNELS
# when the host has fewer.
#
# A channel already on vfio-pci is free of charge, so a prepared host is not
# touched at all. A channel with no driver needs a bind. A channel a kernel
# driver holds needs an unbind first, which is one dpdk-devbind.py call either
# way: idxd releases a DSA device on request, and `bounded` catches the driver
# that does not. That last tier is why a gtest leg no longer stops here -- every
# DSA device on a stock host comes up on idxd, so demanding that a human take
# them away at boot meant demanding it of every host in the fleet.
channels_for() {
	local numa=$1
	{
		dma_channels "${numa}" bound
		dma_channels "${numa}" free
		dma_channels "${numa}" kernel
	} | head -n "${DMA_CHANNELS}"
}

# vfio-pci carries a denylist of devices it will not probe, and Intel DSA
# (8086:0b25) is on it -- a DSA device bound to it without disable_denylist=1
# never appears as a dmadev. The parameter is 0444 once the module is loaded, so
# it cannot be turned on in place, but the module reloads in place: nothing holds
# it open between jobs, and the VFs and channels this script binds are all bound
# after this point. That is the whole reason this is a step of the job now and
# not a modprobe.d file with a reboot behind it.
vfio_pci_loaded() {
	lsmod | awk '{print $1}' | grep -qx vfio_pci
}

vfio_pci_allows_dsa() {
	[ -r "${SYSFS_VFIO_DENYLIST}" ] && [ "$(cat "${SYSFS_VFIO_DENYLIST}")" = Y ]
}

# `|| modprobe vfio-pci` for a kernel whose vfio-pci has no such parameter:
# modprobe refuses the whole load over an unknown one, and a host with no DSA
# device needs the module all the same.
load_vfio_pci() {
	modprobe vfio-pci disable_denylist=1 2>/dev/null || modprobe vfio-pci
}

allow_dsa_probe() {
	vfio_pci_allows_dsa && return 0
	echo "vfio-pci was loaded with its denylist on, which hides Intel DSA; reloading it"
	modprobe -r vfio-pci || {
		log_error "could not unload vfio-pci: something on this host is holding it."
		log_error "Then it takes a boot to allow DSA, see doc/dma.md:"
		log_error "  echo 'options vfio-pci disable_denylist=1' | sudo tee /etc/modprobe.d/vfio-pci.conf"
		return 1
	}
	load_vfio_pci
}

# Hugepages, the other thing every process the suite starts needs and no job
# reserved. Without them EAL stops on "Cannot get hugepage information" inside
# the first case, which reads as a broken build rather than as a host that has
# been rebooted since it was last set up. Raised, never lowered: a host may have
# reserved more for something else, and this suite is not the one to take them.
ensure_hugepages() {
	local have
	if [ ! -w "${SYSFS_HUGEPAGES}" ]; then
		log_error "no 2 MB hugepage pool at ${SYSFS_HUGEPAGES}; every case's EAL needs one."
		return 0
	fi
	have=$(cat "${SYSFS_HUGEPAGES}")
	if [ "${have}" -ge "${MIN_HUGEPAGES}" ]; then
		echo "Hugepages: ${have} x 2 MB reserved, ${MIN_HUGEPAGES} needed"
		return 0
	fi
	echo "Reserving ${MIN_HUGEPAGES} x 2 MB hugepages (this host had ${have})"
	echo "${MIN_HUGEPAGES}" >"${SYSFS_HUGEPAGES}"
	have=$(cat "${SYSFS_HUGEPAGES}")
	if [ "${have}" -lt "${MIN_HUGEPAGES}" ]; then
		log_error "the kernel served ${have} of ${MIN_HUGEPAGES} hugepages, so memory is fragmented."
		log_error "EAL takes what there is; free memory or reboot the host if a case fails on it."
	fi
	if ! grep -q hugetlbfs /proc/mounts; then
		log_error "no hugetlbfs mounted; EAL looks for one at /dev/hugepages:"
		log_error "  sudo mkdir -p /dev/hugepages && sudo mount -t hugetlbfs nodev /dev/hugepages"
	fi
}

# Fewer channels than the suite would like is a smaller suite, not a failed leg.
#
# Every case that copies with DMA asks the library for a channel first
# (st_test_dma_available) and reports itself skipped when there is none, so the
# alternative to running without them is running nothing at all -- which is what
# this step did to three gtest legs a round, on every host in the fleet, for
# something no non-DMA case needs. A host that is meant to serve channels says
# so with MTL_CI_REQUIRE_DMA=1 and gets a failure instead.
report_dma_shortfall() {
	local found=$1
	cat "${dma}"
	log_error "This host serves ${found} DMA channel(s) on NUMA node ${numa}, where the ports are,"
	log_error "and the suite would use ${DMA_CHANNELS}. A channel on another node does not count:"
	log_error "the library only pairs a session with a channel of the port's own socket."
	log_error "The suite runs without DMA offload: its DMA cases ask the library for a channel"
	log_error "and report themselves skipped when there is none. Every other case is unaffected."
	if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
		echo "No DMA offload in gtest: $(hostname) serves ${found} of ${DMA_CHANNELS} channels." \
			>>"${GITHUB_STEP_SUMMARY}"
	fi
	if [ "${MTL_CI_REQUIRE_DMA:-0}" = 1 ]; then
		log_error "MTL_CI_REQUIRE_DMA=1 on this host, so this is a failure."
		log_error "doc/dma.md says how a host serves a channel; a platform that lists no DMA"
		log_error "device at all needs its DSA or CBDMA engines enabled in the BIOS first."
		exit 1
	fi
}

# Whether any of the chosen channels still has to be taken -- reading the
# ${channels} the main flow selected, as bound_vf_count reads ${pf}.
needs_bind() {
	local channel
	for channel in "${channels[@]}"; do
		dma_channels "" bound | grep -qFx "${channel}" || return 0
	done
	return 1
}

# The VFs of the chosen PF that are on vfio-pci. Read from sysfs rather than
# from a listing, because this runs right after creating them.
bound_vf_count() {
	local virtfn count=0
	for virtfn in "${SYSFS_PCI_DEVICES}/${pf}/virtfn"*; do
		[ -e "${virtfn}" ] || continue
		[ "$(basename "$(readlink -f "${virtfn}/driver" 2>/dev/null)")" = vfio-pci ] || continue
		count=$((count + 1))
	done
	echo "${count}"
}

[ "$(id -u)" -eq 0 ] || {
	log_error "preparing the NIC needs root: sudo task ci:bind-test-ports"
	exit 1
}
command -v dpdk-devbind.py >/dev/null || {
	log_error "dpdk-devbind.py not found: build DPDK with 'task ci:build', or install it with"
	log_error "  python3 -m pip install --user dpdk-devbind"
	exit 1
}
lsmod | awk '{print $1}' | grep -qx ice || {
	log_error "the ice driver is not loaded: sudo task ci:activate-ice"
	exit 1
}
vfio_pci_loaded || load_vfio_pci
ensure_hugepages

bounded "nicctl.sh list up" "${root_dir}/script/nicctl.sh" list up >"${ports}"
bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma >"${dma}"

# The PF to run on: an ice PF whose link is up, preferring one with two DMA
# channels on its own NUMA node, because that is the only kind this suite can be
# given -- the library grants a session a channel of the port's socket and no
# other (doc/dma.md, mt_dma_request_dev).
pf=""
numa=""
while read -r candidate candidate_numa; do
	if [ -z "${pf}" ]; then
		pf=${candidate}
		numa=${candidate_numa}
	fi
	if [ "$(channels_for "${candidate_numa}" | wc -l)" -ge 2 ]; then
		pf=${candidate}
		numa=${candidate_numa}
		break
	fi
done < <(awk '$3 == "ice" {print $2, $4}' "${ports}")

if [ -z "${pf}" ]; then
	cat "${ports}"
	log_error "no ice PF has its link up, and the suite needs one to run on."
	log_error "Load the driver with 'sudo task ci:activate-ice' and connect the port;"
	log_error "'${root_dir}/script/nicctl.sh list all' shows what this host has."
	exit 1
fi

# Channels of the PF's own node, and no others. A channel on another node is not
# a slower channel, it is an unusable one: mt_dma_request_dev only ever pairs a
# session with a channel whose socket matches the port's, so one from elsewhere
# registers, is counted by the suite's st_test_dma_available -- and is then never
# granted. Serving one turns every DMA case into a case running without the
# offload it exists to test rather than a case that skips itself.
mapfile -t channels < <(channels_for "${numa}")
if [ "${#channels[@]}" -lt "${DMA_CHANNELS}" ]; then
	report_dma_shortfall "${#channels[@]}"
fi

# The denylist only matters for a channel that is not on vfio-pci yet, and the
# reload has to happen before the VFs are created rather than after: it drops
# every device the module holds, and takes the channels it held with it -- hence
# the fresh listing, which the bind loop below reads.
if needs_bind && ! vfio_pci_allows_dsa; then
	if allow_dsa_probe; then
		bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma >"${dma}"
	fi
fi

echo "Preparing ${pf} (NUMA ${numa}) with ${VF_COUNT} trusted VFs"
bounded "nicctl.sh create_tvf ${pf}" \
	"${root_dir}/script/nicctl.sh" create_tvf "${pf}" "${VF_COUNT}" || {
	log_error "nicctl.sh create_tvf ${pf} failed; the listing above says what state it left"
	exit 1
}

served=()
for channel in "${channels[@]}"; do
	if dma_channels "" bound | grep -qFx "${channel}"; then
		echo "DMA channel ${channel} is already on vfio-pci"
		served+=("${channel}")
		continue
	fi
	echo "Binding DMA channel ${channel} to vfio-pci"
	# A channel that will not bind costs the DMA cases and nothing else, so it is
	# reported and dropped rather than failing the leg -- same reasoning as the
	# shortfall above, and the same MTL_CI_REQUIRE_DMA=1 to make it a failure.
	if bounded "dpdk-devbind.py -b vfio-pci ${channel}" \
		dpdk-devbind.py -b vfio-pci "${channel}"; then
		served+=("${channel}")
	else
		log_error "could not bind ${channel} to vfio-pci; the listing below says who holds it."
		log_error "vfio-pci never probes Intel DSA (8086:0b25) while its denylist is on, and this"
		log_error "job reloads the module to turn it off -- doc/dma.md has the boot-time version"
		log_error "for a host where something else keeps the module loaded."
	fi
done
if [ "${#served[@]}" -lt "${DMA_CHANNELS}" ] && [ "${#served[@]}" -lt "${#channels[@]}" ]; then
	report_dma_shortfall "${#served[@]}"
fi

bound=$(bound_vf_count)
if [ "${bound}" -lt "${MIN_VFIO_PORTS}" ]; then
	log_error "${pf} came back with ${bound} vfio-pci VF(s), and the suite needs ${MIN_VFIO_PORTS}."
	log_error "A VF that does not bind is usually a missing IOMMU: check that the kernel"
	log_error "command line has intel_iommu=on iommu=pt and that VT-d is enabled in the BIOS."
	exit 1
fi

bounded "nicctl.sh list all" "${root_dir}/script/nicctl.sh" list all
bounded "dpdk-devbind.py --status-dev dma" dpdk-devbind.py --status-dev dma
echo "Prepared ${pf}: ${bound} vfio-pci VFs, DMA channels: ${served[*]:-none}"
