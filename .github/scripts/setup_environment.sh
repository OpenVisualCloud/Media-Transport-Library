#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root_folder="$(cd -- "${script_folder}/../.." && pwd)"
# shellcheck disable=SC1091
. "${root_folder}/script/common.sh"

# SET DEFAULT ARGUMENTS

# When set, all build scripts install to this prefix instead of system-wide.
# This replaces the old SKIP_INSTALL logic with a unified local-install approach.
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

setup_build_drivers_options=""
setup_build_ice=0
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
: "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN:=0}"
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

# The flags, for the help text and for the summary at the end.
setup_flags=(
	"SETUP_ENVIRONMENT:Environment bootstrap"
	"SETUP_BUILD_AND_INSTALL_DPDK:DPDK build/install"
	"SETUP_BUILD_AND_INSTALL_DRIVERS:Driver build/install"
	"SETUP_BUILD_AND_INSTALL_DRIVERS_ICE:ICE driver flow"
	"SETUP_BUILD_AND_INSTALL_DRIVERS_IGC:IGC driver flow"
	"SETUP_BUILD_AND_INSTALL_EBPF_XDP:eBPF/XDP toolchain"
	"SETUP_BUILD_AND_INSTALL_GPU_DIRECT:GPU Direct support"
	"MTL_BUILD_AND_INSTALL_DEBUG:MTL debug build"
	"MTL_BUILD_AND_INSTALL:MTL release build"
	"MTL_BUILD_AND_INSTALL_FUZZ:MTL fuzzing build"
	"MTL_BUILD_AND_INSTALL_UNIT_TESTS:MTL unit tests build and run"
	"MTL_BUILD_AND_INSTALL_DOCKER:MTL Docker image"
	"MTL_BUILD_AND_INSTALL_DOCKER_MANAGER:MTL manager Docker image"
	"ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:FFmpeg plugin"
	"ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:GStreamer plugin"
	"ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN:RIST plugin"
	"ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:OBS plugin"
	"PLUGIN_BUILD_AND_INSTALL_SAMPLE:Sample plugin"
	"PLUGIN_BUILD_AND_INSTALL_AVCODEC:AVCodec plugin"
	"PLUGIN_BUILD_AND_INSTALL_JPEGXS:JPEG-XS plugin"
	"HOOK_PYTHON:Python hook"
	"HOOK_RUST:Rust hook"
	"TOOLS_BUILD_AND_INSTALL_MTL_MONITORS:MTL monitors"
	"TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:MTL readpcap"
	"TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR:MTL CPU emulator"
	"TOOLS_BUILD_AND_INSTALL_SET_TAI_OFFSET:set_tai_offset tool"
	"TOOLS_RUN_SET_TAI_OFFSET:set_tai_offset run"
	"CICD_BUILD:Non interactive mode"
)

show_help() {
	local entry
	cat <<EOF
Usage: ${script_name} [-h]

Set up a host for MTL: install the packages, then build and install the
components that the environment variables below select. Each variable is 0
(off, the default) or 1 (on). A shell can also source the script: then it only
sets the defaults.

REQUIRED PACKAGES (Debian/Ubuntu):
	sudo, when the script does not run as root. SETUP_ENVIRONMENT=1 runs
	script/install_dependencies.sh once, with each selected component.

OPTIONS:
	-h, --help	Show this help message

ENVIRONMENT:
	MTL_INSTALL_PREFIX				Install each component into a sibling directory
							of this path instead of the system
EOF
	for entry in "${setup_flags[@]}"; do
		printf '\t%-45s\t%s\n' "${entry%%:*}" "${entry#*:}"
	done
	cat <<EOF

EXAMPLES:
	SETUP_ENVIRONMENT=1 SETUP_BUILD_AND_INSTALL_DPDK=1 MTL_BUILD_AND_INSTALL=1 ${script_name}
	MTL_INSTALL_PREFIX="\$PWD/.local_install/mtl" SETUP_BUILD_AND_INSTALL_DPDK=1 ${script_name}
EOF
}

main() {
	local STEP=1 mtl_build_options local_base enable_gpu enable_jpegxs jpegxs_bundle tai_bin
	local entry var printed=0
	local -a mtl_build_env jpegxs_build_options

	case "${1:-}" in
	"") ;;
	-h | --help)
		show_help
		exit 0
		;;
	*)
		log_error "Unexpected argument: $1. The environment variables select the work."
		show_help
		exit 1
		;;
	esac

	# The trace shows each step in a CI log. A shell that sources the script
	# does not get it.
	set -x

	if [ "$SETUP_ENVIRONMENT" == "1" ]; then
		log_info "$STEP Environment setup."

		local -a dependencies=()
		if [ "${setup_build_ice}" == "1" ]; then dependencies+=(ice); fi
		for entry in SETUP_BUILD_AND_INSTALL_EBPF_XDP:ebpf_xdp SETUP_BUILD_AND_INSTALL_GPU_DIRECT:gpu_direct \
			ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:ffmpeg PLUGIN_BUILD_AND_INSTALL_JPEGXS:jpegxs \
			ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:gstreamer ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:obs \
			HOOK_PYTHON:python HOOK_RUST:rust TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:readpcap; do
			var=${entry%%:*}
			if [ "${!var}" == "1" ]; then dependencies+=("${entry#*:}"); fi
		done
		bash "${root_folder}/script/install_dependencies.sh" "${dependencies[@]}"
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
		bash "${root_folder}/script/build_ebpf_xdp.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DPDK}" == "1" ]; then
		log_info "$STEP DPDK build and install"
		# DPDK installs to a sibling directory for independent caching
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			MTL_INSTALL_PREFIX="${local_base}/dpdk" bash "${root_folder}/script/build_dpdk.sh" -f
		else
			bash "${root_folder}/script/build_dpdk.sh" -f
		fi
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DRIVERS}" == "1" ]; then
		log_info "$STEP Driver build and install"
		# shellcheck disable=SC2086
		bash "${root_folder}/script/build_drivers.sh" ${setup_build_drivers_options}
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
			docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY="${http_proxy:-}" --build-arg HTTPS_PROXY="${https_proxy:-}" ../
		else
			docker build -t mtl:latest -f ubuntu.dockerfile ../
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER}" == "1" ]; then
		log_info "$STEP MTL docker manager build and install"

		pushd "${root_folder}/manager" >/dev/null || exit 1

		if [ -n "${http_proxy:-}" ] && [ -n "${https_proxy:-}" ]; then
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest --build-arg HTTP_PROXY="${http_proxy:-}" --build-arg HTTPS_PROXY="${https_proxy:-}" .
		else
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest .
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_JPEGXS}" == "1" ]; then
		log_info "$STEP Plugin JPEG-XS bundle build"
		export SVT_JPEG_XS_REPO="${root_folder}/script/SVT-JPEG-XS"
		jpegxs_build_options=()
		[ "${CICD_BUILD}" != "1" ] || jpegxs_build_options+=(--ci)
		bash "${root_folder}/script/build_jpegxs.sh" "${jpegxs_build_options[@]}"
		STEP=$((STEP + 1))
	fi

	# After MTL build
	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem FFMPEG plugin build and install"
		if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ]; then
			log_info "Building FFMPEG plugin with GPU Direct support"
			enable_gpu="-g"
		else
			log_info "Building FFMPEG plugin without GPU Direct support"
			enable_gpu=""
		fi

		# -j needs a JPEG XS bundle, not a JPEG XS build. The two are the same thing
		# on a clean host, but not under per-component caching: JPEG XS can be
		# restored while FFmpeg is rebuilt, and the plugin must still link it.
		jpegxs_bundle=""
		[ -z "${MTL_INSTALL_PREFIX:-}" ] || jpegxs_bundle="$(dirname "${MTL_INSTALL_PREFIX}")/jpegxs"
		if [ "${PLUGIN_BUILD_AND_INSTALL_JPEGXS}" == "1" ] || [ -d "${jpegxs_bundle}" ]; then
			export FFMPEG_ENABLE_SVT_JPEG_XS="1"
			enable_jpegxs="-j"
		else
			enable_jpegxs=""
		fi

		# FFmpeg installs to a sibling directory for independent caching
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			MTL_INSTALL_PREFIX="${local_base}/ffmpeg" bash "${root_folder}/ecosystem/ffmpeg_plugin/build.sh" ${enable_gpu} ${enable_jpegxs}
		else
			bash "${root_folder}/ecosystem/ffmpeg_plugin/build.sh" ${enable_gpu} ${enable_jpegxs}
		fi
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem GStreamer plugin build and install"

		# GStreamer plugins go to a sibling directory
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			MTL_INSTALL_PREFIX="${local_base}/gstreamer" bash "${root_folder}/ecosystem/gstreamer_plugin/build.sh"
		else
			pushd "${root_folder}/ecosystem/gstreamer_plugin" >/dev/null || exit 1
			bash build.sh
			popd >/dev/null
		fi

		pushd "${root_folder}/tests/tools/gstreamer_tools/" >/dev/null || exit 1
		meson setup builddir
		ninja -C builddir/

		# Copy test tool .so files alongside gstreamer plugins
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			mkdir -p "${local_base}/gstreamer"
			cp builddir/*.so "${local_base}/gstreamer/"
		fi
		cp builddir/*.so "${root_folder}/ecosystem/gstreamer_plugin/builddir/"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN}" == "1" ]; then
		log_info "$STEP Ecosystem RIST plugin build and install"
		if [ -n "${MTL_INSTALL_PREFIX:-}" ]; then
			local_base="$(dirname "${MTL_INSTALL_PREFIX}")"
			MTL_INSTALL_PREFIX="${local_base}/librist" bash "${root_folder}/ecosystem/librist/build_librist_mtl.sh"
		else
			bash "${root_folder}/ecosystem/librist/build_librist_mtl.sh"
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
				PKG_CONFIG_PATH="${local_base}/ffmpeg/lib/pkgconfig:${local_base}/ffmpeg/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}" \
				LD_LIBRARY_PATH="${local_base}/ffmpeg/lib:${local_base}/ffmpeg/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" \
				bash "${root_folder}/script/build_st22_avcodec_plugin.sh"
		else
			bash "${root_folder}/script/build_st22_avcodec_plugin.sh"
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

	log_info "Enabled setup options:"
	for entry in "${setup_flags[@]}"; do
		var=${entry%%:*}
		[ "${!var:-0}" = "0" ] && continue
		printed=1
		if [ "${!var}" = "1" ]; then
			log_info "  ${entry#*:}"
		else
			log_info "  ${entry#*:} (export ${var}=${!var})"
		fi
	done
	[ "$printed" = "1" ] || log_info "  (none)"

	log_success "Setup installation was successful"
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
