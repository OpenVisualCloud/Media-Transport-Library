# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Build stage, ubuntu 22.04
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37 as builder

LABEL maintainer="ming3.li@intel.com"

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

# Install build dependencies
RUN apt-get update -y && \
    apt-get install -y git gcc pkg-config make m4 clang llvm zlib1g-dev libelf-dev libpcap-dev libcap-ng-dev meson

# Clone and build the xdp-tools project
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    cd xdp-tools && ./configure && make && make install && \
    DESTDIR=/install make install && \
    cd lib/libbpf/src && make install && \
    DESTDIR=/install make install

COPY . manager

# Build the MTL Manager
RUN cd manager && meson setup build && meson compile -C build

# Runtime stage, ubuntu 22.04
FROM ubuntu@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37

# Install runtime dependencies
RUN apt-get update -y && \
    apt-get install -y ethtool libelf1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Copy the necessary binaries and libraries from the builder
COPY --from=builder /install /
COPY --from=builder /manager/build /app

WORKDIR /app

ENTRYPOINT ["./MtlManager"]