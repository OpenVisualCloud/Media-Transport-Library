#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

# Builds and installs xdp-tools + libbpf at the versions pinned in versions.env.
#
#   build_ebpf_xdp.sh                  check the host, then build and install
#   build_ebpf_xdp.sh --check          check the host only, install nothing
#   build_ebpf_xdp.sh --check build    check only what building MTL needs
#
# The check names the package to install for everything it finds missing,
# because these dependencies are invisible until something far away breaks:
# libdpdk.pc requires libelf, so a host without libelf-dev fails inside a
# pkg-config call minutes into a build with no hint of the cause. That is also
# why the build scope exists: a host that builds MTL but never xdp-tools should
# be held to the former's dependencies, not the latter's.

set -e

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}
# common.sh loads versions.env and defines log_*, as_root and command_exists.
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

archive_name="archive.zip"
repo_dir="${script_folder}/xdp-tools"

have_header() { echo "#include <$1>" | cc -E - >/dev/null 2>&1; }

# Everything needed to build and to run MTL's eBPF/XDP paths, and separately the
# subset of it that building MTL itself consumes. Collected rather than reported
# one at a time, so a bare host is fixed with a single apt line.
#
# The two sets are not the same, and treating them as one stopped the fleet. The
# build job builds DPDK, MTL, the plugins, FFmpeg and GStreamer; it never builds
# xdp-tools. It gates on this script because libdpdk.pc names libelf and zlib in
# Requires.private, so a host missing either fails inside a pkg-config call
# minutes into the build with nothing pointing at the cause. When cap-ng.h
# joined the list for xdp-tools' sake, every build on every host that had not
# been reprovisioned failed on a header nothing it builds includes. So the
# caller says which set it needs.
check() {
	local scope=${1:-all} missing=() item config
	config="/boot/config-$(uname -r)"

	case "${scope}" in
	all | build) ;;
	*)
		log_error "unknown check scope '${scope}', want 'all' or 'build'"
		exit 2
		;;
	esac

	# The build scope: what consuming DPDK requires of the host, whatever else
	# that host is for.
	for item in libelf.h:libelf-dev zlib.h:zlib1g-dev; do
		have_header "${item%%:*}" ||
			missing+=("${item%%:*}: apt install ${item#*:} -- libdpdk.pc requires it")
	done
	command_exists make || missing+=("make: apt install make")

	if [ "${scope}" != build ]; then
		# xdp-tools' own prerequisites: it links libcap-ng to drop capabilities,
		# needs libpcap for xdpdump -- its configure calls that one "required" and
		# stops -- compiles its BPF objects with clang and strips them with llvm,
		# and is fetched as an archive by the install path below.
		have_header cap-ng.h || missing+=("cap-ng.h: apt install libcap-ng-dev")
		have_header pcap/pcap.h || missing+=("pcap/pcap.h: apt install libpcap-dev")
		for item in m4:m4 clang:clang llvm-strip:llvm wget:wget unzip:unzip; do
			command_exists "${item%%:*}" || missing+=("${item%%:*}: apt install ${item#*:}")
		done

		# No package supplies these; the running kernel has them or it does not.
		# Skipped where the config is unreadable, as in a container.
		if [ -r "${config}" ]; then
			for item in CONFIG_BPF_SYSCALL CONFIG_XDP_SOCKETS CONFIG_BPF_JIT; do
				grep -q "^${item}=y" "${config}" ||
					missing+=("${item}: kernel $(uname -r) cannot serve AF_XDP, boot one built with it")
			done
		fi
	fi

	local what="eBPF/XDP"
	[ "${scope}" = build ] && what="MTL build"

	if [ "${#missing[@]}" -ne 0 ]; then
		log_error "${what} prerequisites missing on $(hostname):"
		printf '  %s\n' "${missing[@]}" >&2
		exit 1
	fi
	log_success "${what} prerequisites present"
}

# Where this host's pkg-config says it looks, one directory per line. Only for
# reporting: see pkgconfig_searches() for why it is not the truth.
pkgconfig_dirs() {
	pkg-config --variable pc_path pkg-config 2>/dev/null | tr ':' '\n'
}

# Whether this host's pkg-config reads a .pc file placed in ${1}/pkgconfig,
# answered by placing one and asking.
#
# Asked rather than derived, because `pkg-config --variable pc_path pkg-config`
# is a string in a .pc file the distribution ships and it can be wrong: on
# Rocky 9 it names /usr/lib64/pkgconfig and /usr/share/pkgconfig only, while
# pkgconf's compiled-in default also searches /usr/local/lib64/pkgconfig. A
# probe cannot be out of date, and it costs one file per candidate.
pkgconfig_searches() {
	local dir="${1}/pkgconfig" probe=mtl-pkgconfig-probe rc=0
	as_root mkdir -p "${dir}"
	printf 'Name: %s\nDescription: probe\nVersion: 0\n' "${probe}" |
		as_root tee "${dir}/${probe}.pc" >/dev/null
	pkg-config --exists "${probe}" || rc=$?
	as_root rm -f "${dir}/${probe}.pc"
	return "${rc}"
}

# The library directory to install into, under ${1}: the first candidate this
# host's pkg-config reads.
#
# xdp-tools and libbpf both default LIBDIR to ${PREFIX}/lib64. Debian's
# pkg-config does not search that, and the multiarch directory it does search
# does not exist on RHEL, so neither upstream default is portable. Getting it
# wrong is silent, which is what made it worth solving here: `make install`
# succeeds, pkg-config then reports the library absent, and MTL configures
# itself without AF_XDP -- on a host, or in an image whose whole purpose is the
# AF_XDP datapath.
install_libdir() {
	local prefix=$1 candidate arch
	arch=$(cc -dumpmachine 2>/dev/null || true)
	for candidate in ${arch:+"${prefix}/lib/${arch}"} "${prefix}/lib64" "${prefix}/lib"; do
		if pkgconfig_searches "${candidate}"; then
			printf '%s' "${candidate}"
			return
		fi
	done
	# Nothing under the prefix is read at all: install to the conventional
	# directory rather than one the package manager owns, and let
	# publish_pkgconfig() make it visible.
	printf '%s/lib' "${prefix}"
}

# Link the .pc files installed under ${1} into a directory pkg-config does
# search, for a host that searches none under the install prefix.
#
# The alternative is PKG_CONFIG_PATH, and it would have to be set by every
# consumer: DPDK's meson, MTL's meson, and every pkg-config call in a job or a
# shell. A non-login shell reads no profile, so there is nowhere to set it once.
# A .pc file records its own libdir, so it answers correctly from wherever it is
# read, and linking rather than copying keeps one file to look at when the
# version in play is not the one that was pinned.
publish_pkgconfig() {
	local libdir=$1 target name
	target=$(pkgconfig_dirs | grep -m1 '^/usr/lib' || true)
	if [ -z "${target}" ]; then
		log_error "pkg-config searches no directory under /usr: $(pkgconfig_dirs | tr '\n' ' ')"
		exit 1
	fi

	log_info "Publishing into ${target}, the search path of this host's pkg-config"
	as_root mkdir -p "${target}"
	for name in libxdp.pc libbpf.pc; do
		if [ -e "${target}/${name}" ] && [ ! -L "${target}/${name}" ]; then
			log_error "${target}/${name} is a real file, so a package owns it"
			log_error "remove that package before installing these from source"
			exit 1
		fi
		as_root ln -sf "${libdir}/pkgconfig/${name}" "${target}/${name}"
	done
}

build_and_install() {
	pushd "${script_folder}" >/dev/null || exit 1

	if [ -d "${repo_dir}" ]; then
		log_error "source directory exists already, remove it first: rm -rf ${repo_dir}"
		exit 1
	fi

	log_info "Downloading xdp-tools ${XDP_TOOLS_VER} and libbpf ${EBPF_VER}"
	wget -O "${archive_name}" "$XDP_REPO_URL"
	mkdir -p "${repo_dir}"
	unzip "${archive_name}" -d "${repo_dir}"
	mv "${repo_dir}"/xdp-tools-*/* "${repo_dir}"

	rm "${archive_name}"
	wget -O "${archive_name}" "$EBPF_REPO_URL"
	unzip "${archive_name}" -d "${repo_dir}"/lib/libbpf
	mv "${repo_dir}"/lib/libbpf/libbpf*/* "${repo_dir}"/lib/libbpf

	local prefix=/usr/local libdir
	libdir=$(install_libdir "${prefix}")
	log_info "Installing into ${libdir}"

	pushd "${repo_dir}" >/dev/null || exit 1
	./configure
	make
	as_root make install PREFIX="${prefix}" LIBDIR="${libdir}"
	pushd lib/libbpf/src >/dev/null || exit 1
	make
	as_root make install PREFIX="${prefix}" LIBDIR="${libdir}"
	popd >/dev/null
	popd >/dev/null

	# The runtime linker's default search path is as distribution-specific as
	# pkg-config's, and a library it cannot find fails at load time rather than
	# at link time. Naming the directory is idempotent and cheap.
	printf '%s\n' "${libdir}" | as_root tee /etc/ld.so.conf.d/mtl-xdp.conf >/dev/null
	as_root ldconfig

	# Only where the install landed outside the search path, which is the case
	# install_libdir() cannot solve on its own.
	pkg-config --exists libxdp 2>/dev/null || publish_pkgconfig "${libdir}"

	log_info "Removing downloaded XDP sources"
	rm -rf "${repo_dir}"
	rm -f "${script_folder}/${archive_name}"
	popd >/dev/null

	# Assert the install delivered the pinned versions: a distro libxdp.pc
	# earlier on PKG_CONFIG_PATH otherwise wins, silently.
	local item got
	for item in "libxdp:${XDP_TOOLS_VER}" "libbpf:${EBPF_VER}"; do
		got="$(pkg-config --modversion "${item%%:*}" 2>/dev/null || true)"
		[ "${got}" = "${item#*:}" ] || {
			log_error "${item%%:*} is ${got:-absent} after install, want ${item#*:}"
			log_error "pkg-config searches: $(pkgconfig_dirs | tr '\n' ' ')"
			log_error ".pc files installed: $(find "${prefix}" -name '*.pc' 2>/dev/null | tr '\n' ' ')"
			exit 1
		}
	done
	log_success "xdp-tools ${XDP_TOOLS_VER} and libbpf ${EBPF_VER} installed"
}

case "${1:-}" in
--check) check "${2:-all}" ;;
"")
	check
	build_and_install
	;;
*)
	log_error "usage: ${script_name} [--check [all|build]]"
	exit 2
	;;
esac
