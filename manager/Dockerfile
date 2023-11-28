# First stage: build environment
FROM ubuntu:22.04 AS build-env

LABEL maintainer="ming3.li@intel.com"

ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig

# Install build dependencies
RUN apt-get update -y && \
    apt-get install -y git gcc pkg-config make m4 clang llvm zlib1g-dev libelf-dev libpcap-dev libcap-ng-dev meson

# Clone and build the xdp-tools project
RUN git clone --recurse-submodules https://github.com/xdp-project/xdp-tools.git && \
    cd xdp-tools && ./configure && make && make install && \
    cd lib/libbpf/src && make install

# Clone and build the Media-Transport-Library project
RUN git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git && \
    cd Media-Transport-Library/manager && meson setup build && meson compile -C build

# Second stage: runtime environment
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update -y && \
    apt-get install -y ethtool libelf-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Copy the necessary binaries and libraries from the build-env
COPY --from=build-env /usr/local /usr/local
COPY --from=build-env /Media-Transport-Library/manager/build /app

WORKDIR /app

ENTRYPOINT ["./MtlManager"]