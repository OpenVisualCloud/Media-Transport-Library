#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -xe

# Allow pip to modify system packages when run outside a venv (Debian/Ubuntu
# set environments as externally managed).
export PIP_BREAK_SYSTEM_PACKAGES=1

# SET DEFAULT ARGUMENTS

# Before MTL build install
: "${SETUP_ENVIRONMENT:=1}"
: "${SETUP_BUILD_AND_INSTALL_DPDK:=1}"
: "${SETUP_BUILD_AND_INSTALL_ICE_DRIVER:=1}"
: "${SETUP_BUILD_AND_INSTALL_EBPF_XDP:=1}"
: "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT:=1}"

# MTL build and install
: "${MTL_BUILD_AND_INSTALL_DEBUG:=0}"
: "${MTL_BUILD_AND_INSTALL:=0}"
: "${MTL_BUILD_AND_INSTALL_DOCKER:=0}"
: "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER:=0}"
: "${MTL_BUILD_AND_INSTALL_FUZZ:=0}"

# After MTL build
: "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:=0}"

: "${PLUGIN_BUILD_AND_INSTALL_SAMPLE:=0}"
: "${PLUGIN_BUILD_AND_INSTALL_PLUGIN_AVCODEC:=0}"

: "${HOOK_PYTHON:=0}"
: "${HOOK_RUST:=0}"

: "${TOOLS_BUILD_AND_INSTALL_MTL_MONITORS:=0}"
: "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:=0}"
: "${TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR:=0}"

# CICD ONLY ARGUMENTS
: "${CICD_BUILD:=0}"
: "${CICD_BUILD_BUILD_ICE_DRIVER:=0}"

script_name=$(basename "${BASH_SOURCE[0]}")
script_path=$(readlink -qe "${BASH_SOURCE[0]}")
setup_script_folder=${script_path/$script_name/}
root_folder="${setup_script_folder}/../.."
nproc=$(nproc 2>/dev/null || echo 50)
# shellcheck disable=SC1091
. "${root_folder}/script/common.sh"

# Before MTL build install
function setup_ubuntu_install_dependencies() {
	echo "1.1. Install the build dependency from OS software store"

	# Mtl library dependencies
	sudo apt update
	sudo apt install -y \
		git \
		gcc \
		meson \
		python3 \
		python3-pip \
		pkg-config \
		libnuma-dev \
		libjson-c-dev \
		libpcap-dev \
		libgtest-dev \
		libssl-dev \
		systemtap-sdt-dev \
		llvm \
		clang \
		libsdl2-dev \
		libsdl2-ttf-dev \
		cmake \
		linuxptp \
		ethtool \
		netsniff-ng

	# CiCd only
	if [ "${CICD_BUILD}" == "1" ]; then
		sudo apt install -y tzdata python3-venv wget doxygen
		sudo ln -fs /usr/share/zoneinfo/Europe/Warsaw /etc/localtime
		sudo dpkg-reconfigure -f noninteractive tzdata
		python3 -m venv /tmp/mtl-venv
		# shellcheck disable=SC1091
		. /tmp/mtl-venv/bin/activate
		git config --global user.email "you@example.com"
		git config --global user.name "Your Name"
	fi

	python3 -m pip install --upgrade pip
	python3 -m pip install pyelftools ninja

	# Ice driver dependencies
	if [ "${SETUP_BUILD_AND_INSTALL_ICE_DRIVER}" == "1" ]; then
		echo "Installing Ice driver dependencies"

		if ! sudo apt install -y "linux-headers-$(uname -r)"; then
			if [ "${CICD_BUILD}" != "0" ]; then
				ret=0
			else
				log_error "Error: Failed to install linux-headers-$(uname -r)."
				echo "Do you want to try installing the generic linux-headers-generic package instead?"
				echo "It may work for your system, but it is not guaranteed."
				ret=$(get_user_input_confirm)
			fi

			if [ "$ret" == "1" ]; then
				if ! sudo apt install -y linux-headers-generic; then
					log_error "Error: Failed to install linux-headers-generic as well."
				else
					log_warning "Installed linux-headers-generic."
				fi
			else
				log_warning "Installation aborted by user.."
				exit 0
			fi
		fi
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP}" == "1" ]; then
		echo "Installing eBPF/XDP dependencies"
		sudo apt install -y \
			make \
			m4 \
			zlib1g-dev \
			libelf-dev \
			libcap-ng-dev \
			libcap2-bin \
			gcc-multilib # clang llvm
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ]; then
		echo "Installing GPU Direct dependencies"
		ONE_API_TGZ="oneapi.tgz"

		sudo apt install -y file

		wget "${ONE_API_REPO}" -O "${ONE_API_TGZ}"
		if [ -f "${ONE_API_TGZ}" ]; then
			tar -xzf "${ONE_API_TGZ}"
			rm "${ONE_API_TGZ}"
			echo "OneAPI installed to /opt"
		else
			log_error "Error: Failed to download OneAPI repository."
			exit 1
		fi

		pushd "level-zero-${ONE_API_GPU_VER}" >/dev/null || exit 1

		if mkdir build; then
			rm -rf build
			mkdir build
		fi
		pushd build >/dev/null || exit 1
		cmake .. -D CMAKE_BUILD_TYPE=Release
		cmake --build . --target package -j"${nproc}"
		sudo cmake --build . --target install -j"${nproc}"
		popd >/dev/null
		popd >/dev/null
		rm -rf "${setup_script_folder}/level-zero-${ONE_API_GPU_VER}"
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" == "1" ]; then
		echo "Installing FFMPEG dependencies"
		sudo apt install -y \
			nasm \
			unzip \
			patch
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ]; then
		echo "Installing GStreamer dependencies"
		sudo apt install -y \
			libunwind-dev \
			gstreamer1.0-plugins-base \
			libgstreamer-plugins-base1.0-dev \
			gstreamer1.0-plugins-good \
			gstreamer1.0-tools \
			gstreamer1.0-libav \
			libgstreamer1.0-dev
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN}" == "1" ]; then
		echo "Installing OBS dependencies"
		sudo apt install -y \
			libobs-dev
	fi

	if [ "${HOOK_PYTHON}" == "1" ]; then
		echo "Installing Python hook dependencies"
		sudo apt install -y \
			swig \
			automake \
			yacc

		python3 -m pip install setuptools
	fi

	if [ "${HOOK_RUST}" == "1" ]; then
		echo "Installing Rust hook dependencies"
		sudo apt install -y \
			cargo \
			rustc
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP}" == "1" ]; then
		echo "Installing MTL readpcap dependencies"
		sudo apt install -y \
			libpcap-dev
	fi

	sudo ldconfig
	echo -e "${GREEN}All dependencies installed successfully."
}

# Allow sourcing of the script.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then

	if [ "$SETUP_ENVIRONMENT" == "1" ]; then
		echo "$STEP Environment setup."

		if [ -f /etc/os-release ]; then
			# shellcheck disable=SC1091
			. /etc/os-release
			case "$ID" in
			ubuntu)
				echo "Detected OS: Ubuntu"
				setup_ubuntu_install_dependencies
				;;
			centos)
				echo "Detected OS: CentOS"
				echo "For now unsuported OS, please use Ubuntu"
				exit 2
				;;
			rhel)
				echo "Detected OS: RHEL"
				echo "For now unsuported OS, please use Ubuntu"
				exit 2
				;;
			rockos | rocky)
				echo "Detected OS: Rocky Linux"
				echo "For now unsuported OS, please use Ubuntu"
				exit 2
				;;
			*)
				echo "OS not recognized: $ID"
				echo "For now unsuported OS, please use Ubuntu"
				exit 2
				;;
			esac
		else
			echo "/etc/os-release not found. Cannot determine OS."
			exit 2
		fi
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ]; then
		echo "$STEP Install the build dependency for GPU Direct"
		# shellcheck disable=SC1091
		pushd "${root_folder}/gpu_direct" >/dev/null || exit 1

		if [[ ":$LIBRARY_PATH:" != *":/usr/local/lib:"* ]]; then
			export LIBRARY_PATH="/usr/local/lib:$LIBRARY_PATH"
		fi

		meson setup build
		sudo meson install -C build

		if pkg-config --libs mtl_gpu_direct >/dev/null 2>&1; then
			echo "mtl_gpu_direct is available via pkg-config."
		else
			echo "mtl_gpu_direct is NOT available via pkg-config."
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP}" == "1" ]; then
		echo "$STEP Install the build dependency from OS software store"
		bash "${root_folder}/script/build_ebpf_xdp.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DPDK}" == "1" ]; then
		echo "$STEP DPDK build and install"
		bash "${root_folder}/script/build_dpdk.sh" -f
		STEP=$((STEP + 1))
	fi

	if [ "${CICD_BUILD_BUILD_ICE_DRIVER}" == "1" ]; then
		echo "$STEP ICE driver build"
		# shellcheck disable=SC1091
		. "${root_folder}/script/build_ice_driver.sh"
		if [ -z "$setup_script_folder" ] || [ -z "$ICE_VER" ] || [ -z "$ICE_DMID" ]; then
			exit 3
		fi
		pushd "${setup_script_folder}" >/dev/null || exit 1

		echo "Building e810 driver version: $ICE_VER form mirror $ICE_DMID"

		wget "https://downloadmirror.intel.com/${ICE_DMID}/ice-${ICE_VER}.tar.gz"
		tar xvzf "ice-${ICE_VER}.tar.gz"
		pushd "ice-${ICE_VER}" >/dev/null || exit 1

		for patch_file in "${root_folder}"/patches/ice_drv/"${ICE_VER}"/*.patch; do
			patch -p1 -i "$patch_file"
		done

		pushd src >/dev/null || exit 1
		make -j"${nproc}"
		popd >/dev/null
		popd >/dev/null
		rm -rf "ice-${ICE_VER}" "ice-${ICE_VER}.tar.gz"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_ICE_DRIVER}" == "1" ]; then
		echo "$STEP ICE driver build and install"
		bash "${root_folder}/script/build_ice_driver.sh"
		STEP=$((STEP + 1))
	fi

	# MTL build and install

	if [ "${MTL_BUILD_AND_INSTALL_DEBUG}" == "1" ]; then
		echo "$STEP MTL debug build and install"
		pushd "${root_folder}" >/dev/null || exit 1
		./build.sh debug
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	# If both are enabled we build debug but overwrite with release
	if [ "${MTL_BUILD_AND_INSTALL}" == "1" ]; then
		echo "$STEP MTL build and install"
		pushd "${root_folder}" >/dev/null || exit 1
		./build.sh
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_FUZZ}" == "1" ]; then
		echo "$STEP MTL fuzzing build and install"
		cd "${root_folder}" || exit 1
		MTL_BUILD_ENABLE_FUZZING=true ./build.sh release enable_fuzzing
		STEP=$((STEP + 1))
	fi
	# MTL build and install
	mtl_build_options=""

	if [[ "${MTL_BUILD_AND_INSTALL_FUZZ}" == "1" ]]; then
		echo "$STEP enable MTL_fuzzing=true"
		mtl_build_options="${mtl_build_options} enable_fuzzing"
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DEBUG}" == "1" ]; then
		echo "$STEP MTL debug build and install"
		pushd "${root_folder}" >/dev/null || exit 1
		./build.sh debug "${mtl_build_options}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	# If both are enabled we build debug but overwrite with release
	if [ "${MTL_BUILD_AND_INSTALL}" == "1" ]; then
		echo "$STEP MTL build and install"
		pushd "${root_folder}" >/dev/null || exit 1
		./build.sh "${mtl_build_options}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER}" == "1" ]; then
		echo "$STEP MTL docker build and install"
		pushd "${root_folder}/docker" >/dev/null || exit 1

		if [ -n "${http_proxy}" ] && [ -n "${https_proxy}" ]; then
			docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY="${http_proxy}" --build-arg HTTPS_PROXY="${https_proxy}" ../
		else
			docker build -t mtl:latest -f ubuntu.dockerfile ../
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER}" == "1" ]; then
		echo "$STEP MTL docker manager build and install"

		pushd "${root_folder}/manager" >/dev/null || exit 1

		if [ -n "${http_proxy}" ] && [ -n "${https_proxy}" ]; then
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest --build-arg HTTP_PROXY="${http_proxy}" --build-arg HTTPS_PROXY="${https_proxy}" .
		else
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest .
		fi

		popd >/dev/null

		STEP=$((STEP + 1))
	fi

	# After MTL build
	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem FFMPEG plugin build and install"
		if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ]; then
			echo "Building FFMPEG plugin with GPU Direct support"
			enable_gpu="-g"
		else
			echo "Building FFMPEG plugin without GPU Direct support"
		fi

		bash "${root_folder}/ecosystem/ffmpeg_plugin/build.sh" "${enable_gpu}"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem GStreamer plugin build and install"

		pushd "${root_folder}/ecosystem/gstreamer_plugin" >/dev/null || exit 1
		bash build.sh
		popd >/dev/null

		pushd "${root_folder}/tests/tools/gstreamer_tools/" >/dev/null || exit 1
		meson setup builddir
		ninja -C builddir/
		cp builddir/*.so "${root_folder}/ecosystem/gstreamer_plugin/builddir/"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem RIST plugin build and install"
		bash "${root_folder}/ecosystem/librist/build_librist_mtl.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem RIST plugin build and install"
		bash "${root_folder}/ecosystem/librist/build_librist_mtl.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem OBS plugin build and install"
		pushd "${root_folder}/ecosystem/obs_mtl" >/dev/null || exit 1
		pushd linux-mtl >/dev/null || exit 1
		meson setup build
		meson compile -C build
		sudo meson install -C build
		popd >/dev/null
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_SAMPLE}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		pushd "${root_folder}/plugins" >/dev/null || exit 1
		meson setup build
		meson compile -C build
		sudo meson install -C build
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		bash "${root_folder}/script/build_st22_avcodec_plugin.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		"${root_folder}/script/build_st22_avcodec_plugin.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_PYTHON}" == "1" ]; then
		echo "$STEP Hook Python"
		pushd "${root_folder}" >/dev/null || exit 1
		if [ -d swig ]; then
			echo "SWIG directory already exists, skipping clone."
		else
			echo "Cloning SWIG repository..."
			git clone https://github.com/swig/swig.git
		fi
		pushd swig >/dev/null || exit 1
		git checkout v4.1.1
		./autogen.sh
		./configure
		make -j"${nproc}"
		sudo make install
		popd >/dev/null
		pushd "${root_folder}/python/swig" >/dev/null || exit 1
		swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i
		python3 setup.py build_ext --inplace
		sudo python3 setup.py install
		popd >/dev/null
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_RUST}" == "1" ]; then
		echo "$STEP Hook Rust"
		pushd "${root_folder}/rust" >/dev/null || exit 1
		cargo update home --precise "${RUST_HOOK_CARGO_VER}"
		cargo build --release
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_MONITORS}" == "1" ]; then
		echo "$STEP Tools MTL monitors build"
		pushd "${root_folder}/tools/ebpf" >/dev/null || exit 1
		make lcore_monitor -j"${nproc}"
		make udp_monitor -j"${nproc}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP}" == "1" ]; then
		echo "$STEP Tools MTL readpcap build"
		pushd "${root_folder}/tools/readpcap" >/dev/null || exit 1
		make -j"${nproc}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR}" == "1" ]; then
		echo "$STEP Tools MTL CPU emulator build"
		pushd "${root_folder}/tools/sch_smi_emulate" >/dev/null || exit 1
		make -j"${nproc}"
		popd >/dev/null
		STEP=$((STEP + 1))
	fi

	echo "Selected setup options:"
	show_flag() {
		local name="$1"
		local value="$2"
		local desc="$3"
		local status="disabled"
		if [ "$value" = "1" ]; then
			status="enabled"
		elif [ "$value" != "0" ]; then
			status="custom (${value})"
		fi
		printf "  %-45s -> %s (export %s=%s)\n" "$desc" "$status" "$name" "$value"
	}

	echo "Enabled setup options:"
	printed=0
	for entry in \
		"SETUP_ENVIRONMENT:Environment bootstrap" \
		"SETUP_BUILD_AND_INSTALL_DPDK:DPDK build/install" \
		"SETUP_BUILD_AND_INSTALL_ICE_DRIVER:ICE driver build/install" \
		"SETUP_BUILD_AND_INSTALL_EBPF_XDP:eBPF/XDP toolchain" \
		"SETUP_BUILD_AND_INSTALL_GPU_DIRECT:GPU Direct support" \
		"MTL_BUILD_AND_INSTALL_DEBUG:MTL debug build" \
		"MTL_BUILD_AND_INSTALL:MTL release build" \
		"MTL_BUILD_AND_INSTALL_DOCKER:MTL Docker image" \
		"MTL_BUILD_AND_INSTALL_DOCKER_MANAGER:MTL manager Docker image" \
		"ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:FFmpeg plugin" \
		"ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:GStreamer plugin" \
		"ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN:RIST plugin" \
		"ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN:OBS plugin" \
		"PLUGIN_BUILD_AND_INSTALL_SAMPLE:Sample plugin" \
		"PLUGIN_BUILD_AND_INSTALL_PLUGIN_AVCODEC:AVCodec plugin" \
		"HOOK_PYTHON:Python hook" \
		"HOOK_RUST:Rust hook" \
		"TOOLS_BUILD_AND_INSTALL_MTL_MONITORS:MTL monitors" \
		"TOOLS_BUILD_AND_INSTALL_MTL_READPCAP:MTL readpcap" \
		"TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR:MTL CPU emulator" \
		"CICD_BUILD:CICD mode" \
		"CICD_BUILD_BUILD_ICE_DRIVER:CICD ICE driver build"; do

		var=${entry%%:*}
		desc=${entry#*:}
		val=${!var:-0}
		[ "$val" = "0" ] && continue
		printed=1
		if [ "$val" = "1" ]; then
			echo "  $desc"
		else
			echo "  $desc (export $var=$val)"
		fi
	done
	[ "$printed" = "0" ] && echo "  (none)"

	echo "Setup installation was successful"
fi # End of execution block for script
