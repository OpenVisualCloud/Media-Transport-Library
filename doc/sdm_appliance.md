### Streaming video via Intel(r) SDM appliance

This document  contains instructions for streaming a desktop session to a Intel(r) SDM based display over a 2.5Gbps link (Intel I225 Ethernet) using Intel(r) Media Transport Library.

### 1. Use-case Scenario

![Image](./png/desktop-streaming-mtl.png)

### 2. Required Hardware

1. Intel NUC11TNki5 (Codenamed Tiger Canyon) - Sending Device

2. Intel(r) Smart Display Module (SDM) devkit (Codenamed Alder Valley) - Receiving Device

### 3. Required Software

1. Ubuntu 20.04 / Windows 11

2. Intel&reg; Media Transport Library (Intel&reg; MTL codenamed Kahawai)

3. FFmpeg with kahawai encoder and decoder [patches](https://google.com)

### 2. Installation and configuration

The steps below apply for both Linux and Windows. Please refer to the specific section for each OS.

#### Build Intel&reg; MTL

- See [build.md](./build.md) to build libmtl on linux.

- See [build_WIN_MSYS2.md](./build_WIN_MSYS2.md) to build libmtl on windows.

#### Build ffmpeg (with kahawai encoder and decoder)

* See [../ecosystem/ffmpeg_plugin/README.md](./build.md) to build ffmpeg with Kahawai's encoder/decoder on Ubuntu.

* Follow instruction below (TBD)

#### Sample command line to streeam desktop session using ffmpeg

At the sender (Intel&reg; NUC), run:

```
ffmpeg -f dshow -f gdigrab -framerate 15 -i desktop -vf scale=1920:1080,format=rgb24 -udp_port 20000 -port 0000:58:00.0 -local_addr 192.168.100.32 -dst_addr 239.168.85.20 -f kahawai_mux -
```

Streaming will begin once command is executed. Press CTRL-C to terminate.

At the receiver (Intel&reg; SDM), run:

```
ffmpeg -framerate 15 -pixel_format rgb24 -width 1920 -height 1080 -udpP_port 20000 -port 0000:03:00.0 -local_addr "192.168.100.30" -src_addr "239.168.85.20" -ext_frames_mode 0 -f kahawai -i "k" -f sdl -
```

An SDL window will pop-up at the receiver screen. Press CTRL-C to terminate.

**Note**: See [readme](../ecosystem/ffmpeg_plugin/README.md) for more info on the parameters for the kahawai's avdevice.

### 3. Limitation

- This demo is only tested to stream desktop session in uncompressed raw RGB24 format. 

- At 2.5Gbps bandwidth, we may only stream the session at 1920x1080@30fps in rgb24 pixel format. 
