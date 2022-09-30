# Media Transport Library
[![Ubuntu build.](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml/badge.svg)](https://github.com/OpenVisualCloud/Media-Transport-Library/actions/workflows/ubuntu_build.yml)

## 1. Overview:
The Media Transport Library(Kahawai) is a solution based on DPDK prepared for transmitting and receiving high quality video with low latency. It include a compliant implementation to the SMPTE ST 2110 Professional Media Over Managed IP Networks suite of standards.

#### 1.1 Features:
* ST2110-20, ST2110-30, ST2110-40, ST2110-22
* 1080p, 720p, 4k, 8k
* FPS59.94, FPS50, FPS29, FPS25
* YUV 4:2:2 10bit

## 2. Build:
Please refer to [build guide](doc/build.md) for how to build DPDK, the library and the sample application.

For Windows, please refer to [Win build guide](doc/build_WIN.md) for how to build.

## 3. Run:
Please refer to [run guide](doc/run.md) for how to setup and run the demo pipeline application.

For Windows, please refer to [Win run guide](doc/run_WIN.md) for how to setup and run the demo.

For VF and VM support under Linux, please refer to [vf guide](doc/vf.md) for how to setup VF based on SRIOV, [vm guide](doc/vm.md) for how to setup VM based on VF passthrough.

## 4. Programmers guide:
For how to develop application quickly based on Kahawai library, pls refer to [sample code](app/sample).

## 5. Coding style:
Run below command before opening a PR.
```bash
./format-coding.sh
```
