#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation
#
# Start a manager of its own, run MtlManagerProbe against it, stop it again.
#
# The probe needs a manager, and a developer must not have to keep one running
# by hand, nor point the probe at the manager that serves the real instances of
# the host. This script gives the probe a private one on a private socket.
#
#   manager/tools/run_probe.sh                    # the groups that need no NIC
#   manager/tools/run_probe.sh --ifname probe0    # every group
#   sudo manager/tools/run_probe.sh --ifname probe0   # also the XDP groups
#
# WARNING: the manager deletes every receive flow rule of the interface it
# takes, so name an interface of your own. A veth pair costs nothing:
#
#   sudo ip link add probe0 type veth peer name probe1
#   sudo ip link set probe0 up && sudo ip link set probe1 up
#
# Every argument goes to the probe. --sock-path is added by this script.

set -u

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build=${MTLM_BUILD_DIR:-${here}/../build}
manager=${build}/MtlManager
probe=${build}/tools/MtlManagerProbe

for binary in "${manager}" "${probe}"; do
	if [[ ! -x ${binary} ]]; then
		echo "no ${binary} -- build the manager first:" >&2
		echo "  meson setup ${build} manager && ninja -C ${build}" >&2
		exit 2
	fi
done

# The manager loads the XDP program by name and lets libxdp search for it, and
# libxdp does not look in a build tree. Point it at the object that was just
# built, so an in-tree run reaches the real XDP path instead of reporting that
# no program could attach. An object already named in the environment wins.
xdp_object=${build}/mtl.xdp.o
if [[ -z ${MTL_MANAGER_XDP_OBJECT:-} && -f ${xdp_object} ]]; then
	export MTL_MANAGER_XDP_OBJECT=${xdp_object}
fi

# libxdp loads a dispatcher object of its own before it attaches anything, and it
# looks for that object in a path it was compiled with. A libxdp built from
# source installs it under its multiarch directory, which the compiled-in path
# does not always name, and the attach then fails with "Couldn't open BPF file
# xdp-dispatcher.o". Find it and say where it is.
if [[ -z ${LIBXDP_OBJECT_PATH:-} ]]; then
	for candidate in /usr/local/lib/*/bpf /usr/local/lib/bpf /usr/lib/*/bpf /usr/lib/bpf; do
		if [[ -f ${candidate}/xdp-dispatcher.o ]]; then
			export LIBXDP_OBJECT_PATH=${candidate}
			break
		fi
	done
fi

run_dir=$(mktemp -d /tmp/mtlm-probe-run.XXXXXX)
sock=${run_dir}/mtl_manager.sock
log=${run_dir}/manager.log

# The EXIT trap below calls this. shellcheck reads a trap as neither a call
# (SC2329) nor a reachable body (SC2317), so both reports are wrong here.
# shellcheck disable=SC2329,SC2317
cleanup() {
	if [[ -n ${manager_pid:-} ]]; then
		kill "${manager_pid}" 2>/dev/null
		wait "${manager_pid}" 2>/dev/null
	fi
	rm -rf -- "${run_dir}"
}
trap cleanup EXIT

# MTLM_PRELOAD names a library to preload in the manager and the probe, and not
# in this script. An asan build needs its runtime preloaded, and a preload of
# bash puts bash under LeakSanitizer too, which reports the leaks of bash.
preload=()
if [[ -n ${MTLM_PRELOAD:-} ]]; then
	preload=(env "LD_PRELOAD=${MTLM_PRELOAD}")
fi

MTL_MANAGER_SOCK_PATH=${sock} "${preload[@]}" "${manager}" >"${log}" 2>&1 &
manager_pid=$!

for _ in $(seq 1 100); do
	[[ -S ${sock} ]] && break
	sleep 0.1
done

if [[ ! -S ${sock} ]]; then
	echo "the manager did not bind ${sock}" >&2
	cat -- "${log}" >&2
	exit 2
fi

"${preload[@]}" "${probe}" --sock-path "${sock}" "$@"
status=$?

if ((status != 0)); then
	echo
	echo "--- the log of the manager under test ---"
	cat -- "${log}"
fi

# A manager that died during the probe is a failure, whatever the probe said.
if ! kill -0 "${manager_pid}" 2>/dev/null; then
	echo "the manager under test is gone" >&2
	status=1
fi

exit "${status}"
