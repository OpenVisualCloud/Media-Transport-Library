#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

# Builds and installs xdp-tools + libbpf at the versions pinned in
# versions.env, or -- with --check -- verifies that a host already has them.
#
#   build_ebpf_xdp.sh                         build and install
#   build_ebpf_xdp.sh --check [options]       verify only, install nothing
#
#     --mode build|runtime|all  what to verify (default: all)
#     --strict                  exit 1 when a required item is missing
#     --require-xdp             treat xdp-tools/libbpf as mandatory and
#                               demand the exact versions from versions.env
#     --quiet                   only emit the summary
#
# --check exists because these dependencies are invisible until something far
# away breaks. libelf is the clearest case: libdpdk.pc requires it, but nothing
# installs it unless SETUP_BUILD_AND_INSTALL_EBPF_XDP is set. A long-lived
# self-hosted runner has it from an earlier run and passes; a fresh one fails
# inside a pkg-config call three steps later, with no hint of the cause. Run
# --check as the first step of a job so the job dies in seconds, not minutes.

set -e
VERSIONS_ENV_PATH="$(dirname "$(readlink -qe "${BASH_SOURCE[0]}")")/../versions.env"

if [ -f "$VERSIONS_ENV_PATH" ]; then
	# shellcheck disable=SC1090
	. "$VERSIONS_ENV_PATH"
else
	echo -e "Error: versions.env file not found at $VERSIONS_ENV_PATH"
	exit 1
fi

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
script_folder=${script_path/$script_name/}

archive_name="archive.zip"
repo_dir="${script_folder}/xdp-tools"

MISSING_REQUIRED=()
MISSING_OPTIONAL=()

# GitHub renders ::notice:: and friends as annotations; a plain shell shows
# them as-is, which is still readable.
note() { [ "${QUIET:-0}" -eq 1 ] || echo "$1"; }
warn() { echo "::warning::$1"; }
fail() { echo "::error::$1"; }

report() {
	# report <ok:0|1> <required:0|1> <label> <hint>
	local ok="$1" required="$2" label="$3" hint="$4"
	if [ "${ok}" -eq 0 ]; then
		note "  ok       ${label}"
		return
	fi
	if [ "${required}" -eq 1 ]; then
		MISSING_REQUIRED+=("${label} -- ${hint}")
		note "  MISSING  ${label}  (${hint})"
	else
		MISSING_OPTIONAL+=("${label} -- ${hint}")
		note "  absent   ${label}  (${hint}, optional)"
	fi
}

have_header() {
	echo "#include <$1>" | cc -E - >/dev/null 2>&1
}

kernel_config() {
	# Echo the config file for the running kernel, if it is readable.
	if [ -r "/boot/config-$(uname -r)" ]; then
		cat "/boot/config-$(uname -r)"
	elif [ -r /proc/config.gz ]; then
		zcat /proc/config.gz
	fi
}

check_version() {
	# check_version <pkg-config name> <wanted version> <required:0|1>
	local pc="$1" want="$2" required="$3" got
	got="$(pkg-config --modversion "${pc}" 2>/dev/null)"
	if [ -z "${got}" ]; then
		report 1 "${required}" "${pc} (want ${want})" \
			"not installed; run ${script_name} to build it"
	elif [ "${got}" = "${want}" ]; then
		note "  ok       ${pc} ${got}"
	else
		report 1 "${required}" "${pc} ${got} != ${want}" \
			"rebuild with ${script_name} to match versions.env"
	fi
}

check_build() {
	note "eBPF/XDP build prerequisites:"

	# The eBPF toolchain is only required where AF_XDP is actually built or
	# exercised. libelf is not conditional: libdpdk.pc requires it, so every
	# job that consumes DPDK needs it.
	have_header libelf.h
	report $? 1 "libelf.h" "apt install libelf-dev; libdpdk.pc requires it"

	have_header zlib.h
	report $? "${EBPF_REQUIRED}" "zlib.h" "apt install zlib1g-dev"

	command -v clang >/dev/null 2>&1
	report $? "${EBPF_REQUIRED}" "clang" "apt install clang; compiles the eBPF objects"

	command -v llvm-strip >/dev/null 2>&1 || command -v llc >/dev/null 2>&1
	report $? "${EBPF_REQUIRED}" "llvm" "apt install llvm"

	have_header cap-ng.h
	report $? "${EBPF_REQUIRED}" "cap-ng.h" "apt install libcap-ng-dev"

	have_header bpf/bpf.h
	report $? "${EBPF_REQUIRED}" "bpf/bpf.h" "run ${script_name}"

	have_header xdp/xsk.h
	report $? "${EBPF_REQUIRED}" "xdp/xsk.h" "run ${script_name}"

	note "eBPF/XDP versions pinned by versions.env:"
	check_version libxdp "${XDP_TOOLS_VER}" "${EBPF_REQUIRED}"
	check_version libbpf "${EBPF_VER}" "${EBPF_REQUIRED}"

	if pkg-config --exists libdpdk 2>/dev/null; then
		note "  ok       libdpdk resolves ($(pkg-config --modversion libdpdk 2>/dev/null))"
	else
		note "  absent   libdpdk does not resolve on PKG_CONFIG_PATH (expected before the DPDK step)"
	fi
}

check_runtime() {
	note "eBPF/XDP runtime prerequisites (kernel $(uname -r)):"

	local config
	config="$(kernel_config)"
	if [ -z "${config}" ]; then
		note "  unknown  kernel config unreadable; skipping CONFIG_* checks"
	else
		local opt
		for opt in CONFIG_BPF_SYSCALL CONFIG_XDP_SOCKETS CONFIG_BPF_JIT; do
			grep -q "^${opt}=y" <<<"${config}"
			report $? 1 "${opt}" "the running kernel cannot serve AF_XDP"
		done
		grep -q "^CONFIG_XDP_SOCKETS_DIAG=[ym]" <<<"${config}"
		report $? 0 "CONFIG_XDP_SOCKETS_DIAG" "only needed to introspect sockets"
	fi

	ldconfig -p 2>/dev/null | grep -q 'libxdp\.so'
	report $? "${EBPF_REQUIRED}" "libxdp.so" "run ${script_name}"

	ldconfig -p 2>/dev/null | grep -q 'libbpf\.so'
	report $? "${EBPF_REQUIRED}" "libbpf.so" "run ${script_name}"

	command -v bpftool >/dev/null 2>&1
	report $? 0 "bpftool" "apt install linux-tools-common; diagnostics only"

	# AF_XDP needs privilege: root, or CAP_BPF + CAP_NET_RAW on the binary.
	if [ "$(id -u)" -eq 0 ]; then
		note "  ok       running as root"
	else
		note "  note     not root; AF_XDP needs CAP_BPF and CAP_NET_RAW"
	fi

	local unpriv=/proc/sys/kernel/unprivileged_bpf_disabled
	if [ -r "${unpriv}" ] && [ "$(cat "${unpriv}")" != "0" ] && [ "$(id -u)" -ne 0 ]; then
		note "  note     unprivileged_bpf_disabled=$(cat "${unpriv}"); load eBPF as root"
	fi
}

run_checks() {
	# The probes are expected to fail; $? is the signal, not an error.
	set +e

	if [ "${MODE}" = "build" ] || [ "${MODE}" = "all" ]; then
		check_build
	fi
	if [ "${MODE}" = "runtime" ] || [ "${MODE}" = "all" ]; then
		check_runtime
	fi

	local summary="eBPF/XDP check (${MODE}): ${#MISSING_REQUIRED[@]} required missing, ${#MISSING_OPTIONAL[@]} optional missing"

	note ""
	if [ "${#MISSING_REQUIRED[@]}" -eq 0 ]; then
		echo "::notice::${summary}"
		return 0
	fi

	local item
	for item in "${MISSING_REQUIRED[@]}"; do
		if [ "${STRICT}" -eq 1 ]; then
			fail "eBPF/XDP prerequisite missing: ${item}"
		else
			warn "eBPF/XDP prerequisite missing: ${item}"
		fi
	done

	if [ "${STRICT}" -eq 1 ]; then
		fail "${summary}"
		return 1
	fi

	echo "::notice::${summary}"
	return 0
}

build_and_install() {
	pushd "${script_folder}" >/dev/null || exit 1

	if [ -d "${repo_dir}" ]; then
		echo "XDP \"$(realpath "$repo_dir")\" source directory already exists, please remove it first"
		exit 1
	fi

	echo "Clone XDP source code"
	wget -O "${archive_name}" "$XDP_REPO_URL"
	mkdir -p "${repo_dir}"
	unzip "${archive_name}" -d "${repo_dir}"
	mv "${repo_dir}"/xdp-tools-*/* "${repo_dir}"

	rm "${archive_name}"
	wget -O "${archive_name}" "$EBPF_REPO_URL"
	unzip "${archive_name}" -d "${repo_dir}"/lib/libbpf
	mv "${repo_dir}"/lib/libbpf/libbpf*/* "${repo_dir}"/lib/libbpf

	pushd "${repo_dir}" >/dev/null || exit 1
	./configure
	make
	sudo make install
	pushd lib/libbpf/src >/dev/null || exit 1
	make
	sudo make install
	popd >/dev/null
	popd >/dev/null
	echo "Removing downloaded XDP sources"
	rm -rf "${repo_dir}"
	rm -f "${script_folder}/${archive_name}"
	popd >/dev/null
}

(return 0 2>/dev/null) && sourced=1 || sourced=0

if [ "$sourced" -eq 0 ]; then
	CHECK=0
	MODE="all"
	STRICT=0
	QUIET=0
	REQUIRE_XDP=0

	while [ $# -gt 0 ]; do
		case "$1" in
		--check)
			CHECK=1
			shift
			;;
		--mode)
			MODE="${2:-all}"
			shift 2
			;;
		--strict)
			STRICT=1
			shift
			;;
		--require-xdp)
			REQUIRE_XDP=1
			shift
			;;
		--quiet)
			QUIET=1
			shift
			;;
		-h | --help)
			sed -n '6,24p' "$0"
			exit 0
			;;
		*)
			echo "unknown argument: $1" >&2
			exit 2
			;;
		esac
	done

	if [ "${CHECK}" -eq 1 ]; then
		case "${MODE}" in
		build | runtime | all) ;;
		*)
			echo "--mode must be build, runtime or all" >&2
			exit 2
			;;
		esac

		# xdp-tools and libbpf are mandatory only where AF_XDP is built or
		# exercised; elsewhere their absence is reported, not fatal.
		EBPF_REQUIRED=0
		if [ "${REQUIRE_XDP}" -eq 1 ] || [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP:-0}" = "1" ]; then
			EBPF_REQUIRED=1
		fi

		run_checks
		exit $?
	fi

	build_and_install
fi
