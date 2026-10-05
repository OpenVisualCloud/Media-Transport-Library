#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

set -euo pipefail

script_name="$(basename "${BASH_SOURCE[0]}")"
script_folder="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
. "${script_folder}/common.sh"

show_help() {
	cat <<EOF
Usage: ${script_name} [OPTIONS] [COMPONENT...]

Install the Ubuntu packages that the MTL build needs, then the packages of each
COMPONENT. Run it as your own user: it uses sudo to install.

COMPONENT is one of:
	ice		Linux headers of the running kernel, for build_drivers.sh
	ebpf_xdp	Build packages of build_ebpf_xdp.sh
	gpu_direct	Level Zero ${ONE_API_GPU_VER}, built from source
	ffmpeg		FFmpeg plugin build packages
	jpegxs		JPEG XS plugin build packages
	gstreamer	GStreamer plugin build packages
	obs		OBS Studio plugin build packages
	python		Python binding build packages, for build_python.sh
	rust		Rust binding build packages
	readpcap	MTL readpcap tool build packages

REQUIRED PACKAGES (Debian/Ubuntu):
	sudo, when the script does not run as root

OPTIONS:
	-h, --help	Show this help message

EXAMPLES:
	${script_name}			# The MTL build packages only
	${script_name} ice ebpf_xdp	# Also the ICE driver and eBPF/XDP packages
EOF
}

install_gpu_direct() {
	local tgz="oneapi.tgz"

	install_packages file
	wget "${ONE_API_REPO}" -O "${tgz}"
	tar -xzf "${tgz}"
	rm "${tgz}"
	pushd "level-zero-${ONE_API_GPU_VER}" >/dev/null
	rm -rf build
	mkdir build
	pushd build >/dev/null
	cmake .. -D CMAKE_BUILD_TYPE=Release
	cmake --build . --target package -j"${NPROC}"
	as_root cmake --build . --target install -j"${NPROC}"
	popd >/dev/null
	popd >/dev/null
	rm -rf "level-zero-${ONE_API_GPU_VER}"
}

main() {
	local component

	for component in "$@"; do
		case "${component}" in
		ice | ebpf_xdp | gpu_direct | ffmpeg | jpegxs | gstreamer | obs | python | rust | readpcap) ;;
		-h | --help)
			show_help
			exit 0
			;;
		*)
			log_error "Unknown component: ${component}"
			show_help
			exit 1
			;;
		esac
	done
	if [ "${ID:-}" != ubuntu ]; then
		log_error "OS not supported: ${ID:-unknown}. Use Ubuntu, or install the packages of doc/build.md."
		exit 2
	fi

	# Allow pip to modify system packages when run outside a venv (Debian/Ubuntu
	# set environments as externally managed).
	export PIP_BREAK_SYSTEM_PACKAGES=1

	as_root apt-get update
	install_packages git gcc meson python3 python3-pip pkg-config libnuma-dev \
		libjson-c-dev libpcap-dev libgtest-dev libssl-dev systemtap-sdt-dev llvm \
		clang libsdl2-dev libsdl2-ttf-dev cmake linuxptp ethtool netsniff-ng unzip
	python3 -m pip install --upgrade pip
	python3 -m pip install pyelftools ninja

	for component in "$@"; do
		log_info "Installing the ${component} packages"
		case "${component}" in
		ice) install_packages "linux-headers-$(uname -r)" ;;
		ebpf_xdp) install_packages make m4 zlib1g-dev libelf-dev libcap-ng-dev libcap2-bin gcc-multilib ;;
		gpu_direct) install_gpu_direct ;;
		ffmpeg) install_packages nasm unzip patch ;;
		jpegxs) install_packages cmake yasm nasm build-essential ;;
		gstreamer)
			install_packages libunwind-dev gstreamer1.0-plugins-base libgstreamer-plugins-base1.0-dev \
				gstreamer1.0-plugins-good gstreamer1.0-tools gstreamer1.0-libav libgstreamer1.0-dev
			;;
		obs) install_packages libobs-dev ;;
		python)
			install_packages swig automake yacc
			python3 -m pip install setuptools
			;;
		rust) install_packages cargo rustc ;;
		readpcap) install_packages libpcap-dev ;;
		esac
	done
	log_success "All dependencies installed."
}

(return 0 2>/dev/null) && sourced=1 || sourced=0
if [ "${sourced}" -eq 0 ]; then
	main "$@"
fi
