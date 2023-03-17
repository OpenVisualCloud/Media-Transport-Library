# Header split guide

## 1. Background

Header split is a hardware based feature for rx memory copy offload, it can leverage this feature to reduce the memory copy in rx packet processing, then to improve the performance for high resolution streaming.

## 2. Build DPDK with header split patch

```bash
git am patches/dpdk/22.11/hdr_split/0001-net-ice-support-hdr-split-mbuf-callback.patch
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

Check status log to check how many pkts are with header split and still copied by CPU.

```bash
ST: RX_VIDEO_SESSION(1,0): hdr split pkts 2465924, copy 252341
```
