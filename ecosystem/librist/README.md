# The librist support

From 23.03.0 version, Media Transport Library adds the support to user-space UDP stack which is POSIX socket compatible by the ld preload technology.
To allow librist <https://code.videolan.org/rist/librist> running efficiently with the advantage from MTL UDP stack, below is the guide for how to enable this feature.

## Build

Apply the patch which includes a test routine and ufd JSON config on the latest librist open source code, and build librist and the test. Make sure Media Transport Library is build and installed already.

```bash
cd ecosystem/librist/
./build_librist_mtl.sh
```

## Customize the JSON Config

```bash
cd librist/
```

Edit "test/rist/ufd_send.json" and "test/rist/ufd_send.json" as your setup, customize the "port" and "ip" value.

## Run

On TX node, run below sample command, customize dip(the destination IP address for TX).

```bash
LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so MUFD_CFG=test/rist/ufd_send.json ./build/test/rist/test_send --sleep_us 1 --sleep_step 3 --dip 192.168.85.80 --sessions_cnt 1
```

On RX node, run below sample command, customize bind_ip(the source IP address of RX port) as setup.

```bash
LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so MUFD_CFG=test/rist/ufd_receive.json ./build/test/rist/test_receive --bind_ip 192.168.85.80 --sessions_cnt 1
```

The LD_PRELOAD path can be found in the Media Transport Library build log, see below log part.

```bash
~/share/Media-Transport-Library/build/ld_preload ~/share/Media-Transport-Library
```
```text
[1/1] Linking target udp/libmtl_udp_preload.so
[0/1] Installing files.
Installing udp/libmtl_udp_preload.so to /usr/local/lib/x86_64-linux-gnu
~/share/Media-Transport-Library
```
