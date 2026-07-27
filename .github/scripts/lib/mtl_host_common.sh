#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Generic, non-validation-specific host setup/discovery helpers shared by
# BOTH the system-wide setup path (mtl-system-setup MCP server, used for
# gtest/KahawaiTest/VFs) and the tests/validation/ pytest setup path
# (mtl-validation-setup MCP server + validation_setup*.sh). Nothing in this
# file assumes a `.local_install` prefix or pytest-specific config — that
# lives in lib/mtl_validation_discover.sh instead.
#
# Functions here are the single implementation of "is the ICE driver OK?",
# "set hugepages", "set CPU governor to performance", "apt dependencies" —
# both mtl_setup_common.py (Python, shared by both MCP servers) and the
# validation shell scripts source/shell out to these instead of each
# hand-rolling their own copy.
#
# Usage: source this file, then call the mh_* functions.

if [[ -n "${_MTL_HOST_COMMON_SH_:-}" ]]; then
	# shellcheck disable=SC2317 # exit is reached when run directly, not sourced
	return 0 2>/dev/null || exit 0
fi
_MTL_HOST_COMMON_SH_=1

MH_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MH_REPO_ROOT="$(cd "$MH_LIB_DIR/../../.." && pwd)"

# ---------------------------------------------------------------------------
# apt dependencies
# ---------------------------------------------------------------------------
mh_install_dependencies() {
	(
		cd "$MH_REPO_ROOT" &&
			SETUP_ENVIRONMENT=1 \
				SETUP_BUILD_AND_INSTALL_DPDK=0 \
				SETUP_BUILD_AND_INSTALL_ICE_DRIVER=0 \
				SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 \
				SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 \
				MTL_BUILD_AND_INSTALL=0 \
				bash .github/scripts/setup_environment.sh
	)
}

# ---------------------------------------------------------------------------
# ICE driver
# ---------------------------------------------------------------------------
mh_ice_required_version() {
	awk -F= '/^ICE_VER=/ {print $2; exit}' "$MH_REPO_ROOT/versions.env" 2>/dev/null
}

mh_ice_live_version() {
	modinfo ice 2>/dev/null | awk '/^version:/ {print $2; exit}'
}

mh_ice_live_path() {
	modinfo -n ice 2>/dev/null
}

mh_ice_oot_ko_path() {
	echo "/lib/modules/$(uname -r)/updates/drivers/net/ethernet/intel/ice/ice.ko"
}

# True (rc 0) when the loaded ice driver is the out-of-tree, version-matching
# build MTL's rate-limit pacing requires.
mh_ice_driver_ok() {
	local live_path
	live_path=$(mh_ice_live_path)
	[[ "$live_path" == *"/updates/"* ]] || return 1
	[[ "$(mh_ice_live_version)" == *"$(mh_ice_required_version)"* ]]
}

# Markdown status report — matches the historical `ice_driver_status` MCP
# tool output so its docstring/callers don't need to change.
mh_ice_driver_status_report() {
	local want live live_path oot_ko oot_exists oot_ver is_oot matches status
	want=$(mh_ice_required_version)
	live=$(mh_ice_live_version)
	[[ -z "$live" ]] && live="not loaded"
	live_path=$(mh_ice_live_path)
	[[ -z "$live_path" ]] && live_path="not found"
	oot_ko=$(mh_ice_oot_ko_path)
	oot_exists=0
	[[ -f "$oot_ko" ]] && oot_exists=1
	oot_ver=""
	[[ "$oot_exists" == "1" ]] && oot_ver=$(modinfo "$oot_ko" 2>/dev/null | awk '/^version:/ {print $2; exit}')
	is_oot=0
	[[ "$live_path" == *"/updates/"* ]] && is_oot=1
	matches=0
	[[ "$live" != "not loaded" && "$live" == *"$want"* ]] && matches=1
	status="ACTION NEEDED"
	[[ "$is_oot" == "1" && "$matches" == "1" ]] && status="OK"

	echo "## ICE Driver Status: $status"
	echo "- Required version: $want"
	echo "- Live version: $live"
	echo "- Live module path: $live_path"
	echo "- Out-of-tree module: $([[ "$oot_exists" == "1" ]] && echo EXISTS || echo MISSING) at $oot_ko"
	echo "  - OOT version: ${oot_ver:-N/A}"
	echo "- Using out-of-tree: $([[ "$is_oot" == "1" ]] && echo YES || echo 'NO — stock kernel driver')"
	if [[ "$status" != "OK" ]]; then
		echo
		echo "### Issue"
		echo "MTL requires the patched out-of-tree ICE driver for rate-limit pacing."
		echo "Stock kernel ice does not support the iavf TM virtchnl messages."
		echo 'Rebuild it with: SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 bash .github/scripts/setup_environment.sh'
	fi
}

# Build + install the patched out-of-tree ICE driver, then reload it.
# NOTE: reloading ice destroys any existing VFs on every PF.
mh_ice_driver_rebuild() {
	(
		cd "$MH_REPO_ROOT" &&
			SETUP_ENVIRONMENT=0 \
				SETUP_BUILD_AND_INSTALL_DPDK=0 \
				SETUP_BUILD_AND_INSTALL_ICE_DRIVER=1 \
				SETUP_BUILD_AND_INSTALL_EBPF_XDP=0 \
				SETUP_BUILD_AND_INSTALL_GPU_DIRECT=0 \
				MTL_BUILD_AND_INSTALL=0 \
				FORCE_ICE_REBUILD=1 \
				bash .github/scripts/setup_environment.sh
	) || return $?
	sudo depmod -a
	sudo rmmod irdma 2>/dev/null
	sudo rmmod ice 2>/dev/null
	sudo modprobe ice
	modinfo ice | head -5
}

# ---------------------------------------------------------------------------
# Hugepages
# ---------------------------------------------------------------------------
mh_hugepages_report() {
	echo "## Hugepages"
	grep -i huge /proc/meminfo
}

# mh_hugepages_set <nr_hugepages> [size_kb=2048]
mh_hugepages_set() {
	local nr="${1:?nr_hugepages required}" size_kb="${2:-2048}"
	local hp_path="/sys/kernel/mm/hugepages/hugepages-${size_kb}kB/nr_hugepages"
	if [[ ! -e "$hp_path" ]]; then
		echo "Error: hugepage size ${size_kb}kB not supported. Available:" >&2
		ls /sys/kernel/mm/hugepages/ >&2
		return 1
	fi
	echo "$nr" | sudo tee "$hp_path" >/dev/null
}

# ---------------------------------------------------------------------------
# CPU governor
# ---------------------------------------------------------------------------
mh_cpu_governor_is_performance() {
	local f gov
	for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[[ -f "$f" ]] || continue
		gov=$(<"$f")
		[[ "$gov" == "performance" ]] || return 1
	done
	return 0
}

mh_cpu_governor_set_performance() {
	local cpu
	for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		echo performance | sudo tee "$cpu" >/dev/null
	done
}

# Set governor to performance on every CPU, then report pass/fail per-CPU.
mh_cpu_governor_set_and_confirm_performance() {
	mh_cpu_governor_set_performance
	local f gov total=0 non_perf=()
	for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[[ -f "$f" ]] || continue
		total=$((total + 1))
		gov=$(<"$f")
		[[ "$gov" == "performance" ]] || non_perf+=("$f=$gov")
	done
	echo "## CPU Governor (set to performance + confirm)"
	echo "- CPUs checked: $total"
	if [[ "$total" -gt 0 && ${#non_perf[@]} -eq 0 ]]; then
		echo "- Status: PASS"
	else
		echo "- Status: FAIL"
		[[ ${#non_perf[@]} -gt 0 ]] && printf -- '- %s\n' "${non_perf[@]}"
	fi
}
