#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
local_install="${root_dir}/.local_install"
# The shadow host of the perf pair, reached as SHADOW_HOST_USER@SHADOW_HOST_IP.
shadow_ssh_options=(-o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=30)

case "$1" in
dpdk-plugins)
	# Earlier versions linked the build-time pmds path of librte_eal to the
	# restored drivers. MTL passes -d itself when that path is missing, so a
	# leftover link only keeps pointing at whichever tree created it.
	eal=$(find "${local_install}/dpdk" -name 'librte_eal.so.*' -print -quit)
	test -n "$eal"
	baked=$(grep -a -o -m1 -E '/[^"'"'"' ]*/dpdk/pmds-[0-9.]+' "$eal" || true)
	if [ -n "$baked" ] && [ -L "$baked" ]; then
		rm -f "$baked" 2>/dev/null || sudo rm -f "$baked"
		echo "dpdk plugins: removed the stale link ${baked}"
	fi
	;;
make-executable)
	find "$local_install" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec chmod +x {} +
	;;
environment)
	jpeg_pc=$(find "${local_install}/jpegxs" -name SvtJpegxs.pc -print -quit)
	test -n "$jpeg_pc"
	# A stale system copy of an MTL GStreamer plugin would shadow the cached one.
	for stale in /usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstmtl_*.so \
		/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstmtl_*.so \
		/usr/local/lib/gstreamer-1.0/libgstmtl_*.so \
		"${HOME:-/root}"/.local/share/gstreamer-1.0/plugins/libgstmtl_*.so; do
		[ -e "$stale" ] || continue
		rm -f "$stale" 2>/dev/null || sudo rm -f "$stale"
		echo "gstreamer: removed the system plugin ${stale}"
	done
	# A fresh registry, so no entry survives for a plugin removed above.
	gst_registry="${RUNNER_TEMP:-/tmp}/gstreamer-registry.bin"
	rm -f "$gst_registry"
	{
		echo "GST_PLUGIN_PATH=${local_install}/gstreamer/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
		echo "GST_REGISTRY=${gst_registry}"
		# The plugin registry build.sh wrote, restored with the MTL cache.
		echo "KAHAWAI_CFG_PATH=${local_install}/conf/kahawai.json"
		echo "PKG_CONFIG_PATH=$(dirname "$jpeg_pc"):${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig"
	} >>"$GITHUB_ENV"
	printf '%s\n' "${local_install}/mtl/bin" "${local_install}/ffmpeg/bin" \
		"${local_install}/dpdk/bin" >>"$GITHUB_PATH"
	;;
stray-libraries)
	# The install carries libmtl, DPDK, SVT-JPEG-XS and the plugins and finds
	# them relative to itself. A copy of one of them in a system directory is
	# what a tool that is not ours, or a stale linker cache, would find first.
	# Refuse to run beside one; the install needs no ldconfig.
	stray=$(find /lib /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64 -maxdepth 3 \
		\( -name 'libmtl.so*' -o -name 'librte_eal.so*' -o -name 'libSvtJpegxs.so*' \
		-o -name 'libgstmtl_*.so' -o -name 'libst_plugin_st22_*.so' \) 2>/dev/null || true)
	if [ -n "$stray" ]; then
		{
			echo "stray copies of the install's libraries on this host; remove them:"
			while IFS= read -r line; do echo "  ${line}"; done <<<"$stray"
		} >&2
		exit 1
	fi
	echo "stray libraries: none"
	;;
shadow-credentials)
	# shellcheck disable=SC1090
	. "$SHADOW_HOST_FILE"
	printf 'ip=%s\nuser=%s\n' "${SHADOW_IP:-${IP:?}}" "${SHADOW_USER:-${USER:?}}" >>"$GITHUB_OUTPUT"
	;;
shadow-sync)
	# The tree the SUT validated, and the scripts that validate and activate its
	# ICE bundle, at the same path: the shadow runs exactly what the SUT runs.
	# script/common.sh reads versions.env, so script/ needs it beside it.
	remote="${SHADOW_HOST_USER}@${SHADOW_HOST_IP}"
	# shellcheck disable=SC2029
	ssh "${shadow_ssh_options[@]}" "$remote" "mkdir -p '${root_dir}'"
	rsync -azR --delete -e "ssh ${shadow_ssh_options[*]}" "${root_dir}/./.local_install" \
		"${root_dir}/./script" "${root_dir}/./tests" "${root_dir}/./.github/scripts/ci/cache" \
		"${root_dir}/./.github/scripts/ci/host" "${root_dir}/./versions.env" "${remote}:${root_dir}/"
	;;
shadow-activate-ice)
	# The activation of the SUT, on the shadow, from the synced copy: it loads
	# the same Kahawai ice module, and fails when the bundle has no module for
	# the shadow's kernel rather than leaving the in-tree ice in place.
	remote="${SHADOW_HOST_USER}@${SHADOW_HOST_IP}"
	# shellcheck disable=SC2029
	ssh "${shadow_ssh_options[@]}" "$remote" \
		"sudo -n bash '${root_dir}/.github/scripts/ci/host/activate-ice.sh'"
	;;
shadow-prepare-test-ports)
	# Prepare the synced shadow with the same command that prepared the SUT.
	remote="${SHADOW_HOST_USER}@${SHADOW_HOST_IP}"
	# shellcheck disable=SC2029
	ssh "${shadow_ssh_options[@]}" "$remote" \
		"sudo -n bash '${root_dir}/script/prepare_test_ports.sh'"
	;;
*)
	echo "unknown configure-host mode: $1" >&2
	exit 2
	;;
esac
