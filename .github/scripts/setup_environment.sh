#!/bin/bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

set -xe

# SET DEFAULT ARGUMENTS 
: "${SETUP_ENVIRONMENT:=1}"
: "${BUILD_AND_INSTALL_DPDK:=1}"
: "${BUILD_AND_INSTALL_ICE_DRIVER:=1}"
: "${BUILD_AND_INSTALL_EBPF_XDP:=1}"

# CICD ONLY ARGUMENTS
: "${BUILD_ICE_DRIVER:=0}"

function ubuntu_install_dependencies() {
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
        libsdl2-ttf-dev

    # CiCd only
    {
        apt install -y python3-venv sudo wget doxygen
        python3 -m venv /tmp/mtl-venv
        # As this is CICD only we can ignore shellcheck
        # shellcheck disable=SC1091
        . /tmp/mtl-venv/bin/activate
        git config --global user.email "you@example.com"
        git config --global user.name "Your Name"
    }
    # CiCd only end

    pip install --upgrade pip
    pip install pyelftools ninja

    # Ice driver dependencies
    sudo apt-get install -y "linux-headers-$(uname -r)"

    # eBPF XDP dependencies
    sudo apt-get install -y \
        make \
        m4 \
        zlib1g-dev \
        libelf-dev \
        libcap-ng-dev \
        libcap2-bin \
        gcc-multilib
        # clang llvm
}

if [ "$SETUP_ENVIRONMENT" == "1" ]; then
    echo "Environment setup."

    if [ -f /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        case "$ID" in
            ubuntu)
                echo "Detected OS: Ubuntu"
                ubuntu_install_dependencies
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
            rockos|rocky)
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
fi

if [ "${BUILD_AND_INSTALL_EBPF_XDP}" == "1" ]; then
    echo "1.2. Install the build dependency from OS software store"
    bash "$(dirname "$0")/../../script/build_ebpf_xdp.sh"
fi

if [ "${BUILD_AND_INSTALL_DPDK}" == "1" ]; then
    echo "2. DPDK build and install"
    bash "$(dirname "$0")/../../script/build_dpdk.sh"
fi

if [ "${BUILD_ICE_DRIVER}" == "1" ]; then
    echo "3. ICE driver build"
    # shellcheck disable=SC1091
    . "$(dirname "$0")/../../script/build_ice_driver.sh"
    if [ -z "$script_folder" ] || [ -z "$ice_driver_ver" ] || [ -z "$download_mirror" ]; then
        exit 3
    fi
    cd "${script_folder}"

    echo "Building e810 driver version: $ice_driver_ver form mirror $download_mirror"

    wget "https://downloadmirror.intel.com/${download_mirror}/ice-${ice_driver_ver}.tar.gz"
    tar xvzf ice-1.16.3.tar.gz
    cd ice-1.16.3

    git init
    git add .
    git commit -m "init version ${ice_driver_ver}"
    git am ../../patches/ice_drv/"${ice_driver_ver}"/*.patch

    cd src
    make
fi

if [ "${BUILD_AND_INSTALL_ICE_DRIVER}" == "1" ]; then
    echo "3. ICE driver build and install"
    bash "$(dirname "$0")/../../script/build_ice_driver.sh"
fi


