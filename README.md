# Intel® Media Transport Library

[![Ubuntu](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml)
[![Windows](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/msys2_build.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/msys2_build.yml)
[![Test](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build_with_gtest.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build_with_gtest.yml)
[![OpenSSF
Scorecard](https://api.securityscorecards.dev/projects/github.com/OpenVisualCloud/Media-Transport-Library/badge)](https://api.securityscorecards.dev/projects/github.com/OpenVisualCloud/Media-Transport-Library)
[![CodeQL](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/codeql.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/codeql.yml)
[![Dependency Review](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/dependency-review.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/dependency-review.yml)

## 1. Overview

The Intel® Media Transport Library is a DPDK-based solution designed for high-throughput, low-latency transmission and reception of media data. It features an efficient user-space LibOS UDP stack specifically crafted for media transport, and comes equipped with a built-in SMPTE ST 2110-compliant implementation for Professional Media over Managed IP Networks.

### 1.1 Features

* The User-space LibOS UDP stack features a POSIX socket compatible API.

#### 1.1.1 ST2110 features

* ST2110-10, ST2110-20, ST2110-21, ST2110-30, ST2110-40, ST2110-22, ST2022-7
* 1080p, 720p, 4k, 8k and other
* FPS: 120, 119.88, 100, 60, 59.94, 50, 30, 29.97, 25, 24, 23.98
* All video formats listed in ST2110-20, including YUV 4:2:2 10-bit and others, are supported.
* CSC support status: [status](doc/convert.md)

### 1.2 Architecture

The Intel® Media Transport Library leverages DPDK features to implement a highly efficient, real-time, and low-latency media transport stack. This software-based media transport solution enables deployment on edge and cloud environments using COTS hardware.

The library introduces a tasklet asynchronous-based scheduler that maximizes CPU resources, making it easy to integrate with various packet processing units and accelerators.

The packet pacing module supports various algorithms to achieve narrow pacing, including RL (rate limit) which is partially hardware offloaded and TSC which is fully software-based.

The library also includes SIMD CSC (color space format conversion), DMA, and plugin interfaces to enable the creation of a complete video production ecosystem.

<div align="center">
<img src="doc/png/arch.png" align="center" alt="overall architecture">
</div>

## 2. Build

Please refer to [build guide](doc/build.md) for instructions on how to build DPDK, the library, and the sample application.

For Windows, please refer to the [Win build guide](doc/build_WIN.md) for instructions on how to build.

## 3. Run ST2110

Please refer to [run guide](doc/run.md) for instructions on how to set up and run the demo pipeline application based on VF with SR-IOV.

For Windows, please refer to [Windows run guide](doc/run_WIN.md).

Additionally, please refer to the [VM guide](doc/vm.md) and [Windows VM guide](doc/vm_WIN.md) for instructions on setting up Linux and Windows guest VMs based on VF passthrough.

For AWS (cloud environment), please refer to [AWS run guide](doc/aws.md) for instructions on how to set up and run the demo.

## 4. ST2110 Programmers guide

To quickly develop applications based on the Intel® Media Transport Library, please refer to the [sample code](app/sample).

## 5. User space LibOS UDP stack guide

Starting from version 23.03.0, this library extends support for a user-space UDP stack that runs directly under the current process context for improved performance. Other user-space UDP stacks typically run with a client-service architecture, which introduces cross-core message costs that can negatively impact performance.

Our stack runs the NIC tx/rx function directly from the sendto/recvfrom API, which eliminates the need for cross-core calls and maintains data affinity (LLC) to the UDP consumer.

To learn how to use the LibOS UDP stack, please refer to the [udp doc](doc/udp.md).

## 6. How to Contribute

We welcome community contributions to the Intel® Media Transport Library project. If you have any ideas or issues, please share them with us by using GitHub issues or opening a pull request.

### 6.1 Coding style

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
