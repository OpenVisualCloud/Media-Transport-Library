# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# NOTE: This Dockerfile is intended for development purposes only.
# It has been tested for functionality, but not for security.
# Please review and modify as necessary before using in a production environment.

# Build stage, ubuntu 24.04
FROM ubuntu:25.10@sha256:7cc5e35f6567ee8c66d2abb4aab0fd866669e6207c237c3a8f0947a5c7f17092 AS builder

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

ARG VERSION

# Install build dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends git build-essential pkg-config m4 clang llvm zlib1g-dev libelf-dev libpcap-dev libcap-ng-dev meson gcc-multilib ca-certificates

# Clone and build the xdp-tools project. Its libbpf submodule can be older than EBPF_VER
# in versions.env, so check out that release instead, as script/build_ebpf_xdp.sh does.
# Change this tag whenever EBPF_VER changes.
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    git -C xdp-tools/lib/libbpf checkout v1.7.0
WORKDIR /xdp-tools
RUN ./configure && make && \
    make install && \
    DESTDIR=/install make install
WORKDIR /xdp-tools/lib/libbpf/src
RUN make install && \
    DESTDIR=/install make install

COPY . /manager
RUN \
    if [ -z "${VERSION}" ]; then \
        VERSION="0.0.1.DOCKER"; \
    fi; \
    echo "$VERSION" > /VERSION

# Build the MTL Manager
WORKDIR /manager
RUN meson setup build && \
    DESTDIR=/install meson install -C build

# Runtime stage, ubuntu 24.04
FROM ubuntu:25.10@sha256:7cc5e35f6567ee8c66d2abb4aab0fd866669e6207c237c3a8f0947a5c7f17092

# Install runtime dependencies
RUN apt-get update -y && \
    apt-get install -y --no-install-recommends ethtool libelf1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Add user: imtl(1001) with group vfio(2110)
RUN groupadd -g 2110 vfio && \
    useradd -m -G vfio -u 1001 imtl

# Copy the necessary binaries and libraries from the builder
COPY --chown=imtl --from=builder /install /

RUN ldconfig

USER imtl

HEALTHCHECK --interval=30s --timeout=5s CMD true || exit 1
ENTRYPOINT ["MtlManager"]
