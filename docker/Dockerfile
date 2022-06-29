FROM ubuntu

ENV http_proxy=$http_proxy
ENV https_proxy=$http_proxy
ENV no_proxy=$no_proxy

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && \
    apt install -y sudo git gcc meson gdb python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev libsdl2-ttf-dev libssl-dev && \
    apt clean all

# `wget` and `libc6-dev-i386` needed for klocwork scan
RUN apt install -y wget libc6-dev-i386

RUN pip3 install pyelftools

COPY . /opt/dpdk.st.kahawai/
ARG dpdk_st_kahawai=/opt/dpdk.st.kahawai

RUN git config --global http.proxy $http_proxy && \
    git config --global user.email "you@example.com" && \
    git config --global user.name "Your Name"
RUN cd /tmp/ && git clone https://github.com/DPDK/dpdk.git && \
    cd dpdk && \
    git checkout v21.11 && \
    git switch -c v21.11
RUN cd /tmp/dpdk && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0001-build-enable-IEEE1588-PTP-option.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0002-Change-to-enable-PTP.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0003-net-ice-support-max-burst-size-configuration.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0004-eanble-PF-ICE-rate-limit.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0005-update-VF-rate-limit.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0006-net-iavf-support-max-burst-size-configuration.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0007-pcapng-add-ns-timestamp-for-copy-api.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0008-net-ice-support-256-queues.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.11/0009-net-iavf-refine-queue-rate-limit-configure.patch && \
    meson build && \
    ninja -C build && \
    ninja -C build install && \
    cd /tmp/ && rm -rf dpdk

RUN cd $dpdk_st_kahawai && \
    ./build.sh && ldconfig

WORKDIR /opt/dpdk.st.kahawai
