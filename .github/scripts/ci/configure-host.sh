#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
local_install="${root_dir}/.local_install"

case "$1" in
make-executable)
	find "$local_install" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec chmod +x {} +
	;;
dpdk-plugins)
	# EAL loads drivers only from the build-time pmds path; see doc/ci_runner_setup.md.
	eal=$(find "${local_install}/dpdk" -name 'librte_eal.so.*' -print -quit)
	test -n "$eal"
	baked=$(grep -a -o -m1 -E '/[^"'"'"' ]*/dpdk/pmds-[0-9.]+' "$eal" || true)
	if [ -z "$baked" ]; then
		echo "no plugin path is baked into ${eal}; EAL loads no plugins by itself" >&2
		exit 1
	fi
	# meson installs each driver twice; only the pmds copy is the directory EAL scans.
	drivers=$(find "${local_install}/dpdk" -path '*/dpdk/pmds-*' \
		-name 'librte_mempool_ring.so' -print -quit)
	test -n "$drivers"
	drivers=${drivers%/*}
	if [ "$baked" = "$drivers" ]; then
		echo "dpdk plugins: ${baked} (built here, nothing to align)"
	elif [ "$(readlink -f "$baked" 2>/dev/null)" = "$drivers" ]; then
		echo "dpdk plugins: ${baked} -> ${drivers} (already aligned)"
	elif [ -e "$baked" ] && [ ! -L "$baked" ]; then
		echo "${baked} exists and is not our symlink; refusing to replace it" >&2
		exit 1
	else
		mkdir -p "${baked%/*}" 2>/dev/null || sudo mkdir -p "${baked%/*}"
		ln -sfn "$drivers" "$baked" 2>/dev/null || sudo ln -sfn "$drivers" "$baked"
		echo "dpdk plugins: linked ${baked} -> ${drivers}"
	fi
	;;
registry)
	config="${RUNNER_TEMP:-/tmp}/kahawai_ci.json"
	avcodec=$(find "${local_install}/plugins" -name libst_plugin_st22_avcodec.so -print -quit)
	jpegxs=$(find "${local_install}/jpegxs" -name libst_plugin_st22_svt_jpeg_xs.so -print -quit)
	test -n "$jpegxs"
	avcodec_dir=${avcodec%/*}
	[ -n "$avcodec" ] || avcodec_dir="${local_install}/plugins/lib/x86_64-linux-gnu"
	sed -e "s|/usr/local/lib[^\"]*/libst_plugin_st22_svt_jpeg_xs.so|${jpegxs}|" \
		-e "s|REPLACE_BY_CICD_PLUGIN_DIR|${avcodec_dir}|" \
		"${root_dir}/.github/workflows/kahawai_template.json" >"$config"
	echo "KAHAWAI_CFG_PATH=${config}" >>"$GITHUB_ENV"
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
		echo "LD_LIBRARY_PATH=${local_install}/jpegxs/lib:${local_install}/jpegxs/lib64:${local_install}/dpdk/lib/x86_64-linux-gnu:${local_install}/mtl/lib/x86_64-linux-gnu:${local_install}/ffmpeg/lib:${local_install}/gstreamer/gstreamer-1.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
		echo "GST_PLUGIN_PATH=${local_install}/gstreamer/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
		echo "GST_REGISTRY=${gst_registry}"
		echo "PKG_CONFIG_PATH=$(dirname "$jpeg_pc"):${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig"
	} >>"$GITHUB_ENV"
	printf '%s\n' "${local_install}/mtl/bin" "${local_install}/ffmpeg/bin" \
		"${local_install}/dpdk/bin" >>"$GITHUB_PATH"
	;;
shadow-credentials)
	# shellcheck disable=SC1090
	. "$SHADOW_HOST_FILE"
	printf 'ip=%s\nuser=%s\n' "${SHADOW_IP:-${IP:?}}" "${SHADOW_USER:-${USER:?}}" >>"$GITHUB_OUTPUT"
	;;
shadow-sync)
	ssh_options=(-o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=30)
	remote="${SHADOW_HOST_USER}@${SHADOW_HOST_IP}"
	# shellcheck disable=SC2029
	ssh "${ssh_options[@]}" "$remote" "mkdir -p '${root_dir}'"
	rsync -az --delete -e "ssh ${ssh_options[*]}" "${root_dir}/.local_install" \
		"${root_dir}/script" "${root_dir}/tests" "${remote}:${root_dir}/"
	;;
*)
	echo "unknown configure-host mode: $1" >&2
	exit 2
	;;
esac
