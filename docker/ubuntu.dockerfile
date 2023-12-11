# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Ubuntu 22.04
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37

LABEL maintainer="frank.du@intel.com,ming3.li@intel.com"

# Install dependencies and debug tools
RUN apt-get update -y && \
    apt-get install -y git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev && \
    apt-get install -y make m4 clang llvm zlib1g-dev libelf-dev libcap-ng-dev && \
    apt-get install -y sudo vim htop libcap2-bin && \
    pip install pyelftools ninja && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Add user: imtl with group vfio(2110)
RUN groupadd -g 2110 vfio && \
    useradd -m -G vfio,sudo imtl && \
    echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER imtl

WORKDIR /home/imtl/

ENV MTL_REPO=Media-Transport-Library
ENV DPDK_VER=23.11
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

# Clone and build DPDK before bpf and xdp
RUN git clone https://github.com/OpenVisualCloud/$MTL_REPO.git && \
    git clone https://github.com/DPDK/dpdk.git && \
    cd dpdk && \
    git checkout v$DPDK_VER && \
    git switch -c v$DPDK_VER && \
    git config --global user.email "you@example.com" && \
    git config --global user.name "Your Name" && \
    git am ../$MTL_REPO/patches/dpdk/$DPDK_VER/*.patch && \
    meson setup build && \
    ninja -C build && \
    sudo ninja -C build install

# Clone and build the xdp-tools project
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    cd xdp-tools && ./configure && make && sudo make install && \
    cd lib/libbpf/src && sudo make install

# Build IMTL
RUN cd $MTL_REPO && ./build.sh && \
    sudo setcap 'cap_net_admin+ep cap_net_raw+ep' ./build/app/RxTxApp

# Drop the sudo permission
RUN sudo deluser imtl sudo

WORKDIR /home/imtl/$MTL_REPO

CMD ["/bin/bash"]
