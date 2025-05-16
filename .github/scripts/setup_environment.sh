#!/bin/bash

set -xe



function ubuntu_install_dependencies() {
    echo "1.1. Install the build dependency from OS software store"

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

    # CiCd only
    apt install -y python3-venv sudo wget
    python3 -m venv /tmp/mtl-venv
    . /tmp/mtl-venv/bin/activate
    git config --global user.email "you@example.com"
    git config --global user.name "Your Name"

    pip install --upgrade pip
    pip install pyelftools ninja

    sudo apt-get install -y linux-headers-$(uname -r)
}

if [ -f /etc/os-release ]; then
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

if [ "${BUILD_AND_INSTALL_DPDK}" == "1" ]; then
    echo "2. DPDK build and install"
    bash "$(dirname "$0")/../../script/build_dpdk.sh"
fi

if [ "${BUILD_E810_DRIVER}" == "1" ]; then
    echo "3. E810 driver build and install"
    . "$(dirname "$0")/../../script/build_e810_driver.sh"

    echo "Building e810 driver version: $e810_driver_ver form mirror $download_mirror"

    wget "https://downloadmirror.intel.com/${download_mirror}/ice-${e810_driver_ver}.tar.gz"
    tar xvzf ice-1.16.3.tar.gz
    cd ice-1.16.3

    git init
    git add .
    git commit -m "init version ${e810_driver_ver}"
    git am ../../../patches/ice_drv/${e810_driver_ver}/*.patch

    cd src
    make
elif [ "${BUILD_AND_INSTALL_E810_DRIVER}" == "1" ]; then
    echo "3. E810 driver build and install"
    bash "$(dirname "$0")/../../script/build_e810_driver.sh"
fi


