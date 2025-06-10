#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -xe

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

# After MTL build
: "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN:=0}"
: "${ECOSYSTEM_BUILD_AND_INSTALL_MSDK_PLUGIN:=0}"
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
script_folder=${script_path/$script_name/}
# shellcheck disable=SC1091
. "${script_folder}/../../script/common.sh"

if [ "$ECOSYSTEM_BUILD_AND_INSTALL_MSDK_PLUGIN" == "1" ]; then
	if [ "${CICD_BUILD}" != "0" ]; then
		ret=0
	else
		log_warning "Error: MSDK is not activly supported"
		ret=$(get_user_input_confirm)
	fi

	if [ "$ret" == "1" ]; then
		log_warning "Proceeding with MSDK plugin build, but this feature is not fully supported."
	else
		log_warning "Installation aborted by user.."
		exit 0
	fi
fi

# Before MTL build install
function setup_ubuntu_install_dependencies() {
	echo "1.1. Install the build dependency from OS software store"

	# Mtl library dependencies
	apt-get update
	apt-get install -y \
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
		cmake

	# CiCd only
	if [ "${CICD_BUILD}" == "1" ]; then
		apt install -y tzdata python3-venv sudo wget doxygen
		ln -fs /usr/share/zoneinfo/Europe/Warsaw /etc/localtime
		dpkg-reconfigure -f noninteractive tzdata
		python3 -m venv /tmp/mtl-venv
		# shellcheck disable=SC1091
		. /tmp/mtl-venv/bin/activate
		git config --global user.email "you@example.com"
		git config --global user.name "Your Name"
	fi

	pip install --upgrade pip
	pip install pyelftools ninja

	# Ice driver dependencies
	if [ "${SETUP_BUILD_AND_INSTALL_ICE_DRIVER}" == "1" ]; then
		echo "Installing Ice driver dependencies"

		if sudo apt-get install -y "linux-headers-$(uname -r)"; then
			if [ "${CICD_BUILD}" != "0" ]; then
				ret=0
			else
				log_error "Error: Failed to install linux-headers-$(uname -r)."
				echo "Do you want to try installing the generic linux-headers-generic package instead?"
				echo "It may work for your system, but it is not guaranteed."
				ret=$(get_user_input_confirm)
			fi

			if [ "$ret" == "1" ]; then
				if ! sudo apt-get install -y linux-headers-generic; then
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

	if [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing eBPF/XDP dependencies"
		sudo apt-get install -y \
			make \
			m4 \
			zlib1g-dev \
			libelf-dev \
			libcap-ng-dev \
			libcap2-bin \
			gcc-multilib # clang llvm
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_GPU_DIRECT}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing GPU Direct dependencies"
		ONE_API_TGZ="oneapi.tgz"

		apt-get install -y file

		wget "${ONE_API_REPO}" -O "${ONE_API_TGZ}"
		if [ -f "${ONE_API_TGZ}" ]; then
			tar -xzf "${ONE_API_TGZ}"
			rm "${ONE_API_TGZ}"
			echo "OneAPI installed to /opt"
		else
			log_error "Error: Failed to download OneAPI repository."
			exit 1
		fi

		cd "level-zero-${ONE_API_GPU_VER}" || exit 1

		if mkdir build; then
			rm -rf build
			mkdir build
		fi
		cd build || exit 1
		cmake .. -D CMAKE_BUILD_TYPE=Release
		cmake --build . --target package

		cmake --build . --target install
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_FFMPEG_PLUGIN}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing FFMPEG dependencies"
		sudo apt install -y \
			nasm
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing GStreamer dependencies"
		sudo apt install -y \
			gstreamer1.0-plugins-base \
			gstreamer1.0-plugins-good \
			gstreamer1.0-tools \
			gstreamer1.0-libav \
			libgstreamer1.0-dev
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_MSDK_PLUGIN}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing MSDK dependencies"
		sudo apt install -y \
			curl \
			libva-dev
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing OBS dependencies"
		sudo apt install -y \
			libobs-dev
	fi

	if [ "${HOOK_PYTHON}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing Python hook dependencies"
		sudo apt-get install -y \
			swig \
			automake \
			yacc

		pip install setuptools
	fi

	if [ "${HOOK_RUST}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing Rust hook dependencies"
		sudo apt-get install -y \
			cargo \
			rustc
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP}" == "1" ] || [ "${CICD_BUILD}" == "1" ]; then
		echo "Installing MTL readpcap dependencies"
		sudo apt-get install -y \
			libpcap-dev
	fi

	ldconfig
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
		cd "${script_folder}/../../gpu_direct" || exit 1

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

		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_EBPF_XDP}" == "1" ]; then
		echo "$STEP Install the build dependency from OS software store"
		bash "${script_folder}/../../script/build_ebpf_xdp.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_DPDK}" == "1" ]; then
		echo "$STEP DPDK build and install"
		bash "${script_folder}/../../script/build_dpdk.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${CICD_BUILD_BUILD_ICE_DRIVER}" == "1" ]; then
		echo "$STEP ICE driver build"
		# shellcheck disable=SC1091
		. "${script_folder}/../../script/build_ice_driver.sh"
		if [ -z "$script_folder" ] || [ -z "$ice_driver_ver" ] || [ -z "$download_mirror" ]; then
			exit 3
		fi
		cd "${script_folder}"

		echo "Building e810 driver version: $ice_driver_ver form mirror $download_mirror"

		wget "https://downloadmirror.intel.com/${download_mirror}/ice-${ice_driver_ver}.tar.gz"
		tar xvzf "ice-${ice_driver_ver}.tar.gz"
		cd "ice-${ice_driver_ver}"

		git init
		git add .
		git commit -m "init version ${ice_driver_ver}"
		git am ../../patches/ice_drv/"${ice_driver_ver}"/*.patch

		cd src
		make
		STEP=$((STEP + 1))
	fi

	if [ "${SETUP_BUILD_AND_INSTALL_ICE_DRIVER}" == "1" ]; then
		echo "$STEP ICE driver build and install"
		bash "${script_folder}/../../script/build_ice_driver.sh"
		STEP=$((STEP + 1))
	fi

	# MTL build and install

	if [ "${MTL_BUILD_AND_INSTALL_DEBUG}" == "1" ]; then
		echo "$STEP MTL debug build and install"
		bash "${script_folder}/../../build.sh" "debug"
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL}" == "1" ]; then
		echo "$STEP MTL build and install"
		bash "${script_folder}/../../build.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER}" == "1" ]; then
		echo "$STEP MTL docker build and install"
		cd "${script_folder}/../../docker" || exit 1

		if [ -z "${http_proxy}" ] && [ -z "${https_proxy}" ]; then
			docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY="${http_proxy}" --build-arg HTTPS_PROXY="${https_proxy}" ../
		else
			docker build -t mtl:latest -f ubuntu.dockerfile ../
		fi

		STEP=$((STEP + 1))
	fi

	if [ "${MTL_BUILD_AND_INSTALL_DOCKER_MANAGER}" == "1" ]; then
		echo "$STEP MTL docker manager build and install"

		cd "${script_folder}/../../manager" | exit 1

		if [ -z "${http_proxy}" ] && [ -z "${https_proxy}" ]; then
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest --build-arg HTTP_PROXY="${http_proxy}" --build-arg HTTPS_PROXY="${https_proxy}" .
		else
			docker build --build-arg VERSION="$(cat ../VERSION)" -t mtl-manager:latest .
		fi

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

		bash "${script_folder}/../../ecosystem/ffmpeg_plugin/build.sh" "${enable_gpu}"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_GSTREAMER_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem GStreamer plugin build and install"

		bash "${script_folder}/../../ecosystem/gstreamer_plugin/build.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem RIST plugin build and install"
		bash "${script_folder}/../../ecosystem/librist/build_librist_mtl.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_RIST_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem RIST plugin build and install"
		bash "${script_folder}/../../ecosystem/librist/build_librist_mtl.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_MSDK_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem RIST plugin build and install"
		bash "${script_folder}/../../ecosystem/msdk/build_msdk_mtl.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${ECOSYSTEM_BUILD_AND_INSTALL_OBS_PLUGIN}" == "1" ]; then
		echo "$STEP Ecosystem OBS plugin build and install"
		cd "${script_folder}/../../ecosystem/obs_mtl" || exit 1
		cd linux-mtl
		meson setup build
		meson compile -C build
		sudo meson install -C build
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_SAMPLE}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		cd "${script_folder}/../../plugins" || exit 1
		meson setup build
		meson compile -C build
		sudo meson install -C build
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		bash "${script_folder}/../../script/build_st22_avcodec_plugin.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${PLUGIN_BUILD_AND_INSTALL_AVCODEC}" == "1" ]; then
		echo "$STEP Plugin sample build and install"
		bash "${script_folder}/../../script/build_st22_avcodec_plugin.sh"
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_PYTHON}" == "1" ]; then
		echo "$STEP Hook Python"
		cd "${script_folder}/../.." || exit 1
		if [ -d swig ]; then
			echo "SWIG directory already exists, skipping clone."
		else
			echo "Cloning SWIG repository..."
			git clone https://github.com/swig/swig.git
		fi
		cd swig/
		git checkout v4.1.1
		./autogen.sh
		./configure
		make
		sudo make install
		cd "${script_folder}/../../python/swig"
		swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i
		python3 setup.py build_ext --inplace
		sudo python3 setup.py install
		STEP=$((STEP + 1))
	fi

	if [ "${HOOK_RUST}" == "1" ]; then
		echo "$STEP Hook Rust"
		cd "${script_folder}/../../rust" || exit 1
		cargo update home --precise "${RUST_HOOK_CARGO_VER}"
		cargo build --release
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_MONITORS}" == "1" ]; then
		echo "$STEP Tools MTL monitors build and install"
		cd "${script_folder}/../../tools/ebpf" || exit 1
		make lcore_monitor
		make udp_monitor
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_READPCAP}" == "1" ]; then
		echo "$STEP Tools MTL readpcap build and install"
		cd "${script_folder}/../../tools/readpcap" || exit 1
		make
		STEP=$((STEP + 1))
	fi

	if [ "${TOOLS_BUILD_AND_INSTALL_MTL_CPU_EMULATOR}" == "1" ]; then
		echo "$STEP Tools MTL CPU emulator build and install"
		cd "${script_folder}/../../tools/sch_smi_emulate" || exit 1
		make
		STEP=$((STEP + 1))
	fi

fi # End of execution block for script
