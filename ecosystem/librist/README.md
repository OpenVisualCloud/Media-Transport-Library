# The librist support with MTL
From 23.03.0 version, MTL add the support to user-space UDP stack which is POSIX socket compatible.
To allow librist("https://code.videolan.org/rist/librist") running efficiently with the advantage from MTL UDP stack, below is the guide for how to enable this feature.

## Build

Apply the MTL patch on the latest librist open source code, and build with MTL stack. Make sure MTL is build and installed already.
```bash
cd ecosystem/librist/
./build_librist_mtl.sh
```

## Customize the json config

```bash
cd librist/
```
Edit "test/rist/ufd_send.json" and "test/rist/ufd_send.json" as your setup, customize the "port" and "ip" value.

## Run

On TX node, run below sample command, customize dip as setup.
```bash
MUFD_CFG=test/rist/ufd_send.json ./build/test/rist/test_send --sleep_us 1 --sleep_step 3 --dip 192.168.85.80 --sessions_cnt 1
```

On RX node, run below sample command.
```bash
MUFD_CFG=test/rist/ufd_receive.json ./build/test/rist/test_receive --sessions_cnt 1
```
