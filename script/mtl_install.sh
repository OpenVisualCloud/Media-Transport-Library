#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_folder="$(cd -- "${script_folder}/.." && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/mtl_common.sh"

# The template of the options. Copy it, set the options, source it, and run
# the script. test-scripts.sh checks that it names each option below.
install_template="${script_folder}/mtl_install_template.env"

# SET DEFAULT ARGUMENTS

# When set, all build scripts install to this prefix instead of system-wide.
: "${MTL_INSTALL_PREFIX:=}"
export MTL_INSTALL_PREFIX

# Before MTL build install
: "${SETUP_ENVIRONMENT:=0}"
: "${SETUP_BUILD_AND_INSTALL_DPDK:=0}"
: "${SETUP_BUILD_AND_INSTALL_DRIVERS:=0}"
: "${SETUP_BUILD_AND_INSTALL_DRIVERS_ICE:=0}"
: "${SETUP_BUILD_AND_INSTALL_DRIVERS_IGC:=0}"
: "${SETUP_BUILD_AND_INSTALL_EBPF_XDP:=0}"
: "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT:=0}"

# MTL build and install
: "${MTL_BUILD_AND_INSTALL:=0}"
: "${MTL_BUILD_AND_INSTALL_DEBUG:=0}"
: "${MTL_BUILD_AND_INSTALL_FUZZ:=0}"
: "${MTL_BUILD_AND_INSTALL_UNIT_TESTS:=0}"
: "${MTL_BUILD_AND_INSTALL_DOCKER:=0}"
: "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER:=0}"

# After MTL build
: "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_GSTREAMER_PLUGIN_RELEASE:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:=0}"

: "${PLUGIN_BUILD_AND_INSTALL_SAMPLE:=0}"
: "${PLUGIN_BUILD_AND_INSTALL_AVCODEC:=0}"
: "${PLUGIN_BUILD_AND_INSTALL_JPEGXS:=0}"

: "${HOOK_PYTHON:=0}"
: "${HOOK_RUST:=0}"

: "${TOOLS_BUILD_AND_INSTALL_MTL_MONITORS:=0}"
: "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:=0}"
: "${TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR:=0}"
: "${TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET:=0}"
: "${TOOLS_RUN_SET_TAI_OFFSET:=0}"

# CI/CD-only settings
# Run in non-interactive mode for automated/containerized builds.
# If any dependency installation fails, the script exits immediately.
: "${CICD_BUILD:=0}"

# The options in the order of the steps, for the help, the check, the dry run
# and the summary. "<variable>|<group>|<what the step does>".
setup_flags=(
	"SETUP_ENVIRONMENT|host|Install the build packages of each selected component (mtl_install_dependencies.sh). Ubuntu only."
	"SETUP_BUILD_AND_INSTALL_GPU_DIRECT|host|Build and install gpu_direct/ (mtl_gpu_direct) into the system."
	"SETUP_BUILD_AND_INSTALL_EBPF_XDP|host|Build and install xdp-tools and libbpf (mtl_build_ebpf_xdp.sh) into the system, for the AF_XDP backend."
	"SETUP_BUILD_AND_INSTALL_DPDK|host|Download, patch, build and install DPDK of versions.env (mtl_build_dpdk.sh -f). Rebuilds on each run."
	"SETUP_BUILD_AND_INSTALL_DRIVERS|host|Build and install the ice and igc kernel drivers (mtl_build_drivers.sh)."
	"SETUP_BUILD_AND_INSTALL_DRIVERS_ICE|host|The ice driver only, for E810 and E830."
	"SETUP_BUILD_AND_INSTALL_DRIVERS_IGC|host|The igc driver only, for I225 and I226."
	"MTL_BUILD_AND_INSTALL|mtl|Build and install MTL, the apps and the manager, release (build.sh)."
	"MTL_BUILD_AND_INSTALL_DEBUG|mtl|The same as a debug build (build.sh debug). It replaces the release build."
	"MTL_BUILD_AND_INSTALL_FUZZ|mtl|Add the libFuzzer harnesses (build.sh enable_fuzzing). Needs clang and an MTL build option."
	"MTL_BUILD_AND_INSTALL_UNIT_TESTS|mtl|Build and run the unit tests (build.sh unit) instead of the MTL install: nothing is installed. Needs an MTL build option."
	"MTL_BUILD_AND_INSTALL_DOCKER|mtl|Build the Docker image mtl:latest (docker/ubuntu22.dockerfile)."
	"MTL_BUILD_AND_INSTALL_DOCKER_MANAGER|mtl|Build the Docker image mtl-manager:latest (manager/Dockerfile)."
	"PLUGIN_BUILD_AND_INSTALL_JPEGXS|plugin|Build and install SVT JPEG XS and its ST 2110-22 plugin (mtl_build_jpegxs.sh)."
	"ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN|plugin|Build and install FFmpeg with the MTL devices (mtl_build_ffmpeg_plugin.sh)."
	"ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN|plugin|Build and install the GStreamer plugins and test tools (mtl_build_gstreamer_plugin.sh), debug."
	"ECOSYSTEM_BUILD_GSTREAMER_PLUGIN_RELEASE|plugin|Build the GStreamer plugins as release (-Werror), not debug."
	"ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN|plugin|Build and install the OBS Studio plugin into the system."
	"PLUGIN_BUILD_AND_INSTALL_SAMPLE|plugin|Build and install the sample plugins of plugins/ into the system."
	"PLUGIN_BUILD_AND_INSTALL_AVCODEC|plugin|Build and install the ST 2110-22 avcodec plugin (mtl_build_st22_avcodec_plugin.sh)."
	"HOOK_PYTHON|binding|Build SWIG 4.1.1 and install the Python binding into the system python3."
	"HOOK_RUST|binding|Build the Rust binding (cargo build --release in rust/)."
	"TOOLS_BUILD_AND_INSTALL_MTL_MONITORS|tool|Build the eBPF lcore_monitor and udp_monitor (tools/ebpf)."
	"TOOLS_BUILD_AND_INSTALL_MTL_READPCAP|tool|Build readpcap (tools/readpcap)."
	"TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR|tool|Build the SMI emulator of the scheduler (tools/sch_smi_emulate)."
	"TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET|tool|Build set_tai_offset (tools/set_tai_offset)."
	"TOOLS_RUN_SET_TAI_OFFSET|tool|Run set_tai_offset as root: set the TAI offset of the kernel to 37 s. Builds it first if needed."
	"CICD_BUILD|ci|Non-interactive mode for CI and containers: stop at the first failed package install."
)

# The prefixes of the option names. An exported variable with one of them that
# is not an option is a typing error: the script stops instead of ignoring it.
option_prefixes="SETUP_ENVIRONMENT SETUP_BUILD_AND_INSTALL_ MTL_BUILD_AND_INSTALL ECOSYSTEM_BUILD_ PLUGIN_BUILD_AND_INSTALL_ HOOK_ TOOLS_BUILD_AND_INSTALL_ TOOLS_RUN_ CICD_BUILD"

print_group() {
	local group="$1" entry var rest
	for entry in "${setup_flags[@]}"; do
		var=${entry%%|*}
		rest=${entry#*|}
		[ "${rest%%|*}" = "${group}" ] || continue
		printf '\t%s\n\t\t%s\n' "${var}" "${rest#*|}"
	done
}

show_help() {
	cat <<EOF
Usage: ${script_name} [-h] [-n]

Set up a Linux host for the Media Transport Library (MTL) in one command:
install the build packages, then build and install DPDK, the NIC drivers,
MTL, the plugins, the bindings and the tools, in the correct order.

Environment variables select the work. Each option is 0 (off, the default)
or 1 (on). With no option on, the script does nothing.

The script runs the other scripts of script/. You can run each of them
yourself instead, step by step; doc/getting_started/build.md tells how.
The script does not set up the runtime (hugepages, VFs, the MtlManager
service): see doc/getting_started/run.md and mtl_nicctl.sh.

Run it as your own user, from any directory. It uses sudo for each step that
installs into the system. In CI, run it with 'sudo -E'.

OPTIONS:
	-h, --help	Show this help message
	-n, --dry-run	Check the options, print the steps that they select, in
			order, and stop. Changes nothing on the host

THREE WAYS TO SELECT THE WORK:
	1. On the command line:
		SETUP_ENVIRONMENT=1 SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 ${script_name}
	2. With a template file (recommended for a repeated setup):
		cp ${install_template#"${root_folder}/"} my_setup.env
		\$EDITOR my_setup.env           # set each option that you need to 1
		source my_setup.env && ${script_name}
	3. Exported in the shell or in a CI job:
		export SETUP_BUILD_AND_INSTALL_DPDK=1; ${script_name}

	The template exports each option, so the script sees the values. A value
	set on the command line replaces the value of the template.

REQUIRED PACKAGES (Debian/Ubuntu):
	sudo, when the script does not run as root. SETUP_ENVIRONMENT=1 installs
	the other packages of each selected component. On another distribution,
	install the packages of doc/getting_started/build.md section 1 yourself
	and leave SETUP_ENVIRONMENT at 0.

HOST OPTIONS (before MTL):
$(print_group host)

MTL OPTIONS:
$(print_group mtl)

PLUGIN AND ECOSYSTEM OPTIONS (after MTL):
$(print_group plugin)

BINDING OPTIONS (after MTL):
$(print_group binding)

TOOL OPTIONS:
$(print_group tool)

CI OPTIONS:
$(print_group ci)

ENVIRONMENT:
	MTL_INSTALL_PREFIX
		Install MTL into this directory, and each other component into a
		directory beside it, instead of /usr/local. The path must have no
		space. For example
		\$PWD/.local_install/mtl gives .local_install/dpdk,
		.local_install/jpegxs, .local_install/ffmpeg and others. Such a
		directory needs no root and no ldconfig. The eBPF/XDP, GPU Direct,
		OBS, sample plugin and Python steps still install into the system.
	NPROC
		Parallel build jobs (default: the CPU count)
	http_proxy, https_proxy
		Passed to the Docker builds. Keep them with 'sudo -E'.

ORDER OF THE STEPS:
	packages -> GPU Direct -> eBPF/XDP -> DPDK -> drivers -> MTL -> Docker
	images -> JPEG XS -> FFmpeg -> GStreamer -> OBS -> sample plugins ->
	avcodec plugin -> Python -> Rust -> tools. A step that fails stops the
	script; the steps before it stay installed. Run the script again with
	the same options when the cause is fixed.

EXIT STATUS:
	0 when each selected step passes, 1 for a wrong option, else the
	status of the step that failed.

EXAMPLES:
	# Packages, the E810/E830 driver, DPDK and MTL: a new E800 host
	SETUP_ENVIRONMENT=1 SETUP_BUILD_AND_INSTALL_DRIVERS_ICE=1 \\
		SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 ${script_name}
	sudo ldconfig

	# The same into .local_install, as the CI does; no ldconfig needed
	MTL_INSTALL_PREFIX="\$PWD/.local_install/mtl" SETUP_BUILD_AND_INSTALL_DPDK=1 \\
		MTL_BUILD_AND_INSTALL=1 ${script_name}

	# Rebuild MTL as debug and run the unit tests
	MTL_BUILD_AND_INSTALL_DEBUG=1 MTL_BUILD_AND_INSTALL_UNIT_TESTS=1 ${script_name}

	# Show what a template selects, change nothing
	source my_setup.env && ${script_name} --dry-run
EOF
}

# Stop for a value other than 0 or 1, and for a variable with the prefix of
# an option that is not an option, such as SETUP_BUILD_AND_INSTALL_DPKD=1.
check_options() {
	local entry var name prefix known faults=0
	for entry in "${setup_flags[@]}"; do
		var=${entry%%|*}
		case "${!var}" in
		0 | 1) ;;
		*)
			log_error "${var}=${!var}: the value must be 0 or 1."
			faults=$((faults + 1))
			;;
		esac
	done
	while read -r name; do
		for prefix in ${option_prefixes}; do
			[[ "${name}" == "${prefix}"* ]] || continue
			known=0
			for entry in "${setup_flags[@]}"; do
				[ "${entry%%|*}" = "${name}" ] && known=1 && break
			done
			if [ "${known}" = 0 ]; then
				log_error "${name} is not an option of ${script_name}. Run '${script_name} -h' for the list."
				faults=$((faults + 1))
			fi
			break
		done
	done < <(compgen -e)
	# The unit test and fuzzing selections only change the MTL build: on their
	# own they would build nothing. Stop before any work rather than guess.
	for var in MTL_BUILD_AND_INSTALL_UNIT_TESTS MTL_BUILD_AND_INSTALL_FUZZ; do
		if [ "${!var}" == "1" ] && [ "${MTL_BUILD_AND_INSTALL}" != "1" ] &&
			[ "${MTL_BUILD_AND_INSTALL_DEBUG}" != "1" ]; then
			log_error "${var}=1 needs MTL_BUILD_AND_INSTALL=1 or MTL_BUILD_AND_INSTALL_DEBUG=1."
			faults=$((faults + 1))
		fi
	done
	# DPDK cannot install into such a path; stop before the packages, not after.
	if [[ "${MTL_INSTALL_PREFIX}" == *" "* ]]; then
		log_error "MTL_INSTALL_PREFIX='${MTL_INSTALL_PREFIX}' has a space. DPDK cannot install into it: use a path with no space."
		faults=$((faults + 1))
	fi
	if [ "${ECOSYSTEM_BUILD_GSTREAMER_PLUGIN_RELEASE}" == "1" ] &&
		[ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" != "1" ]; then
		log_error "ECOSYSTEM_BUILD_GSTREAMER_PLUGIN_RELEASE=1 needs ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN=1."
		faults=$((faults + 1))
	fi
	[ "${faults}" -eq 0 ]
}

# Print each selected option in the order of the steps; "(none)" if none.
print_selected() {
	local entry var rest printed=0
	for entry in "${setup_flags[@]}"; do
		var=${entry%%|*}
		[ "${!var}" = "1" ] || continue
		rest=${entry#*|}
		log_info "  ${var}: ${rest#*|}"
		printed=1
	done
	[ "${printed}" = "1" ] || log_info "  (none)"
}

main() {
	local STEP=1 mtl_build_options local_base jpegxs_bundle tai_bin
	local -a ffmpeg_build_options
	local entry var dry_run=0
	local -a mtl_build_env
	local setup_build_drivers_options="" setup_build_ice=0

	while [ "$#" -gt 0 ]; do
		case "$1" in
		-h | --help)
			show_help
			exit 0
			;;
		-n | --dry-run)
			dry_run=1
			;;
		*)
			log_error "Unexpected argument: $1. The environment variables select the work; see '${script_name} -h'."
			exit 1
			;;
		esac
		shift
	done

	check_options || exit 1

	for entry in "${setup_flags[@]}"; do
		var=${entry%%|*}
		[ "${!var}" = "1" ] && break
		var=""
	done
	if [ -z "${var}" ]; then
		log_warning "No option is set to 1, so there is nothing to do. Run '${script_name} -h' for the options."
		exit 0
	fi

	if [ "${dry_run}" = "1" ]; then
		log_info "Install prefix: ${MTL_INSTALL_PREFIX:-the system (/usr/local)}"
		log_info "The options select these steps, in this order:"
		print_selected
		exit 0
	fi

	# The ice and igc selections are parts of the driver step.
	if [ "${SETUP_BUILD_AND_INSTALL_DRIVERS}" == "1" ]; then
		setup_build_ice=1
	else
		if [ "${SETUP_BUILD_AND_INSTALL_DRIVERS_ICE}" == "1" ]; then
			SETUP_BUILD_AND_INSTALL_DRIVERS=1
			setup_build_ice=1
		else
			setup_build_drivers_options="--disable-ice"
		fi
		if [ "${SETUP_BUILD_AND_INSTALL_DRIVERS_IGC}" == "1" ]; then
			SETUP_BUILD_AND_INSTALL_DRIVERS=1
		else
			setup_build_drivers_options="${setup_build_drivers_options} --disable-igc"
		fi
	fi

	# The trace shows each step in a CI log. A shell that sources the script
	# does not get it.
	set -x

	if [ "$SETUP_ENVIRONMENT" == "1" ]; then
		log_info "$STEP Environment setup."

		local -a dependencies=()
		if [ "${setup_build_ice}" == "1" ]; then dependencies+=(ice); fi
		for entry in SETUP_BUILD_AND_INSTALL_EBPF_XDP:ebpf_xdp SETUP_BUILD_AND_INSTALL_GPU_DIRECT:gpu_direct \
			ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:ffmpeg PLUGIN_BUILD_AND_INSTALL_JPEGXS:jpegxs \
			PLUGIN_BUILD_AND_INSTALL_AVCODEC:avcodec \
			ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:gstreamer ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:obs \
			HOOK_PYTHON:python HOOK_RUST:rust TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:readpcap; do
			var=${entry%%:*}
			if [ "${!var}" == "1" ]; then dependencies+=("${entry#*:}"); fi
		done
		bash "${root_folder}/script/mtl_install_dependencies.sh" "${dependencies[@]}"
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ]; then
		log_info "$STEP Install the build dependency for GPU Direct"
		# shellcheck disable=SC1091
		pushd "${root_folder}/gpu_direct" >/dev/null || exit 1

		if [[ ":${LIBRARY_PATH:-}:" != *":/usr/local/lib:"* ]]; then
			export LIBRARY_PATH="/usr/local/lib:${LIBRARY_PATH:-}"
		fi

		meson setup build
		as_root meson install -C build

		if pkg-config --libs mtl_gpu_direct >/dev/null 2>&1; then
			log_info "mtl_gpu_direct is available via pkg-config."
		else
			log_warning "mtl_gpu_direct is NOT available via pkg-config."
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP}" == "1" ]; then
		log_info "$STEP Install the build dependency from OS software store"
		bash "${root_folder}/script/mtl_build_ebpf_xdp.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DPDK}" == "1" ]; then
		log_info "$STEP DPDK build and install"
		# With MTL_INSTALL_PREFIX, the script installs into the dpdk sibling.
		bash "${root_folder}/script/mtl_build_dpdk.sh" -f
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DRIVERS}" == "1" ]; then
		log_info "$STEP Driver build and install"
		# shellcheck disable=SC2086
		bash "${root_folder}/script/mtl_build_drivers.sh" ${setup_build_drivers_options}
		STEP=$((STEP + 1))
	fi

	# MTL build and install
	mtl_build_options="release"
	mtl_build_env=()
	if [ "${MTL_BUILD_AND_INSTALL_DEBUG}" == "1" ]; then
		mtl_build_options="debug"
	fi
	if [ "${MTL_BUILD_AND_INSTALL_FUZZ}" == "1" ]; then
		mtl_build_options="${mtl_build_options} enable_fuzzing"
		# libFuzzer is clang's, and gcc has no -fsanitize=fuzzer at all, so
		# tests/fuzz/meson.build stops the configure with the compiler it was
		# given. Only this build switches compiler: DPDK is installed by now and
		# is linked rather than recompiled.
		if ! command -v clang >/dev/null; then
			log_error "MTL_BUILD_AND_INSTALL_FUZZ needs clang: apt install clang"
			exit 1
		fi
		mtl_build_env=(env CC=clang CXX=clang++)
	fi
	if [ "${MTL_BUILD_AND_INSTALL_UNIT_TESTS}" == "1" ]; then
		mtl_build_options="${mtl_build_options} unit"
	fi

	if [ "${MTL_BUILD_AND_INSTALL}" == "1" ] || [ "${MTL_BUILD_AND_INSTALL_DEBUG}" == "1" ]; then
		log_info "$STEP MTL build and install: ${mtl_build_options}"
		pushd "${root_folder}" >/dev/null || exit 1
		# shellcheck disable=SC2086
		"${mtl_build_env[@]}" ./build.sh ${mtl_build_options}
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER}" == "1" ]; then
		log_info "$STEP MTL docker build and install"
		pushd "${root_folder}/docker" >/dev/null || exit 1

		if [ -n "${http_proxy:-}" ] && [ -n "${https_proxy:-}" ]; then
			docker build -t mtl:latest -f ubuntu22.dockerfile --build-arg HTTP_PROXY="${http_proxy:-}" --build-arg HTTPS_PROXY="${https_proxy:-}" ../
		else
			docker build -t mtl:latest -f ubuntu22.dockerfile ../
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER}" == "1" ]; then
		log_info "$STEP MTL docker manager build and install"

		pushd "${root_folder}/manager" >/dev/null || exit 1

		if [ -n "${http_proxy:-}" ] && [ -n "${https_proxy:-}" ]; then
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest --build-arg HTTP_PROXY="${http_proxy:-}" --build-arg HTTPS_PROXY="${https_proxy:-}" -f Dockerfile ..
		else
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest -f Dockerfile ..
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_JPEGXS}" == "1" ]; then
		log_info "$STEP Plugin JPEG-XS build and install"
		export SVT_JPEG_XS_REPO="${root_folder}/script/SVT-JPEG-XS"
		bash "${root_folder}/script/mtl_build_jpegxs.sh"
		STEP=$((STEP + 1))
	fi

	# After MTL build. Both scripts read MTL_INSTALL_PREFIX themselves and install
	# into the ffmpeg and gstreamer siblings of its parent.
	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem FFMPEG plugin build and install"
		ffmpeg_build_options=()
		[ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" != "1" ] || ffmpeg_build_options+=(--gpu-direct)
		# --jpegxs needs a JPEG XS bundle, not a JPEG XS build. The two are the
		# same thing on a clean host, but not under per-component caching: JPEG XS
		# can be restored while FFmpeg is rebuilt, and the plugin must still link it.
		jpegxs_bundle=""
		[ -z "${MTL_INSTALL_PREFIX:-}" ] || jpegxs_bundle="$(dirname "${MTL_INSTALL_PREFIX}")/jpegxs"
		if [ "${PLUGIN_BUILD_AND_INSTALL_JPEGXS}" == "1" ] || [ -d "${jpegxs_bundle}" ]; then
			ffmpeg_build_options+=(--jpegxs)
		fi
		bash "${root_folder}/script/mtl_build_ffmpeg_plugin.sh" "${ffmpeg_build_options[@]}"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem GStreamer plugin build and install"
		# The test plugins of tests/tools/gstreamer_tools go beside the MTL ones.
		if [ "${ECOSYSTEM_BUILD_GSTREAMER_PLUGIN_RELEASE}" == "1" ]; then
			# Only a non-debug build has -Werror.
			bash "${root_folder}/script/mtl_build_gstreamer_plugin.sh" release --test-tools
		else
			# A debug plugin has tx-test-mode, which the anc_format tests use.
			bash "${root_folder}/script/mtl_build_gstreamer_plugin.sh" debug --test-tools
		fi
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem OBS plugin build and install"
		pushd "${root_folder}/ecosystem/obs_mtl" >/dev/null || exit 1
		pushd linux-mtl >/dev/null || exit 1
		meson setup build
		meson compile -C build
		as_root meson install -C build
		popd >/dev/null
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_SAMPLE}" == "1" ]; then
		log_info "$STEP Plugin sample build and install"
		pushd "${root_folder}/plugins" >/dev/null || exit 1
		meson setup build
		meson compile -C build
		as_root meson install -C build
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}" == "1" ]; then
		log_info "$STEP Plugin AVCODEC build and install"
		# st22 avcodec plugin installs to a sibling directory for independent caching.
		# It links libavcodec/libavutil, provided by the .local_install/ffmpeg build.
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			MTL_PLUGIN_PREFIX="${local_base}/plugins" \
				bash "${root_folder}/script/mtl_build_st22_avcodec_plugin.sh"
		else
			bash "${root_folder}/script/mtl_build_st22_avcodec_plugin.sh"
		fi
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_PYTHON}" == "1" ]; then
		log_info "$STEP Hook Python"
		pushd "${root_folder}" >/dev/null || exit 1
		if [ -d swig ]; then
			log_info "SWIG directory already exists, skipping clone."
		else
			log_info "Cloning SWIG repository..."
			git clone https://github.com/swig/swig.git
		fi
		pushd swig >/dev/null || exit 1
		git checkout v4.1.1
		./autogen.sh
		./configure
		make -j"${NPROC}"
		as_root make install
		popd >/dev/null
		pushd "${root_folder}/python/swig" >/dev/null || exit 1
		swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i
		python3 setup.py build_ext --inplace
		as_root python3 setup.py install
		popd >/dev/null
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_RUST}" == "1" ]; then
		log_info "$STEP Hook Rust"
		pushd "${root_folder}/rust" >/dev/null || exit 1
		cargo update home --precise "${RUST_HOOK_CARGO_VER}"
		cargo build --release
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_MONITORS}" == "1" ]; then
		log_info "$STEP Tools MTL monitors build"
		pushd "${root_folder}/tools/ebpf" >/dev/null || exit 1
		make lcore_monitor -j"${NPROC}"
		make udp_monitor -j"${NPROC}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP}" == "1" ]; then
		log_info "$STEP Tools MTL readpcap build"
		pushd "${root_folder}/tools/readpcap" >/dev/null || exit 1
		make -j"${NPROC}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR}" == "1" ]; then
		log_info "$STEP Tools MTL CPU emulator build"
		pushd "${root_folder}/tools/sch_smi_emulate" >/dev/null || exit 1
		make -j"${NPROC}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET}" == "1" ]; then
		log_info "$STEP Tools set_tai_offset build"
		pushd "${root_folder}/tools/set_tai_offset" >/dev/null || exit 1
		meson setup build || true
		ninja -C build -j"${NPROC}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_RUN_SET_TAI_OFFSET}" == "1" ]; then
		log_info "$STEP Tools set_tai_offset run"
		tai_bin="${root_folder}/tools/set_tai_offset/build/set_tai_offset"
		if [ ! -x "${tai_bin}" ]; then
			log_info "set_tai_offset not built, building first..."
			pushd "${root_folder}/tools/set_tai_offset" >/dev/null || exit 1
			meson setup build || true
			ninja -C build -j"${NPROC}"
			popd >/dev/null
		fi
		as_root "${tai_bin}" -v -0
		STEP=$((STEP + 1))
	fi

	{ set +x; } 2>/dev/null
	log_info "Enabled setup options:"
	print_selected

	log_success "Setup installation was successful"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
