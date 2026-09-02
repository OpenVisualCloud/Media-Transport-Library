#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# Regression tests for nicctl.sh against a PF that is bound to a kernel driver but
# has registered no netdev. ice unregisters and re-registers the PF netdev across a
# rebuild, so a caller can land in that window; dpdk-devbind.py then prints `if=`
# with nothing after it. nicctl.sh used to read that back as the literal `drv=ice`,
# skip its own no-netdev branch, and go straight to `echo 0 >sriov_numvfs`, which
# does not return while the PF is rebuilding. Three smoke legs died there, each
# taking SIGTERM and outliving it until the SSH connection dropped 60s later.
#
# No NIC and no root: dpdk-devbind.py is stubbed through MTL_INSTALL_PREFIX, which
# nicctl.sh prepends to PATH. The BDF is one that exists on no host, so any write to
# /sys/bus/pci/devices/<bdf>/sriov_numvfs fails with ENOENT -- the presence or
# absence of that error is how the tests below tell "wrote" from "refused to write".

set -u

root_dir=$(cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")/.." && pwd)
nicctl=${NICCTL:-${root_dir}/script/nicctl.sh}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir -p "${scratch}/dpdk/bin"
stub="${scratch}/dpdk/bin/dpdk-devbind.py"

bdf=0000:98:00.0
no_netdev="${bdf} 'Ethernet Controller E810-C for QSFP 1592' numa_node=2 if= drv=ice unused=vfio-pci"
netdev="${bdf} 'Ethernet Controller E810-C for QSFP 1592' numa_node=2 if=ice0 drv=ice unused=vfio-pci"

fails=0

set_stub() {
	{
		echo '#!/bin/sh'
		echo 'echo "Network devices using kernel driver"'
		echo 'echo "===================================="'
		printf 'echo %q\n' "$1"
	} >"$stub"
	chmod +x "$stub"
}

pass() { echo "ok    $1"; }

fail() {
	echo "FAIL  $1 -- $2"
	fails=$((fails + 1))
}

# Run create_tvf and assert on its output: $2 must appear, $3 must not.
check_create_tvf() {
	local name=$1 want=$2 avoid=$3 out why=""
	out=$(MTL_INSTALL_PREFIX="$scratch" bash "$nicctl" create_tvf "$bdf" 1 2>&1)
	grep -qF -- "$want" <<<"$out" || why="missing '${want}'"
	if grep -qF -- "$avoid" <<<"$out"; then
		why="${why}${why:+; }must not contain '${avoid}'"
	fi
	if [ -n "$why" ]; then
		fail "$name" "$why"
		while IFS= read -r line; do printf '        | %s\n' "$line"; done <<<"$out"
	else
		pass "$name"
	fi
}

# devbind_field in isolation, sourced out of nicctl.sh: the parse is the defect, so
# it is asserted directly and not only through the behaviour it drives.
check_devbind_field() {
	local name=$1 line=$2 want=$3 got
	set_stub "$line"
	got=$(MTL_INSTALL_PREFIX="$scratch" bash -c "
		PATH=${scratch}/dpdk/bin:\$PATH
		$(sed -n '/^devbind_field() {/,/^}/p' "$nicctl")
		printf '%s|%s|%s' \"\$(devbind_field ${bdf} if)\" \
			\"\$(devbind_field ${bdf} drv)\" \"\$(devbind_field ${bdf} unused)\"")
	if [ "$got" = "$want" ]; then
		pass "${name} -> ${got}"
	else
		fail "$name" "got '${got}', want '${want}'"
	fi
}

echo "== devbind_field(if|drv|unused) =="
check_devbind_field "if=ice0" "$netdev" "ice0|ice|vfio-pci"
check_devbind_field "if= empty" "$no_netdev" "|ice|vfio-pci"

echo
echo "== create_tvf, PF with no netdev: refuse, and write nothing =="
set_stub "$no_netdev"
check_create_tvf "names the state and leaves sriov_numvfs alone" \
	"but has no netdev" "No such file or directory"

echo
echo "== create_tvf, PF with a netdev: the guard stays out of the way =="
set_stub "$netdev"
check_create_tvf "proceeds to the sriov_numvfs write" \
	"No such file or directory" "but has no netdev"

echo
if [ "$fails" -eq 0 ]; then
	echo "nicctl_test: all checks passed"
else
	echo "nicctl_test: ${fails} check(s) failed"
fi
exit "$fails"
