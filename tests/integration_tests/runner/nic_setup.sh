#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Minimal NIC shell helper: create-tvf, bind-pvf, and matching cleanup only.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="${MTL_REPO_ROOT:-$(realpath "${script_dir}/../../..")}"
nicctl="${MTL_NICCTL:-${repo_root}/script/nicctl.sh}"
bdf_re='^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$'

run_nicctl() {
	if [ "${EUID}" -eq 0 ] || [ -n "${MTL_NICCTL_NO_SUDO:-}" ]; then
		"${nicctl}" "$@"
	else
		sudo -E "${nicctl}" "$@"
	fi
}

check_bdf() {
	[[ "$1" =~ ${bdf_re} ]] || {
		echo "Error: invalid PCI BDF '$1'" >&2
		exit 2
	}
}

cmd="${1:-}"
shift || true
case "${cmd}" in
create-tvf)
	pf="${1:-}"
	count="${2:-6}"
	check_bdf "${pf}"
	[[ "${count}" =~ ^[1-9][0-9]*$ ]] || {
		echo "Error: invalid VF count '${count}'" >&2
		exit 2
	}
	run_nicctl create_tvf "${pf}" "${count}" >&2
	"${nicctl}" list "${pf}" | awk '/^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$/ {print $1}'
	;;
bind-pvf)
	[ "$#" -ge 1 ] || exit 2
	for bdf in "$@"; do
		check_bdf "${bdf}"
		run_nicctl bind_pmd "${bdf}" >&2
		printf '%s\n' "${bdf}"
	done
	;;
restore-pvf | disable-vf | vf-rss-lut)
	[ "$#" -ge 1 ] || exit 2
	sub="${cmd//-/_}"
	[ "${cmd}" = restore-pvf ] && sub="bind_kernel"
	rc=0
	for bdf in "$@"; do
		check_bdf "${bdf}"
		run_nicctl "${sub}" "${bdf}" >&2 || rc=$?
	done
	exit "${rc}"
	;;
*)
	echo "Usage: nic_setup.sh {create-tvf <pf> [n]|bind-pvf|restore-pvf|disable-vf|vf-rss-lut} <bdf...>" >&2
	exit 2
	;;
esac
