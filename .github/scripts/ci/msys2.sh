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
*)
	echo "Usage: $0 {npcap|mman|convert-patches|hash-patches|apply-patches|build-dpdk|install-dpdk|build|build-debug|build-tap}" >&2
	exit 2
	;;
esac