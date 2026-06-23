# syntax=docker/dockerfile:1

# Copyright (c) 2025 Intel Corporation.
# SPDX-License-Identifier: BSD-3-Clause

# Ubuntu 22.04 — no eBPF/XDP (unsupported with DPDK >= 26.03 on Ubuntu 22)
ARG IMAGE_CACHE_REGISTRY=docker.io
FROM "${IMAGE_CACHE_REGISTRY}/library/ubuntu:22.04@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37" AS builder

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

ARG NPROC=20
ARG PREFIX_PATH=/opt/intel
ARG MTL_REPO=${PREFIX_PATH}/mtl
ENV DEBIAN_FRONTEND="noninteractive"
ENV TZ="Europe/Warsaw"

SHELL ["/bin/bash", "-ex", "-o", "pipefail", "-c"]

# Install build dependencies
WORKDIR "${MTL_REPO}"
RUN apt-get update -y && \
    apt-get upgrade -y && \
    apt-get install -y --no-install-recommends \
        ca-certificates sudo curl unzip wget \
        apt-transport-https apt-utils python3-dev \
        git build-essential python3-pyelftools pkg-config \
        libnuma-dev libjson-c-dev libpcap-dev libgtest-dev \
        libsdl2-dev libsdl2-ttf-dev libssl-dev systemtap-sdt-dev \
        libcap2-bin && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    curl -fsSL https://bootstrap.pypa.io/get-pip.py | python3 && \
    python3 -m pip --no-cache-dir install --upgrade pip setuptools meson ninja

COPY . "${MTL_REPO}"

# Build DPDK using the project build script
WORKDIR "${MTL_REPO}/script"
RUN ./build_dpdk.sh -f

# Build MTL
WORKDIR "${MTL_REPO}"
RUN ./build.sh && \
    ninja -C build install && \
    DESTDIR=/install ninja -C build install && \
    setcap 'cap_net_raw+ep' tests/tools/RxTxApp/build/RxTxApp

# Ubuntu 22.04, runtime/final stage
ARG MTL_REPO
ARG IMAGE_CACHE_REGISTRY
FROM "${IMAGE_CACHE_REGISTRY}/library/ubuntu:22.04@sha256:149d67e29f765f4db62aa52161009e99e389544e25a8f43c8c89d4a445a7ca37" AS final

LABEL org.opencontainers.image.authors="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"
LABEL org.opencontainers.image.url="https://github.com/OpenVisualCloud/Media-Transport-Library"
LABEL org.opencontainers.image.title="Intel® Media Transport Library"
LABEL org.opencontainers.image.description="Intel® Media Transport Library (MTL), DPDK-based real-time media transport for ST 2110"
LABEL org.opencontainers.image.documentation="https://openvisualcloud.github.io/Media-Transport-Library/README.html"
LABEL org.opencontainers.image.version="1.26.0"
LABEL org.opencontainers.image.vendor="Intel® Corporation"
LABEL org.opencontainers.image.licenses="BSD 3-Clause License"

ARG PREFIX_PATH=/opt/intel
ARG MTL_REPO=${PREFIX_PATH}/mtl
ENV DEBIAN_FRONTEND="noninteractive"
ENV TZ="Europe/Warsaw"
SHELL ["/bin/bash", "-ex", "-o", "pipefail", "-c"]

# Install runtime dependencies
WORKDIR /home/imtl/
RUN apt-get clean -y && rm -rf /var/lib/apt/lists/* && \
    apt-get update -y && \
    apt-get install -y --no-install-recommends ca-certificates sudo curl unzip && \
    apt-get install -y --no-install-recommends libnuma1 libjson-c5 libpcap0.8 libsdl2-2.0-0 libsdl2-ttf-2.0-0 libssl3 libatomic1 && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    echo "Add user: imtl(20001) with group vfio(2110)" && \
    groupadd -g 2110 vfio && \
    useradd -m -G vfio,root,sudo -u 20001 imtl

# Copy libraries and binaries
COPY --chown=imtl --from=builder /install /
COPY --chown=imtl --from=builder "${MTL_REPO}/build" "/home/imtl"
COPY --chown=imtl --from=builder "${MTL_REPO}/tests/tools/RxTxApp/build/RxTxApp" "/home/imtl/RxTxApp"
COPY --chown=imtl --from=builder "${MTL_REPO}/tests/tools/RxTxApp/script" "/home/imtl/scripts"

RUN ldconfig
SHELL ["/bin/bash", "-c"]

USER imtl
HEALTHCHECK --interval=30s --timeout=5s CMD true || exit 1
CMD ["/bin/bash"]
