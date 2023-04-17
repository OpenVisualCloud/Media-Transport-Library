# The MSDK sample encode

## Build

run `build_msdk_mtl.sh`

## Run

```shell
cd MediaSDK/build/__bin/release

LD_LIBRARY_PATH=. ./sample_encode h265 -i::imtl -o test_4kp30.hevc -w 3840 -h 2160 -mtlfps 30 -d 0000:18:00.1 -ip 192.168.96.1 -lip 192.168.96.2 -port 20000 -pt 112 -y210 -ec::y210
```

Args information:

```shell
   [-d]                            - NIC port BDF (eg: 0000:4b:00.0)
   [-dma]                          - DMA device BDF (eg: 0000:01:00.0)
   [-port]                         - UDP port number (eg: 20000)
   [-ip]                           - Remote IP address
   [-lip]                          - Local IP address
   [-mtlfps]                       - MTL FPS
   [-pt]                           - RTP payload type (eg: 112)
   [-i::imtl]                      - To enable IMTL option
```

More information refer to : [samples/readme-encode_linux.md](https://github.com/Intel-Media-SDK/MediaSDK/blob/master/doc/samples/readme-encode_linux.md)
