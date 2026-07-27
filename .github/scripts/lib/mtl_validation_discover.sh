#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Read-only discovery helpers for preparing a host to run
# tests/validation/tests/single/ pytest.
#
# This file defines no side effects — every vd_* function only reads system
# state (sysfs/procfs/pkg-config/etc.) and either prints a report or returns
# data on stdout for a caller to parse. It is meant to be `source`d by:
#   - validation_setup.sh (the interactive/auto CLI entry point)
#   - validation_setup_base.sh (broad host setup, for before/after checks)
#   - the mtl-validation-setup MCP server (via mtl_setup_common.py), so the
#     shell script and the MCP tool report identical status instead of two
#     hand-maintained copies of the same probing logic.
#
# Usage: source this file, then call the vd_* functions. It can also be run
# directly as `bash mtl_validation_discover.sh` to print the full report.

# Guard against double-sourcing.
if [[ -n "${_MTL_VALIDATION_DISCOVER_SH_:-}" ]]; then
	# shellcheck disable=SC2317 # exit is reached when run directly, not sourced
	return 0 2>/dev/null || exit 0
fi
_MTL_VALIDATION_DISCOVER_SH_=1

VD_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VD_REPO_ROOT="$(cd "$VD_LIB_DIR/../../.." && pwd)"
VD_LOCAL_PREFIX_DEFAULT="$VD_REPO_ROOT/.local_install/mtl"

# Generic (non-validation-specific) host helpers — hugepages, CPU governor,
# ICE driver — shared with the mtl-system-setup MCP server's Python code, so
# there is exactly one implementation of each.
# shellcheck source=mtl_host_common.sh disable=SC1091
source "$VD_LIB_DIR/mtl_host_common.sh"

# Intel E810/E830/E825/E835 PCI vendor:device IDs — matches the vendor/device
# check already used by .github/scripts/setup_validation.sh's preflight stage
# and gen_config.py's PF autodetection, so all three agree on what a valid
# validation PF looks like.
VD_INTEL_NIC_VENDOR_DEVICE_RE='8086:(1592|12d2|579d|1249)'

# ---------------------------------------------------------------------------
# Kernel / CPU
# ---------------------------------------------------------------------------
vd_report_kernel_cpu() {
	echo "## Kernel / CPU"
	echo "- Kernel: $(uname -r)"
	echo "- CPUs: $(nproc)"
	echo "- $(lscpu | grep 'NUMA node(s)')"
	if [[ -e /var/run/reboot-required ]]; then
		echo "- WARNING: /var/run/reboot-required present"
	fi
}

# ---------------------------------------------------------------------------
# Hugepages
# ---------------------------------------------------------------------------
vd_hugepages_free_mb() {
	awk '/HugePages_Free/ {print $2*2}' /proc/meminfo
}

vd_report_hugepages() {
	echo "## Hugepages"
	mh_hugepages_report | tail -n +2 | sed 's/^/- /'
}

# ---------------------------------------------------------------------------
# CPU governor
# ---------------------------------------------------------------------------
vd_cpu_governor_is_performance() { mh_cpu_governor_is_performance; }

vd_report_cpu_governor() {
	local status="OK"
	vd_cpu_governor_is_performance || status="NOT performance"
	echo "## CPU Governor: $status"
	echo "- $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unavailable') (cpu0, representative)"
}

# ---------------------------------------------------------------------------
# ICE driver
# ---------------------------------------------------------------------------
vd_ice_required_version() { mh_ice_required_version; }
vd_ice_live_version() { mh_ice_live_version; }
vd_ice_driver_ok() { mh_ice_driver_ok; }

vd_report_ice_driver() {
	local status="OK"
	vd_ice_driver_ok || status="ACTION NEEDED"
	echo "## ICE Driver: $status"
	echo "- Required version: $(vd_ice_required_version)"
	echo "- Live version: $(vd_ice_live_version)"
	echo "- Live module path: $(mh_ice_live_path)"
	if [[ "$status" != "OK" ]]; then
		echo "- Fix: run 'validation_setup.sh setup --base-only' (auto-detects and"
		echo "       rebuilds if needed) or 'SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 bash .github/scripts/setup_environment.sh'"
	fi
}

# ---------------------------------------------------------------------------
# NIC PFs (Intel E810/E830/E825/E835)
# ---------------------------------------------------------------------------
# Prints one PF per line: bdf|iface|driver|numa|sriov_totalvfs|sriov_numvfs
vd_list_pfs() {
	local bdf iface driver numa totalvfs numvfs
	while read -r bdf _vd; do
		[[ -n "$bdf" ]] || continue
		bdf="0000:$bdf"
		local dev="/sys/bus/pci/devices/$bdf"
		[[ -d "$dev" ]] || continue
		iface="N/A"
		if [[ -d "$dev/net" ]]; then
			iface=$(find "$dev/net" -mindepth 1 -maxdepth 1 -printf '%f\n' 2>/dev/null | head -1)
			[[ -n "$iface" ]] || iface="N/A"
		fi
		driver="N/A"
		[[ -e "$dev/driver" ]] && driver=$(basename "$(readlink -f "$dev/driver")")
		numa="N/A"
		[[ -r "$dev/numa_node" ]] && numa=$(<"$dev/numa_node")
		totalvfs="0"
		[[ -r "$dev/sriov_totalvfs" ]] && totalvfs=$(<"$dev/sriov_totalvfs")
		numvfs="0"
		[[ -r "$dev/sriov_numvfs" ]] && numvfs=$(<"$dev/sriov_numvfs")
		echo "${bdf}|${iface}|${driver}|${numa}|${totalvfs}|${numvfs}"
	done < <(lspci -nn 2>/dev/null | grep -Ei "$VD_INTEL_NIC_VENDOR_DEVICE_RE" | awk '{print $1, $3}' | tr -d '[]')
}

vd_report_pfs() {
	echo "## NIC PFs (Intel E810/E830/E825/E835)"
	local rows
	rows=$(vd_list_pfs)
	if [[ -z "$rows" ]]; then
		echo "- none found"
		return
	fi
	printf '%-14s %-8s %-8s %-6s %-8s %-8s\n' "BDF" "IFACE" "DRIVER" "NUMA" "TOTALVF" "NUMVF"
	local bdf iface driver numa totalvfs numvfs
	while IFS='|' read -r bdf iface driver numa totalvfs numvfs; do
		printf '%-14s %-8s %-8s %-6s %-8s %-8s\n' "$bdf" "$iface" "$driver" "$numa" "$totalvfs" "$numvfs"
	done <<<"$rows"
}

# ---------------------------------------------------------------------------
# DPDK / MTL build status
# ---------------------------------------------------------------------------
vd_local_install_ready() {
	local prefix="${1:-$VD_LOCAL_PREFIX_DEFAULT}"
	[[ -x "$prefix/bin/MtlManager" && -x "$prefix/bin/RxTxApp" ]]
}

vd_report_build() {
	local prefix="${1:-$VD_LOCAL_PREFIX_DEFAULT}"
	echo "## Build status"
	echo "- libdpdk (system, for gtest): $(pkg-config --modversion libdpdk 2>/dev/null || echo MISSING)"
	echo "- $prefix/bin/MtlManager: $([[ -x "$prefix/bin/MtlManager" ]] && echo OK || echo MISSING)"
	echo "- $prefix/bin/RxTxApp: $([[ -x "$prefix/bin/RxTxApp" ]] && echo OK || echo MISSING)"
	echo "- $prefix/lib*/libmtl.so: $([[ -f "$prefix/lib64/libmtl.so" || -f "$prefix/lib/x86_64-linux-gnu/libmtl.so" ]] && echo OK || echo MISSING)"
	echo "- $VD_REPO_ROOT/.local_install/ffmpeg/bin/ffmpeg: $([[ -x "$VD_REPO_ROOT/.local_install/ffmpeg/bin/ffmpeg" ]] && echo OK || echo "MISSING (only needed for application=ffmpeg tests)")"
	echo "- $VD_REPO_ROOT/.local_install/gstreamer: $([[ -d "$VD_REPO_ROOT/.local_install/gstreamer" ]] && echo OK || echo "MISSING (only needed for application=gstreamer tests)")"
}

# ---------------------------------------------------------------------------
# NFS media
# ---------------------------------------------------------------------------
vd_nfs_media_ok() {
	local f=ParkJoy_1920x1080_10bit_50Hz_P422_yuv422p10be_To_yuv422YCBCR10be.yuv
	[[ -f "/mnt/media/$f" ]]
}

vd_report_nfs() {
	echo "## NFS Media (/mnt/media)"
	if mountpoint -q /mnt/media 2>/dev/null; then
		echo "- mounted from: $(findmnt -no SOURCE /mnt/media)"
		echo "- entries: $(find /mnt/media -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
	elif [[ -n "$(ls -A /mnt/media 2>/dev/null)" ]]; then
		echo "- /mnt/media not mounted but non-empty (local files)"
	else
		echo "- NOT MOUNTED / empty — needs NFS_SOURCE=<host>:<export>"
	fi
	vd_nfs_media_ok && echo "- canonical media file: present" || echo "- canonical media file: MISSING"
}

# ---------------------------------------------------------------------------
# venv / configs
# ---------------------------------------------------------------------------
vd_report_venv_configs() {
	echo "## Pytest venv / configs"
	echo "- tests/validation/venv: $([[ -x "$VD_REPO_ROOT/tests/validation/venv/bin/python3" ]] && echo OK || echo MISSING)"
	local topo="$VD_REPO_ROOT/tests/validation/configs/topology_config.yaml"
	local tcfg="$VD_REPO_ROOT/tests/validation/configs/test_config.yaml"
	echo "- topology_config.yaml: $([[ -f "$topo" ]] && echo OK || echo MISSING)"
	echo "- test_config.yaml: $([[ -f "$tcfg" ]] && echo OK || echo MISSING)"
	if [[ -f "$tcfg" ]]; then
		echo "- compliance: $(grep -m1 '^compliance:' "$tcfg" | awk '{print $2}')"
	fi
}

# ---------------------------------------------------------------------------
# Full report — the "status" entry point
# ---------------------------------------------------------------------------
vd_full_report() {
	local prefix="${1:-$VD_LOCAL_PREFIX_DEFAULT}"
	vd_report_kernel_cpu
	echo
	vd_report_hugepages
	echo
	vd_report_cpu_governor
	echo
	vd_report_ice_driver
	echo
	vd_report_pfs
	echo
	vd_report_build "$prefix"
	echo
	vd_report_nfs
	echo
	vd_report_venv_configs
}

# Allow `bash mtl_validation_discover.sh` to print the full report directly.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
	vd_full_report "$@"
fi
