# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

FROM ubuntu

LABEL maintainer="frank.du@intel.com"

ENV MTL_REPO=Media-Transport-Library
ENV DPDK_REPO=dpdk
ENV DPDK_VER=22.11

RUN apt-get update -y

# Install dependencies
RUN apt-get install -y git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev

RUN pip install pyelftools ninja

RUN apt clean all

WORKDIR /opt/

RUN git config --global user.email "you@example.com" && \
    git config --global user.name "Your Name"

RUN git clone https://github.com/OpenVisualCloud/$MTL_REPO.git

RUN git clone https://github.com/DPDK/$DPDK_REPO.git && \
    cd $DPDK_REPO && \
    git checkout v$DPDK_VER && \
    git switch -c v$DPDK_VER

# build dpdk
RUN cd $DPDK_REPO && \
    git am ../Media-Transport-Library/patches/dpdk/$DPDK_VER/*.patch && \
    meson build && \
    ninja -C build && \
    ninja -C build install && \
    cd ..

# build mtl
RUN apt-get install -y sudo
RUN cd $MTL_REPO && \
    ./build.sh && \
    cd ..

CMD ["/bin/bash"]
