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

	# That export reaches the steps of this job and nothing else. The acceptance
	# suite starts every app over SSH to localhost and then under sudo, so it
	# inherits none of the job's environment -- the same reason dpdk-plugins above
	# is a symlink and not a variable. With KAHAWAI_CFG_PATH unset in that shell
	# mt_config_init falls back to the cwd-relative "kahawai.json", and the cwd of
	# an SSH session is the login user's home, so the registry has to be a file
	# there or st22p gets whatever a human last left in that directory. Both
	# spellings, because both are read: the plain name is what the fallback opens,
	# the dotted one is what the hosts that already pass point KAHAWAI_CFG_PATH at.
	# Write one and not the other and the fleet stays split, which is what it was:
	# on the hosts with neither, every JPEG-XS case died in st22_get_encoder with
	# "fail to get, input fmt: YUV422PLANAR10LE" because the registry it did find
	# still carried the disabled st22_svt_jpegxs entries of the tracked kahawai.json.
	for installed in "${HOME:?HOME is required}/kahawai.json" "${HOME}/.kahawai.json"; do
		cp "$config" "$installed"
	done
	echo "plugin registry: ${config}, installed in ${HOME} for the apps started over SSH"
	;;
clock-tai)
	# ST 2110 RTP timestamps are on the PTP/TAI timescale; the Linux system
	# clock is UTC. The kernel carries the difference as a settable constant,
	# and on a host where no PTP or NTP stack ever set it, it reads 0 -- which
	# claims TAI == UTC and is wrong by every leap second since 1972.
	#
	# Two things consume it, so 0 is not cosmetic. RxTxApp reads CLOCK_TAI to
	# stamp outgoing media (doc/chunks/_run_i226.md), and the acceptance
	# suite's conftest._host_tai_utc_offset reads it to compute the phc2sys -O
	# that lands the capture NIC's PHC on TAI. With 0 the capture clock is
	# parked on UTC, 37s off the media clock, and ST 2110-21 VRX is then
	# measuring the gap between two timescales rather than MTL's pacing. Every
	# smoke leg that reached the compliance analyser logged "TAI-UTC offset
	# reads 0" first; see doc/ci_runner_setup.md.
	#
	# tzdata's leap-second table is the source, not a hard-coded 37, so this
	# stays correct across the next leap second. Entries are stamped in the NTP
	# epoch (1900-01-01, 2208988800s before the Unix one) and the table may
	# announce a future one, so take the last entry already in effect.
	table=/usr/share/zoneinfo/leap-seconds.list
	if [ ! -r "$table" ]; then
		echo "clock-tai: ${table} is missing -- install tzdata" >&2
		exit 1
	fi
	offset=$(awk -v now="$(($(date +%s) + 2208988800))" \
		'!/^#/ && NF >= 2 && $1 + 0 <= now { v = $2 } END { print v + 0 }' "$table")
	if [ "$offset" -le 0 ]; then
		echo "clock-tai: no leap-second entry in effect found in ${table}" >&2
		exit 1
	fi

	# clock_adjtime(CLOCK_REALTIME, {modes: ADJ_TAI, constant: offset}). No
	# shell tool sets this: adjtimex(8) is not packaged on the runners, and
	# chronyd would need a leapsectz configuration and a running daemon.
	sudo python3 - "$offset" <<-'PY'
		import ctypes, sys, time

		L = ctypes.c_long


		class Timeval(ctypes.Structure):
		    _fields_ = [("tv_sec", L), ("tv_usec", L)]


		# struct timex, x86_64: 208 bytes. Asserted below rather than trusted,
		# because a short struct would make the kernel write past what we
		# allocated and a mislaid field would silently set the wrong knob.
		class Timex(ctypes.Structure):
		    _fields_ = [
		        ("modes", ctypes.c_uint), ("offset", L), ("freq", L),
		        ("maxerror", L), ("esterror", L), ("status", ctypes.c_int),
		        ("constant", L), ("precision", L), ("tolerance", L),
		        ("time", Timeval), ("tick", L), ("ppsfreq", L), ("jitter", L),
		        ("shift", ctypes.c_int), ("stabil", L), ("jitcnt", L),
		        ("calcnt", L), ("errcnt", L), ("stbcnt", L),
		        ("tai", ctypes.c_int), ("padding", ctypes.c_int * 11),
		    ]


		assert ctypes.sizeof(Timex) == 208, ctypes.sizeof(Timex)

		ADJ_TAI = 0x0080
		CLOCK_REALTIME = 0
		libc = ctypes.CDLL("libc.so.6", use_errno=True)


		def kernel_tai():
		    tx = Timex()
		    tx.modes = 0  # read-only query
		    if libc.clock_adjtime(CLOCK_REALTIME, ctypes.byref(tx)) < 0:
		        raise OSError(ctypes.get_errno(), "clock_adjtime query failed")
		    return tx.tai


		want = int(sys.argv[1])
		have = kernel_tai()
		if have == want:
		    print(f"clock-tai: kernel TAI-UTC offset already {have}s")
		    sys.exit(0)

		tx = Timex()
		tx.modes = ADJ_TAI
		tx.constant = want
		if libc.clock_adjtime(CLOCK_REALTIME, ctypes.byref(tx)) < 0:
		    raise OSError(ctypes.get_errno(), "clock_adjtime(ADJ_TAI) failed")

		# Read back through both interfaces: the timex field proves the write
		# landed where intended, the clock difference proves the kernel acted on
		# it. A silent no-op here is the failure this whole stage exists to stop.
		readback = kernel_tai()
		live = round(time.clock_gettime(time.CLOCK_TAI) - time.clock_gettime(time.CLOCK_REALTIME))
		if readback != want or live != want:
		    raise SystemExit(
		        f"clock-tai: set {want}s but kernel reports timex.tai={readback}s "
		        f"and CLOCK_TAI-CLOCK_REALTIME={live}s"
		    )
		print(f"clock-tai: kernel TAI-UTC offset {have}s -> {want}s")
	PY
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
