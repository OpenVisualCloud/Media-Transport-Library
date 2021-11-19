# dpdk.st.kahawai

## 1. Overview:
The Intel Media Streaming Library is a solution based on DPDK prepared for transmitting and receiving high quality video with low latency. It is compliant to the SMPTE ST 2110 Professional Media Over Managed IP Networks suite of standards.

#### 1.1 Features:
* ST2110-20, ST2110-30, ST2110-40, ST2110-22
* 1080p, 720p, 4k
* FPS59.94, FPS50, FPS29
* YUV 4:2:2 10bit

## 2. Build:
Please refer to [build guide](doc/build.md) for how to build DPDK, ST Kahawai library and the sample app.

## 3. Run:
Please refer to [run guide](doc/run.md) for how to setup and run the sample app.

## 4. Coding style:
Run below command before opening a PR.
```bash
./format-coding.sh
```
