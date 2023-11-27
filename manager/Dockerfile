FROM ubuntu:22.04

LABEL maintainer="ming3.li@intel.com"

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

WORKDIR /root

RUN apt-get update -y && \
    apt-get install -y git gcc pkg-config make m4 clang llvm zlib1g-dev libelf-dev libpcap-dev libcap-ng-dev meson ethtool && \
    git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    # Build and install libbpf, libxdp and xdp-tools
    cd xdp-tools && ./configure && make && make install && cd lib/libbpf/src && make install && \
    cd /root && git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git && \
    cd Media-Transport-Library/manager && meson setup build && meson compile -C build && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /root/Media-Transport-Library/manager/build

ENTRYPOINT ["./MtlManager"]
