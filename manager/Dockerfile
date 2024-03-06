# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# NOTE: This Dockerfile is intended for development purposes only.
# It has been tested for functionality, but not for security.
# Please review and modify as necessary before using in a production environment.

# Build stage, ubuntu 22.04
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37 as builder

LABEL maintainer="ming3.li@intel.com"

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

# Install build dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends git build-essential pkg-config m4 clang llvm zlib1g-dev libelf-dev libpcap-dev libcap-ng-dev meson gcc-multilib ca-certificates

# Clone and build the xdp-tools project
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git
WORKDIR /xdp-tools
RUN ./configure && make && \
    make install && \
    DESTDIR=/install make install
WORKDIR /xdp-tools/lib/libbpf/src
RUN make install && \
    DESTDIR=/install make install

COPY . /manager

# Build the MTL Manager
WORKDIR /manager
RUN meson setup build && \
    DESTDIR=/install meson install -C build

# Runtime stage, ubuntu 22.04
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37

# Install runtime dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends ethtool libelf1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Copy the necessary binaries and libraries from the builder
COPY --from=builder /install /

RUN ldconfig

ENTRYPOINT ["MtlManager"]