#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)

case "${1:-}" in
npcap)
	cd "$root_dir"
	wget https://nmap.org/npcap/dist/npcap-sdk-1.12.zip
	unzip -d npcap-sdk npcap-sdk-1.12.zip
	cp npcap-sdk/Lib/x64/* "${MSYSTEM_PREFIX:?MSYSTEM_PREFIX is required}/lib/"
	;;
mman)
	cd "${root_dir}/mman-win32"
	./configure --prefix="${MSYSTEM_PREFIX:?MSYSTEM_PREFIX is required}"
	make -j"$(nproc)"
	make install
	;;
convert-patches)
	for patch_dir in "${root_dir}/patches/dpdk/${DPDK_VERSION:?DPDK_VERSION is required}" \
		"${root_dir}/patches/dpdk/${DPDK_VERSION}/windows"; do
		cd "$patch_dir"
		for patch in ./*.patch; do
			if [[ $(sed -n '1p' "$patch") =~ ^\.\./.*\.patch$ ]]; then
				cp "$(cat "$patch")" "$patch"
			fi
		done
	done
	;;
hash-patches)
	hash=$(sha1sum "${root_dir}/patches/dpdk/${DPDK_VERSION:?DPDK_VERSION is required}"/*.patch \
		"${root_dir}/patches/dpdk/${DPDK_VERSION}/windows"/*.patch | sha1sum | cut -d' ' -f1)
	printf 'hash=%s\n' "$hash" >>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	;;
apply-patches)
	cd "${root_dir}/dpdk"
	git config user.name github-actions
	git config user.email github-actions@github.com
	git am "${root_dir}/patches/dpdk/${DPDK_VERSION:?DPDK_VERSION is required}"/*.patch
	git am "${root_dir}/patches/dpdk/${DPDK_VERSION}/windows"/*.patch
	;;
build-dpdk)
	cd "${root_dir}/dpdk"
	meson setup build -Dplatform=generic
	meson install -C build
	;;
install-dpdk)
	meson install -C "${root_dir}/dpdk/build" --no-rebuild
	;;
build)
	(cd "$root_dir" && ./build.sh)
	;;
build-debug)
	rm -rf "${root_dir}/build"
	(cd "$root_dir" && ./build.sh debugonly)
	;;
build-tap)
	cd "$root_dir"
	meson setup tap_build -Denable_tap=true
	meson install -C tap_build
	;;
unit)
	# The unit tier needs no NIC, no hugepage and no administrator right, so
	# Windows can run it.
	#
	# libmtl.dll imports wpcap.dll, because the pcap PMD and pcapng of DPDK are
	# inside it and DPDK links -lwpcap on Windows. wpcap.dll is the runtime
	# library of npcap and the npcap step above installs the SDK alone, which is
	# enough to link but not to run. The installer of the free edition of npcap
	# needs a window, so a runner cannot install it. The libpcap.dll of MSYS2
	# holds every pcap_ function of the import table, so a copy of it under the
	# name of the npcap library answers the loader. No unit case calls pcap: the
	# tier has no NIC and no pcap file.
	prefix="${MSYSTEM_PREFIX:?MSYSTEM_PREFIX is required}"
	if [[ ! -f /c/Windows/System32/wpcap.dll && ! -f ${prefix}/bin/wpcap.dll ]]; then
		cp "${prefix}/bin/libpcap.dll" "${prefix}/bin/wpcap.dll"
	fi
	(cd "$root_dir" && ./build.sh unit)
	;;
*)
	echo "Usage: $0 {npcap|mman|convert-patches|hash-patches|apply-patches|build-dpdk|install-dpdk|build|build-debug|build-tap|unit}" >&2
	exit 2
	;;
esac
