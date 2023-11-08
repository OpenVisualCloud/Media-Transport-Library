# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

FROM ubuntu@sha256:dfd64a3b4296d8c9b62aa3309984f8620b98d87e47492599ee20739e8eb54fbf

LABEL maintainer="frank.du@intel.com"

ENV MTL_REPO=Media-Transport-Library
ENV DPDK_REPO=dpdk
ENV DPDK_VER=23.07
ENV IMTL_USER=imtl

RUN apt-get update -y

# Install dependencies
RUN apt-get install -y git gcc meson python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev

RUN pip install pyelftools ninja

RUN apt-get install -y sudo

# some misc tools
RUN apt-get install -y vim htop

RUN apt clean all

# user: imtl
RUN adduser $IMTL_USER
RUN usermod -G sudo $IMTL_USER
RUN echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers
USER $IMTL_USER

WORKDIR /home/$IMTL_USER/

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
    sudo ninja -C build install && \
    cd ..

# build mtl
RUN cd $MTL_REPO && \
    ./build.sh && \
    cd ..

CMD ["/bin/bash"]
