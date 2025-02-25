# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2025 Intel Corporation

# NOTE: This Dockerfile is intended for development purposes only.
# It has been tested for functionality, but not for security.
# Please review and modify as necessary before using in a production environment.

# rockylinux:9.3, build stage
FROM rockylinux@sha256:d7be1c094cc5845ee815d4632fe377514ee6ebcf8efaed6892889657e5ddaaa6 AS builder

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

ENV MTL_REPO=Media-Transport-Library
ENV DPDK_VER=23.11
ENV JSON_C_VER=0.16
ENV PCAP_VER=1.9
ENV GTEST_VER=1.13
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

# Install build dependencies and debug tools
RUN yum update -y && \
    yum install -y git \
        gcc \
        gcc-c++ \
        python3 \
        python3-pip \
        pkg-config \
        SDL2-devel \
        openssl-devel \
        numactl-devel \
        libasan \
        systemtap-sdt-devel \
 # TODO add documentation
        cmake \
        flex \
        bison \
        patch \
        clang \
        llvm \
        zlib-devel \
        elfutils-libelf-devel \
        glibc-devel.i686 \
 # TODO GStreamer documentation
        gstreamer1-plugins-base \
        gstreamer1-plugins-base-devel \
        gstreamer1-plugins-good \
        gstreamer1-devel \
        glib2-devel


RUN pip3 install meson ninja pyelftools

WORKDIR /dependencies
RUN git clone https://github.com/json-c/json-c.git -b json-c-$JSON_C_VER && \
    git clone https://github.com/the-tcpdump-group/libpcap.git -b libpcap-$PCAP_VER && \
    git clone https://github.com/google/googletest.git -b v$GTEST_VER.x && \
    git clone https://github.com/DPDK/dpdk.git && \
    git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git

WORKDIR /dependencies/json-c
RUN mkdir build && cd build && \
    cmake ../ && \
    make -j $(nproc) && \
    make install && \
    DESTDIR=/install make install

WORKDIR /dependencies/libpcap
RUN ./configure && \
    make -j $(nproc) && \
    make install && \
    DESTDIR=/install make install

# Instead of gtest googletest
WORKDIR /dependencies/googletest
RUN mkdir build && cd build && \
    cmake ../ && \
    make -j $(nproc) && \
    make install && \
    DESTDIR=/install make install

# Build the xdp-tools project
WORKDIR /dependencies/xdp-tools
RUN ./configure && make &&\
    make install && \
    DESTDIR=/install make install
WORKDIR /dependencies/xdp-tools/lib/libbpf/src
RUN make install && \
    DESTDIR=/install make install

COPY . /$MTL_REPO/

WORKDIR /dependencies/dpdk
RUN git checkout v$DPDK_VER && \
    git switch -c v$DPDK_VER && \
    for patch in /$MTL_REPO/patches/dpdk/23.11/*.patch; do \
      patch -p1 < "$patch"; \
    done && \
    meson setup build && \
    meson install -C build && \
    DESTDIR=/install meson install -C build

# # secure_path for root user
# RUN sed -i '/^Defaults\s\+secure_path\s*=/ s|$|:/usr/local/bin|' /etc/sudoers

# Build MTL
WORKDIR /$MTL_REPO
RUN ./build.sh && \
    DESTDIR=/install meson install -C build && \
    setcap 'cap_net_raw+ep' ./tests/tools/RxTxApp/build/RxTxApp

WORKDIR /$MTL_REPO/ecosystem/gstreamer_plugin
RUN ./build.sh


FROM rockylinux@sha256:d7be1c094cc5845ee815d4632fe377514ee6ebcf8efaed6892889657e5ddaaa6 AS final

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

# Install build dependencies and debug tools
RUN yum update -y && \
    yum install -y \
    SDL2-devel \
    openssl-devel \
    numactl-devel \
    libasan \
    systemtap-sdt-devel \
    libatomic \
    gstreamer1-plugins-base \
    gstreamer1-plugins-good \
    gstreamer1-devel

# Add user: mtl(1001) with group vfio(2110)
RUN groupadd -g 2110 vfio && \
    useradd -m -G vfio -u 1001 mtl

# Copy libraries and binaries
COPY --chown=mtl --from=builder /install /
COPY --chown=mtl --from=builder /Media-Transport-Library/tests/tools/RxTxApp/build/RxTxApp /home/mtl/RxTxApp
COPY --chown=mtl --from=builder /Media-Transport-Library/tests/tools/RxTxApp/script /home/mtl/scripts
COPY --chown=mtl --from=builder /Media-Transport-Library/ecosystem/gstreamer_plugin/builddir/*.so /home/mtl/gstreamer/

# Setup dpdk
RUN echo -e "/usr/local/lib\n/usr/local/lib64" > /etc/ld.so.conf.d/dpdk.conf
RUN ldconfig

WORKDIR /home/mtl/


USER mtl

ENTRYPOINT ["/bin/bash", "-c"]
