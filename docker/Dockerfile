FROM ubuntu

ENV http_proxy=$http_proxy
ENV https_proxy=$http_proxy
ENV no_proxy=$no_proxy

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && \
    apt install -y sudo git gcc meson gdb python3 python3-pip pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libsdl2-dev && \
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
    git checkout v21.08 && \
    git switch -c v21.08
RUN cd /tmp/dpdk && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0001-update-ice-1588-timesync-based-on-21.08.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0002-temp-fix-to-enable-multicast-rx-on-ice.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0003-fix-L4-packet-not-work.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0004-eanble-PF-ICE-rate-limit.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0005-update-VF-rate-limit.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0006-net-iavf-add-capability-for-runtime-rx-tx-queue-setu.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0007-build-enable-IEEE1588-PTP-option.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0008-enable-rx-timestamp-and-fix-performance-issue.patch && \
    git am $dpdk_st_kahawai/patches/dpdk/21.08/0009-net-ice-support-max-burst-size-configuration.patch && \
    meson build && \
    ninja -C build && \
    ninja -C build install && \
    cd /tmp/ && rm -rf dpdk

RUN cd $dpdk_st_kahawai && \
    ./build.sh && ldconfig

WORKDIR /opt/dpdk.st.kahawai
