#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
# shellcheck disable=SC1091
. "${root_dir}/versions.env"

dry_run=0
probe_only=0
case ${1:-} in
--dry-run) dry_run=1 ;;
--probe) probe_only=1 ;;
"") ;;
*)
	echo "usage: activate-ice.sh [--dry-run|--probe]" >&2
	exit 2
	;;
esac

kernel_release=${ICE_KERNEL_RELEASE:-$(uname -r)}
architecture=${ICE_ARCH:-$(uname -m)}
bundle_root=${ICE_BUNDLE_ROOT:-"${root_dir}/.local_install/ice"}
artifact_dir="${bundle_root}/${kernel_release}/${architecture}"
module="${artifact_dir}/ice.ko"
metadata="${artifact_dir}/metadata.env"
stamp=${ICE_ACTIVATION_STAMP:-/var/lib/mtl-ci/ice.state}
lock=${ICE_ACTIVATION_LOCK:-/run/lock/mtl-ci-ice.lock}
sys_root=${ICE_SYS_ROOT:-/sys}
modules_root=${ICE_MODULES_ROOT:-/lib/modules}
command_log=${ICE_COMMAND_LOG:-/dev/stdout}
installed_module="${modules_root}/${kernel_release}/updates/drivers/net/ethernet/intel/ice/ice.ko"

ICE_BUNDLE_ROOT="$bundle_root" ICE_KERNEL_RELEASE="$kernel_release" ICE_ARCH="$architecture" \
	bash "${root_dir}/.github/scripts/ci/validate-ice.sh"
desired_hash=$(sed -n 's/^module_sha256=//p' "$metadata")
loaded_version=$(cat "${sys_root}/module/ice/version" 2>/dev/null || true)
stamped_hash=$(sed -n 's/^module_sha256=//p' "$stamp" 2>/dev/null || true)

if [ "$desired_hash" = "$stamped_hash" ] && [ "$loaded_version" = "Kahawai_${ICE_VER}" ]; then
	echo "ICE activation is already current"
	exit 0
fi
if [ "$probe_only" -eq 1 ]; then
	echo "ICE activation required: desired=${desired_hash} active=${stamped_hash:-none} loaded=${loaded_version:-none}"
	exit 3
fi

if [ "$dry_run" -eq 0 ]; then
	[ "$(id -u)" -eq 0 ] || [ "${ICE_ALLOW_UNPRIVILEGED_TEST:-0}" = 1 ] || {
		echo "ICE activation must run as root" >&2
		exit 1
	}
	mkdir -p "$(dirname "$lock")"
	exec 9>"$lock"
	flock 9
	ICE_BUNDLE_ROOT="$bundle_root" ICE_KERNEL_RELEASE="$kernel_release" ICE_ARCH="$architecture" \
		bash "${root_dir}/.github/scripts/ci/validate-ice.sh"
	desired_hash=$(sed -n 's/^module_sha256=//p' "$metadata")
	loaded_version=$(cat "${sys_root}/module/ice/version" 2>/dev/null || true)
	stamped_hash=$(sed -n 's/^module_sha256=//p' "$stamp" 2>/dev/null || true)
	if [ "$desired_hash" = "$stamped_hash" ] && [ "$loaded_version" = "Kahawai_${ICE_VER}" ]; then
		echo "ICE activation became current while waiting for the lock"
		exit 0
	fi
fi

run() {
	printf '%q ' "$@" >>"$command_log"
	printf '\n' >>"$command_log"
	if [ "$dry_run" -eq 0 ]; then
		"$@"
	fi
}

run_optional() {
	printf '%q ' "$@" >>"$command_log"
	printf '\n' >>"$command_log"
	if [ "$dry_run" -eq 0 ]; then
		"$@" || true
	fi
}

write_vf_count() {
	count=$1
	vf_file=$2
	printf 'write %s %s\n' "$count" "$vf_file" >>"$command_log"
	if [ "$dry_run" -eq 0 ]; then
		echo "$count" >"$vf_file"
	fi
}

vf_state=$(mktemp)
trap 'rm -f "$vf_state"' EXIT
for vf_file in "${sys_root}"/class/net/*/device/sriov_numvfs; do
	[ -e "$vf_file" ] || continue
	count=$(cat "$vf_file")
	[ "$count" -gt 0 ] || continue
	pf_interface=$(basename "$(dirname "$(dirname "$vf_file")")")
	pf_device=$(dirname "$(readlink -f "$vf_file")")
	pf_driver=$(basename "$(readlink -f "$pf_device/driver" 2>/dev/null || true)")
	[ "$pf_driver" = ice ] || continue
	pf_bdf=$(basename "$pf_device")
	vf_mode=create_kvf
	if [ "$sys_root" = /sys ]; then
		all_vfio=1
		for vf_link in "$pf_device"/virtfn*; do
			[ -e "$vf_link" ] || continue
			vf_driver=$(basename "$(readlink -f "$vf_link/driver" 2>/dev/null || true)")
			[ "$vf_driver" = vfio-pci ] || all_vfio=0
		done
		if [ "$all_vfio" -eq 1 ]; then
			vf_mode=create_vf
			if ip -d link show "$pf_interface" | grep -q 'trust on'; then
				vf_mode=create_tvf
			fi
		fi
	fi
	printf '%s|%s|%s|%s\n' "$vf_file" "$count" "$pf_bdf" "$vf_mode" >>"$vf_state"
done

restore_vfs() {
	while IFS='|' read -r vf_file count pf_bdf vf_mode; do
		[ -n "$vf_file" ] || continue
		if [ "$sys_root" = /sys ]; then
			run bash "${root_dir}/script/nicctl.sh" "$vf_mode" "$pf_bdf" "$count"
		else
			write_vf_count "$count" "$vf_file"
		fi
	done <"$vf_state"
}

backup_dir=$(mktemp -d)
previous_module="${backup_dir}/ice.ko"
previous_module_present=0
previous_module_loaded=0
previous_irdma_loaded=0
[ -f "$installed_module" ] && {
	cp -a "$installed_module" "$previous_module"
	previous_module_present=1
}
[ -d "${sys_root}/module/ice" ] && previous_module_loaded=1
[ -d "${sys_root}/module/irdma" ] && previous_irdma_loaded=1

rollback() {
	rc=$?
	trap - ERR EXIT
	set +e
	echo "rollback begin" >>"$command_log"
	while IFS='|' read -r vf_file _count _pf_bdf _vf_mode; do
		[ -n "$vf_file" ] && write_vf_count 0 "$vf_file"
	done <"$vf_state"
	run_optional modprobe -r irdma
	run_optional modprobe -r ice
	if [ "$previous_module_present" -eq 1 ]; then
		run install -D -m 0644 "$previous_module" "$installed_module"
	else
		run rm -f "$installed_module"
	fi
	run depmod -a "$kernel_release"
	[ "$previous_module_loaded" -eq 0 ] || run modprobe ice
	[ "$previous_irdma_loaded" -eq 0 ] || run modprobe irdma
	restore_vfs
	echo "rollback complete" >>"$command_log"
	rm -rf "$backup_dir"
	exit "$rc"
}

if [ "$dry_run" -eq 0 ]; then
	trap rollback ERR EXIT
fi

for process_name in MtlManager RxTxApp; do
	run_optional pkill -TERM -x "$process_name"
	if [ "$dry_run" -eq 0 ] && pgrep -x "$process_name" >/dev/null; then
		echo "failed to stop ${process_name}" >&2
		false
	fi
done
while IFS='|' read -r vf_file _count _pf_bdf _vf_mode; do
	[ -n "$vf_file" ] && write_vf_count 0 "$vf_file"
done <"$vf_state"
run_optional modprobe -r irdma
run modprobe -r ice
run install -D -m 0644 "$module" "$installed_module"
run depmod -a "$kernel_release"
run modprobe ice

if [ "$dry_run" -eq 0 ]; then
	loaded_version=$(cat "${sys_root}/module/ice/version" 2>/dev/null || true)
	[ "$loaded_version" = "Kahawai_${ICE_VER}" ] || {
		echo "loaded ICE version is ${loaded_version:-unknown}, expected Kahawai_${ICE_VER}" >&2
		exit 1
	}
	selected_module=$(${ICE_MODINFO:-modinfo} -n ice)
	[ "$(readlink -f "$selected_module")" = "$(readlink -f "$installed_module")" ] || {
		echo "loaded ICE module was not selected from the installed artifact" >&2
		exit 1
	}
	"${ICE_NM:-nm}" "$selected_module" | grep '[[:space:]]ice_vc_cfg_q_bw$' >/dev/null || {
		echo "loaded ICE module is missing the Kahawai QoS capability" >&2
		exit 1
	}
fi

if [ "$previous_irdma_loaded" -ne 0 ]; then
	if ! run modprobe irdma; then
		echo "warning: unable to reload optional irdma; it remains unloaded" >&2
	fi
fi

restore_vfs

if [ "$dry_run" -eq 1 ]; then
	echo "write stamp ${stamp}" >>"$command_log"
else
	mkdir -p "$(dirname "$stamp")"
	temporary_stamp="${stamp}.tmp.$$"
	printf 'module_sha256=%s\nice_version=Kahawai_%s\n' "$desired_hash" "$ICE_VER" >"$temporary_stamp"
	chmod 0600 "$temporary_stamp"
	mv "$temporary_stamp" "$stamp"
fi

trap - ERR EXIT
rm -rf "$backup_dir"
echo "ICE activation complete"
