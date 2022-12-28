# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

FROM ubuntu

ENV http_proxy=$http_proxy
ENV https_proxy=$http_proxy
ENV no_proxy=$no_proxy
ENV dpdk_ver=22.11

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && \
    apt install -y sudo git gcc meson gdb python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev && \
    apt clean all

# `wget` and `libc6-dev-i386` needed for klocwork scan
RUN apt install -y wget libc6-dev-i386

RUN pip3 install pyelftools

COPY . /opt/mtl.kahawai/
ARG mtl_kahawai=/opt/mtl.kahawai

RUN git config --global http.proxy $http_proxy && \
    git config --global user.email "you@example.com" && \
    git config --global user.name "Your Name"
RUN cd /tmp/ && git clone https://github.com/DPDK/dpdk.git && \
    cd dpdk && \
    git checkout v$dpdk_ver && \
    git switch -c v$dpdk_ver
RUN cd /tmp/dpdk && \
    git am $mtl_kahawai/patches/dpdk/$dpdk_ver/*.patch && \
    meson build && \
    ninja -C build && \
    ninja -C build install && \
    cd /tmp/ && rm -rf dpdk

RUN cd $mtl_kahawai && \
    ./build.sh && ldconfig

WORKDIR /opt/dpdk.st.kahawai
