#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
local_install="${root_dir}/.local_install"
mode=${1:?usage: configure-host.sh MODE}

case "$mode" in
make-executable)
	find "$local_install" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec chmod +x {} +
	;;
registry)
	template="${root_dir}/.github/workflows/kahawai_template.json"
	config="${RUNNER_TEMP:-/tmp}/kahawai_ci.json"
	avcodec=$(find "${local_install}/plugins" -name libst_plugin_st22_avcodec.so -print -quit)
	jpegxs=$(find "${local_install}/jpegxs" -name libst_plugin_st22_svt_jpeg_xs.so -print -quit)
	test -n "$jpegxs"
	avcodec_dir=${avcodec%/*}
	[ -n "$avcodec" ] || avcodec_dir="${local_install}/plugins/lib/x86_64-linux-gnu"
	cp "$template" "$config"
	sed -i \
		-e "s|/usr/local/lib/x86_64-linux-gnu/libst_plugin_st22_svt_jpeg_xs.so|${jpegxs}|" \
		-e "s|/usr/local/lib64/libst_plugin_st22_svt_jpeg_xs.so|${jpegxs}|" \
		-e "s|REPLACE_BY_CICD_PLUGIN_DIR|${avcodec_dir}|" "$config"
	echo "KAHAWAI_CFG_PATH=${config}" >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	;;
environment)
	jpeg_pc=$(find "${local_install}/jpegxs" -name SvtJpegxs.pc -print -quit)
	test -n "$jpeg_pc"
	{
		echo "LD_LIBRARY_PATH=${local_install}/jpegxs/lib:${local_install}/jpegxs/lib64:${local_install}/dpdk/lib/x86_64-linux-gnu:${local_install}/mtl/lib/x86_64-linux-gnu:${local_install}/ffmpeg/lib:${local_install}/gstreamer/gstreamer-1.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
		echo "GST_PLUGIN_PATH=${local_install}/gstreamer/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
		echo "PKG_CONFIG_PATH=$(dirname "$jpeg_pc"):${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig"
	} >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	printf '%s\n' "${local_install}/mtl/bin" "${local_install}/ffmpeg/bin" \
		"${local_install}/dpdk/bin" >>"${GITHUB_PATH:?GITHUB_PATH is required}"
	;;
shadow-credentials)
	credentials=${SHADOW_HOST_FILE:?SHADOW_HOST_FILE is required}
	# shellcheck disable=SC1090
	. "$credentials"
	printf 'ip=%s\nuser=%s\n' "${IP:?IP is required}" "${USER:?USER is required}" \
		>>"${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
	;;
shadow-sync)
	shadow_ip=${SHADOW_HOST_IP:?SHADOW_HOST_IP is required}
	shadow_user=${SHADOW_HOST_USER:?SHADOW_HOST_USER is required}
	ssh_options=(-o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=30)
	# shellcheck disable=SC2029
	ssh "${ssh_options[@]}" "${shadow_user}@${shadow_ip}" \
		"mkdir -p '${root_dir}/.local_install' '${root_dir}/script' '${root_dir}/tests'"
	for path in .local_install script tests; do
		rsync -az --delete -e "ssh ${ssh_options[*]}" "${root_dir}/${path}/" \
			"${shadow_user}@${shadow_ip}:${root_dir}/${path}/"
	done
	;;
*)
	echo "unknown configure-host mode: ${mode}" >&2
	exit 2
	;;
esac