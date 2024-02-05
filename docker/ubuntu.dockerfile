# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Ubuntu 22.04, build stage
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37 AS builder

LABEL maintainer="frank.du@intel.com,ming3.li@intel.com"

# Install build dependencies and debug tools
RUN apt-get update -y && \
    apt-get install -y git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev && \
    apt-get install -y make m4 clang llvm zlib1g-dev libelf-dev libcap-ng-dev libcap2-bin && \
    pip install pyelftools ninja && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

ENV MTL_REPO=Media-Transport-Library
ENV DPDK_VER=23.11
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

COPY . $MTL_REPO

# Clone and build DPDK before bpf and xdp
RUN git clone https://github.com/DPDK/dpdk.git && \
    cd dpdk && \
    git checkout v$DPDK_VER && \
    git switch -c v$DPDK_VER && \
    git config --global user.email "you@example.com" && \
    git config --global user.name "Your Name" && \
    git am ../$MTL_REPO/patches/dpdk/$DPDK_VER/*.patch && \
    meson setup build && \
    meson install -C build && \
    DESTDIR=/install meson install -C build

# Clone and build the xdp-tools project
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    cd xdp-tools && ./configure && make && make install && \
    DESTDIR=/install make install && \
    cd lib/libbpf/src && make install && \
    DESTDIR=/install make install

# Build IMTL
RUN cd $MTL_REPO && ./build.sh && \
    DESTDIR=/install meson install -C build && \
    setcap 'cap_net_raw+ep' ./build/app/RxTxApp

# Ubuntu 22.04, runtime stage
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37 AS final

LABEL maintainer="frank.du@intel.com,ming3.li@intel.com"

# Install runtime dependencies
RUN apt-get update -y && \
    apt-get install -y libnuma1 libjson-c5 libpcap0.8 libsdl2-2.0-0 libsdl2-ttf-2.0-0 libssl3 zlib1g libelf1 libcap-ng0 libatomic1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Add user: imtl(1001) with group vfio(2110)
RUN groupadd -g 2110 vfio && \
    useradd -m -G vfio -u 1001 imtl

# Copy libraries and binaries
COPY --chown=imtl --from=builder /install /
COPY --chown=imtl --from=builder /Media-Transport-Library/build /home/imtl

# ldconfig
RUN cd /home/imtl && ldconfig

USER imtl

WORKDIR /home/imtl/

CMD ["/bin/bash"]
