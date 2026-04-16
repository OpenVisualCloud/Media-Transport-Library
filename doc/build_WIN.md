# Build guide for Windows

## Requirements

- Windows Server 2025

## Prepare build environment

1. Install MSYS2

    Download the latest installer from <https://www.msys2.org/>

1. Install npcap

    Download the latest installer from <https://npcap.com/#download>

1. Run MSYS2 UCRT64

    > **Note:** All the following commands should be executed in UCRT64 environment.

1. Install tools

    ```bash
    pacman -S git pactoys unzip
    ```

    ```bash
    pacboy -S dlfcn:p gcc:p gtest:p json-c:p libpcap:p meson:p mman-win32:p
    ```

1. Install npcap SDK

    ```bash
    wget https://npcap.com/dist/npcap-sdk-1.16.zip
    ```

    ```bash
    unzip -d npcap-sdk-1.16 ./npcap-sdk-1.16.zip
    ```

    ```bash
    cp -r ./npcap-sdk-1.16/lib/x64/. "${MSYSTEM_PREFIX}/lib"
    ```

## Build DPDK

1. Clone the MTL repository

    ```bash
    git clone https://github.com/OpenVisualCloud/Media-Transport-Library.git
    ```

    ```bash
    cd ./Media-Transport-Library
    ```

    ```bash
    MTL_PATH="$PWD"
    ```

1. Clone the DPDK repository

    > **Note:** The DPDK repository should be located directly in the MTL repository.

    ```bash
    git clone -b v25.11 https://github.com/DPDK/dpdk.git
    ```

1. Apply the MTL patches for DPDK

    ```bash
    cd "${MTL_PATH}/dpdk"
    ```

    ```bash
    git am "$MTL_PATH"/patches/dpdk/25.11/*.patch
    ```

    ```bash
    git apply "$MTL_PATH"/patches/dpdk/25.11/windows/*.patch
    ```

1. Build DPDK

    ```bash
    meson setup -Dmax_lcores=256 build
    ```

    ```bash
    meson compile -C build
    ```

    Create a copy of the `sched.h` file

    > **Note:** DPDK installation overwrites the `sched.h` file and cause MTL build problems

    ```bash
    cp "${MSYSTEM_PREFIX}/include/sched.h" "${MTL_PATH}/sched.h.bak"
    ```

    ```bash
    meson install -C build
    ```

    Restore the copy

    ```bash
    cp "${MTL_PATH}/sched.h.bak" "${MSYSTEM_PREFIX}/include/sched.h"
    ```

## Build MTL

1. Run the build script

    ```bash
    cd "$MTL_PATH"
    ```

    ```bash
    ./build.sh debugonly
    ```
