# Header split guide

## 1. Background

Header split is a hardware-based feature for RX memory copy offload. It can leverage this feature to reduce the memory copy in RX packet processing, thereby improving the performance of high-resolution streaming.

## 2. Build DPDK with header split patch

```bash
git am patches/dpdk/23.03/hdr_split/0001-net-ice-support-hdr-split-mbuf-callback.patch
```

## 3. Update DDP package version with header split feature

Double check the DDP version is right from the log.

```bash
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
./build/app/RxTxApp --config_file tests/script/hdr_split/1080p59_1v.json --hdr_split
```

Check log to see if header split is enabled.

```bash
MT: rv_attach(0), hdr_split enabled in ops
ice_hdrs_mbuf_set_cb(): RX queue 1 register hdrs mbuf cb at 0x7f59f0b2a310
MT: dev_rx_queue_create_flow_raw(1), queue 1 succ
```
