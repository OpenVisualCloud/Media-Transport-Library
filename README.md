# Intel® Media Transport Library

[![Ubuntu](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml)
[![Windows](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/msys2_build.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/msys2_build.yml)
[![Test](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build_with_gtest.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build_with_gtest.yml)
[![OpenSSF
Scorecard](https://api.securityscorecards.dev/projects/github.com/OpenVisualCloud/Media-Transport-Library/badge)](https://api.securityscorecards.dev/projects/github.com/OpenVisualCloud/Media-Transport-Library)
[![CodeQL](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/codeql.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/codeql.yml)
[![Dependency Review](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/dependency-review.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/dependency-review.yml)

## 1. Overview

The Intel® Media Transport Library(IMTL) is a software based solution designed for high-throughput, low-latency transmission and reception of media data. It features an efficient user-space LibOS UDP stack specifically crafted for media transport, and comes equipped with a built-in SMPTE ST 2110-compliant implementation for Professional Media over Managed IP Networks.

The Intel® Media Transport Library solves the strict timing challenges of transporting ST2110 compliant media streams using a software library and through IP networks. Instead of specialized hardware, this library leverages existing  commonly available CPU platforms with conventional NICs that incorporate rate limiting to meet the strict timing challenges in the SMPTE ST 2110 standard.

If you find value in our project, please consider giving it a star. Your support helps us grow and reach more people in the open-source community. Every star counts and is greatly appreciated.

### 1.1 Features

* Supported data path backend: DPDK PMD,native kernel socket, and AF_XDP with eBPF filter(still in developing)
* The User-space LibOS UDP stack features a POSIX socket compatible API.
* Non-root run.
* Multi-process handling, allowing for up to 8 NICs per process.
* Virtualization support by SR-IOV.
* Built-in PTP protocol with hardware timestamp offload.
* FFMPEG plugin, OBS(Open Broadcaster Software) plugin, and Intel® Media SDK support.

#### 1.1.1 ST2110 features

* Narrow and wide pacing. Please see [compliance](doc/compliance.md) page for the ST2110 narrow report on our software solution.
* ST2110-10, ST2110-20, ST2110-21, ST2110-30, ST2110-40, ST2022-7
* 1080p, 1080i, 720p, 4k, 8k and other
* FPS: 120, 119.88, 100, 60, 59.94, 50, 30, 29.97, 25, 24, 23.98
* All video formats listed in ST2110-20, including YUV 4:2:2 10-bit and others, are supported.
* SIMD color space converter between big-endian and little-endian.
* ST2110-22 with encoder/decoder plugin interface.
* ST2022-6 by RTP passthrough interface.

### 1.2 Architecture

The Intel® Media Transport Library leverages DPDK (Data Plane Development Kit) EAL (Environment Abstraction Layer including the memory and core management) to implement a highly efficient, real-time, and low-latency media transport solution. This software-based media transport stack enables deployment on edge and cloud environments using COTS hardware.

The library incorporates a virtual data path backend layer, designed to abstract various NIC implementation and provide a unified packet TX/RX interface to the upper network layer. It currently supports three types of NIC devices:

* DPDK Poll-Mode Drivers (PMDs): These drivers fully bypass the kernel's networking stack, utilizing the 'poll mode' driver. This approach provides direct hardware access, eliminates heavy user/kernel context switches, and avoids IRQ (Interrupt Request) switches. As a result, DPDK PMDs deliver ultra-low latency and high throughput, making them ideal for demanding networking applications.
* Native Linux Kernel Network Socket Stack: This option supports the full range of kernel ecosystems.
* AF_XDP (Express Data Path) with eBPF filter(still in developing): AF_XDP represents a significant advancement in the Linux networking stack, striking a balance between raw performance and integration with the kernel's networking ecosystem. It's particularly valuable in scenarios where performance is critical, but a full kernel bypass solution like DPDK is not feasible or desired.

The library introduces a tasklet-based asynchronous scheduler that optimizes CPU resource utilization, facilitating integration with various packet processing units and accelerators.

Additionally, the packet pacing module offers support for various pacing algorithms, including RL (Rate Limit), which is partially hardware-offloaded, and TSC (timestamp Counter), which is fully software-based.

IMTL also incorporates SIMD (Single Instruction, Multiple Data) for CSC (Color Space Format Conversion) of the big-endian and little-endian, DMA (Direct Memory Access), and plugin interfaces, enabling the creation of a comprehensive video production ecosystem.

For the detail design, please refer to [design guide](doc/design.md).

<div align="center">
<img src="doc/png/arch.svg" align="center" alt="/Overall Architecture">
</div>

### 1.3 Ethernet supported

IMTL offers versatile Ethernet support, thanks to its compatibility with DPDK PMD, kernel socket, and AF_XDP backends.

For DPDK PMD support, you can refer to the DPDK PMD site <https://doc.dpdk.org/guides/nics/> for a comprehensive list of supported Ethernet hardware.

In cases where your NIC is not supported by DPDK, IMTL provides a fallback option with kernel (Linux) socket transport support.

However, please note that our daily development and validation is primarily conducted on the Intel E810 series and AWS ENA, so we can't guarantee the status for other network interface cards (NICs).

An important point to note is that narrow pacing of TX is only supported for the Intel E810 series together with DPDK PMD due to the rate limit feature. All other type of usage have to use TSC (Timestamp Counter) as the pacing source, which can only ensure a broad wide pacing.

## 2. Build

Please refer to [build guide](doc/build.md) for instructions on how to build DPDK, the library, and the sample application.

For Windows, please refer to the [Win build guide](doc/build_WIN.md) for instructions on how to build.

## 3. Run ST2110

Please refer to [run guide](doc/run.md) for instructions on how to set up and run the demo pipeline application based on DPDK PMD backend.

For Windows, please refer to [Windows run guide](doc/run_WIN.md).

Additionally, please refer to the [VM guide](doc/vm.md) and [Windows VM guide](doc/vm_WIN.md) for instructions on setting up Linux and Windows guest VMs based on VF passthrough.

For AWS (cloud environment), please refer to [AWS run guide](doc/aws.md) for instructions on how to set up and run the demo.

To run this library on the kernel network stack with the built-in kernel NIC driver, please follow the instructions provided in the [kernel socket guide](doc/kernel_socket.md).

## 4. ST2110 Programmers guide

To quickly develop applications based on the Intel® Media Transport Library, please refer to `## 6. ST2110 API` from [design guide](doc/design.md).

## 5. User space LibOS UDP stack guide

IMTL has support for a LD preload POSIX-compatible user-space UDP stack that operates directly within the current process context. This enhancement significantly boosts performance by eliminating the cross-core message costs typically associated with client-service architectures used in other user-space UDP stacks.
IMTL's stack allows the NIC transmission and reception functions to run directly from the sendto/recvfrom API, eliminating the need for cross-core calls and maintaining data affinity (LLC) to the UDP consumer, thereby optimizing performance.

To learn how to use the LibOS UDP stack, please refer to the [udp doc](doc/udp.md).

## 6. Publication

MHV'23: A Real-time Media Transport Stack Based on Commercial Off-the-shelf Hardware. <https://dl.acm.org/doi/10.1145/3588444.3591002>

Whitepaper: Open Source Library Enables Real-Time Media over IP Networks. <https://www.intel.com/content/www/us/en/content-details/786203/open-source-library-enables-real-time-media-over-ip-networks.html>

2022 DPDK Userspace Summit: Real-time and low latency media transport stack based on DPDK. <https://www.youtube.com/watch?v=fiiOvHezpBs>

## 7. How to Contribute

We welcome community contributions to the Intel® Media Transport Library project. If you have any ideas or issues, please share them with us by using GitHub issues or opening a pull request.

### 7.1 Fork this repository

Before opening a pull request, please follow these steps:

1. [Fork](https://github.com/OpenVisualCloud/Media-Transport-Library/fork) this repository to your own space.
2. Create a new branch for your changes.
3. Make your changes and commit them.
4. Push your changes to your forked repository.
5. Open a pull request to the main repository.

If you do not want the main branch automatically synced to the upstream, please go to `Actions` and disable the `Upstream Sync` workflow.

### 7.2 Coding style

We use the super-linter action for style checks.

For C/C++ coding, you can run the following command to quickly fix the style:

```bash
./format-coding.sh
```

For other languages, please check with the following example command inside the Docker container:

```bash
# super-linter
docker run -it --rm  -v "$PWD":/opt/ --entrypoint /bin/bash github/super-linter

cd /opt/

# echo "shell check"
find ./ -name "*.sh" -exec shellcheck {} \;

# hadolint check
hadolint docker/ubuntu.dockerfile

# actionlint check
actionlint

# markdownlint check
find ./ -name "*.md" -exec markdownlint {} -c .markdown-lint.yml \;
# find ./ -name "*.md" -exec markdownlint {} --fix -c .markdown-lint.yml \;

# textlint
find ./ -name "*.md" -exec textlint {} \;
# find ./ -name "*.md" -exec textlint {} --fix \;
```
