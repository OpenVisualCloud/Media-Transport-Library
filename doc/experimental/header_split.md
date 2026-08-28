# Header Split Guide

## 1. Background

Header split is a hardware-based feature for RX memory copy offload. It can leverage this feature to reduce the memory copy in RX packet processing, thereby improving the performance of high-resolution streaming.

## 2. Build DPDK with header split patch

Note: header split is experimental.

To reproduce the verified configuration, set `DPDK_VER=23.03` in `versions.env` before you run the commands below. Restore the pin after you finish.

Note: point `$mtl_source_code` to the top source code tree of Media Transport Library.

```bash
. $mtl_source_code/versions.env
cd dpdk
git checkout v${DPDK_VER}
git switch -c v${DPDK_VER}
git am $mtl_source_code/patches/dpdk/${DPDK_VER}/*.patch
git am $mtl_source_code/patches/dpdk/${DPDK_VER}/hdr_split/*.patch

# build dpdk
rm build -rf
meson setup build
ninja -C build
sudo ninja install -C build
cd ..
```

## 3. Update DDP package version with header split feature

Double check the DDP version is right from the log.

```text
ice_load_pkg_type(): Active package is: 1.3.9.99, ICE Wireless Edge Package (double VLAN mode)
```

Use below command to update if it's not latest.

```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
cp <latest_ddp_dir>/ice_wireless_edge-1.3.9.99_1.pkg ./
rm ice.pkg
ln -s ice_wireless_edge-1.3.9.99_1.pkg ice.pkg
```

## 4. Run

```bash
./tests/tools/RxTxApp/build/RxTxApp --config_file tests/script/hdr_split_json/1080p59_1v.json --hdr_split
```

Check log to see if header split is enabled.

```text
MT: rv_attach(0), hdr_split enabled in ops
ice_hdrs_mbuf_set_cb(): RX queue 1 register hdrs mbuf cb at 0x7f59f0b2a310
MT: dev_rx_queue_create_flow_raw(1), queue 1 succ
```
