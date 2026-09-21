# syntax=docker/dockerfile:1

# Copyright (c) 2025 Intel Corporation.
# SPDX-License-Identifier: BSD-3-Clause

# Rocky Linux 9 with eBPF/XDP support
ARG IMAGE_CACHE_REGISTRY=docker.io
FROM "${IMAGE_CACHE_REGISTRY}/library/rockylinux:9" AS builder

LABEL maintainer="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"

ARG NPROC=20
ARG PREFIX_PATH=/opt/intel
ARG MTL_REPO=${PREFIX_PATH}/mtl
ENV TZ="Europe/Warsaw"

SHELL ["/bin/bash", "-ex", "-o", "pipefail", "-c"]

# Install build dependencies (including eBPF/XDP deps).
#
# glibc-devel.i686 is the RHEL counterpart of the gcc-multilib the Ubuntu images
# install, and it is here for one header: clang compiling the manager's XDP
# object reaches /usr/include/features.h, which includes gnu/stubs-32.h. Without
# it the whole image fails at `Generating mtl.xdp.temp.o`, and only once
# pkg-config can see libxdp -- before that meson leaves the target out and the
# gap stays hidden.
WORKDIR "${MTL_REPO}"
RUN dnf install -y epel-release && \
    dnf config-manager --set-enabled crb && \
    dnf update -y && \
    dnf install -y --allowerasing \
        ca-certificates sudo curl unzip wget patch \
        python3-devel python3-pip python3-pyelftools \
        git gcc gcc-c++ make pkg-config ninja-build \
        numactl-devel json-c-devel libpcap-devel gtest-devel gmock-devel \
        SDL2-devel SDL2_ttf-devel openssl-devel systemtap-sdt-devel \
        m4 clang llvm zlib-devel elfutils-libelf-devel libcap-ng-devel libcap-ng-utils \
        glibc-devel.i686 && \
    dnf clean all && \
    python3 -m pip --no-cache-dir install --upgrade pip setuptools meson ninja

COPY . "${MTL_REPO}"

# Build eBPF/XDP using the project build script
WORKDIR "${MTL_REPO}/script"
RUN ./build_ebpf_xdp.sh

# Build DPDK using the project build script
WORKDIR "${MTL_REPO}/script"
RUN ./build_dpdk.sh -f

# Run the unit suite, then build MTL
WORKDIR "${MTL_REPO}"
RUN ./build.sh unit && \
    ./build.sh && \
    ninja -C build install && \
    DESTDIR=/install ninja -C build install && \
    setcap 'cap_net_raw+ep' tests/tools/RxTxApp/build/RxTxApp

# Rocky Linux 9, runtime/final stage
ARG MTL_REPO
ARG IMAGE_CACHE_REGISTRY
FROM "${IMAGE_CACHE_REGISTRY}/library/rockylinux:9" AS final

LABEL org.opencontainers.image.authors="andrzej.wilczynski@intel.com,dawid.wesierski@intel.com,marek.kasiewicz@intel.com"
LABEL org.opencontainers.image.url="https://github.com/OpenVisualCloud/Media-Transport-Library"
LABEL org.opencontainers.image.title="Intel® Media Transport Library (Rocky Linux 9 + eBPF/XDP)"
LABEL org.opencontainers.image.description="Intel® Media Transport Library (MTL), DPDK + AF_XDP real-time media transport for ST 2110"
LABEL org.opencontainers.image.documentation="https://openvisualcloud.github.io/Media-Transport-Library/README.html"
LABEL org.opencontainers.image.version="1.26.0"
LABEL org.opencontainers.image.vendor="Intel® Corporation"
LABEL org.opencontainers.image.licenses="BSD 3-Clause License"

ARG PREFIX_PATH=/opt/intel
ARG MTL_REPO=${PREFIX_PATH}/mtl
ENV TZ="Europe/Warsaw"
SHELL ["/bin/bash", "-ex", "-o", "pipefail", "-c"]

# Install runtime dependencies
WORKDIR /home/imtl/
RUN dnf install -y epel-release && \
    dnf install -y --allowerasing \
        ca-certificates sudo curl unzip \
        numactl-libs json-c libpcap SDL2 SDL2_ttf openssl-libs zlib elfutils-libelf libcap-ng libatomic pciutils && \
    dnf clean all && \
    echo "Add user: imtl(20001) with group vfio(2110)" && \
    groupadd -g 2110 vfio && \
    useradd -m -G vfio,root -u 20001 imtl

# Copy libraries and binaries.
#
# Meson uses the libdir of the distribution, so DPDK and MTL install to
# /usr/local/lib64 on Rocky Linux and to /usr/local/lib/x86_64-linux-gnu on
# the Debian images. A COPY of the Debian path fails the Rocky build.
COPY --from=builder /usr/local/lib64/* /usr/local/lib64/
COPY --from=builder /usr/local/bin/* /usr/local/bin/
COPY --chown=imtl --from=builder /install /
COPY --chown=imtl --from=builder "${MTL_REPO}/build" "/home/imtl"
COPY --chown=imtl --from=builder "${MTL_REPO}/tests/tools/RxTxApp/build/RxTxApp" "/home/imtl/RxTxApp"
COPY --chown=imtl --from=builder "${MTL_REPO}/tests/tools/RxTxApp/script" "/home/imtl/scripts"
COPY --chown=imtl --from=builder "${MTL_REPO}/script" "/home/imtl/script"

# ldconfig on Rocky Linux reads /lib and /lib64 only, so it does not see
# /usr/local/lib64. Name the directory before ldconfig runs, or RxTxApp and the
# manager do not find libmtl.so and librte_*.so.
RUN echo "/usr/local/lib64" >/etc/ld.so.conf.d/mtl-usr-local-lib64.conf && \
    ldconfig
SHELL ["/bin/bash", "-c"]

USER imtl
CMD ["/bin/bash"]
