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
dpdk-plugins)
	# A DPDK install remembers where it was built. meson bakes
	# RTE_EAL_PMD_PATH -- <prefix>/lib/<arch>/dpdk/pmds-<abi> -- into
	# librte_eal, and EAL loads every driver from that one absolute path;
	# there is no environment override, and MTL builds a fixed EAL argv, so
	# no -d either. The build job's prefix is its own workspace, which is not
	# this runner's workspace whenever the two are different machines (or, in
	# a local run, a container at /github/workspace and the host that owns
	# the card). Then no driver registers, and the first failure is
	# mtl_init's mempool -- "mt_mempool_create_by_ops, fail(Invalid argument)
	# for T_P0_SYS" -- because even the mempool ops MTL asks for ("stack")
	# ship as plugins. Make the path the restored library looks for resolve
	# to the drivers actually restored.
	eal=$(find "${local_install}/dpdk" -name 'librte_eal.so.*' -print -quit)
	test -n "$eal"
	# grep -a rather than strings: binutils is not a prerequisite of a host
	# that only runs tests.
	baked=$(grep -a -o -m1 -E '/[^"'"'"' ]*/dpdk/pmds-[0-9.]+' "$eal" || true)
	if [ -z "$baked" ]; then
		echo "no plugin path is baked into ${eal}; EAL loads no plugins by itself" >&2
		exit 1
	fi
	# meson installs every driver twice -- once in the library directory and
	# once in the pmds directory EAL actually scans -- so a search by name
	# alone answers with whichever copy readdir happens to yield first. Only
	# the pmds copy names the path being aligned; matching the library one
	# made this step compare the baked plugin path against the library
	# directory, so a runner whose workspace path equals the build job's --
	# every host in the fleet, which lays its runner out identically -- took
	# the "not our symlink" refusal below instead of "nothing to align".
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
		# The baked path is absolute and outside the workspace, so its parent
		# may not be writable by the job's user -- /github/workspace on a host
		# runner is the usual case. Every other privileged step of these jobs
		# assumes passwordless sudo, and so does this one, but only when the
		# plain call fails.
		parent=${baked%/*}
		mkdir -p "$parent" 2>/dev/null || sudo mkdir -p "$parent"
		ln -sfn "$drivers" "$baked" 2>/dev/null || sudo ln -sfn "$drivers" "$baked"
		echo "dpdk plugins: linked ${baked} -> ${drivers}"
	fi
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

	# The MTL GStreamer elements must come from the cache and from nowhere else.
	# GST_PLUGIN_PATH below only adds a directory to the search: GStreamer also
	# reads its system plugin directories, so a libgstmtl_*.so that an earlier
	# install left in one of them is a second plugin of the same name. Which of
	# the two answers a pipeline then follows from the order the registry was
	# built in, and not from this variable, so a test can run against a plugin
	# that no job put there and that no cache key covers. The same reason
	# activate-ice.sh takes the loaded driver out before it loads the cached one.
	#
	# Only the MTL plugins go. The core, base and good plugins stay: the
	# pipelines of the suite need videotestsrc, filesink and their like, and
	# those come from the packages of the host.
	for gst_dir in /usr/lib/x86_64-linux-gnu/gstreamer-1.0 \
		/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
		/usr/local/lib/gstreamer-1.0 \
		"${HOME:-/root}/.local/share/gstreamer-1.0/plugins"; do
		[ -d "$gst_dir" ] || continue
		for stale in "${gst_dir}"/libgstmtl_*.so; do
			[ -e "$stale" ] || continue
			rm -f "$stale" 2>/dev/null || sudo rm -f "$stale"
			echo "gstreamer: removed the system plugin ${stale}"
		done
	done

	# A registry of this job alone. The cached one under ~/.cache names the files
	# it read the last time, and these runners are long-lived, so it can hold an
	# entry for a plugin that the loop above has just taken away. GStreamer then
	# reports the element and fails to load it.
	gst_registry="${RUNNER_TEMP:-/tmp}/gstreamer-registry.bin"
	rm -f "$gst_registry"

	{
		echo "LD_LIBRARY_PATH=${local_install}/jpegxs/lib:${local_install}/jpegxs/lib64:${local_install}/dpdk/lib/x86_64-linux-gnu:${local_install}/mtl/lib/x86_64-linux-gnu:${local_install}/ffmpeg/lib:${local_install}/gstreamer/gstreamer-1.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
		echo "GST_PLUGIN_PATH=${local_install}/gstreamer/gstreamer-1.0${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
		echo "GST_REGISTRY=${gst_registry}"
		echo "PKG_CONFIG_PATH=$(dirname "$jpeg_pc"):${local_install}/dpdk/lib/x86_64-linux-gnu/pkgconfig:${local_install}/mtl/lib/x86_64-linux-gnu/pkgconfig"
	} >>"${GITHUB_ENV:?GITHUB_ENV is required}"
	printf '%s\n' "${local_install}/mtl/bin" "${local_install}/ffmpeg/bin" \
		"${local_install}/dpdk/bin" >>"${GITHUB_PATH:?GITHUB_PATH is required}"
	;;
shadow-credentials)
	credentials=${SHADOW_HOST_FILE:?SHADOW_HOST_FILE is required}
	# shellcheck disable=SC1090
	. "$credentials"
	printf 'ip=%s\nuser=%s\n' "${SHADOW_IP:-${IP:?IP is required}}" \
		"${SHADOW_USER:-${USER:?USER is required}}" \
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
