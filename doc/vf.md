# VF Guide for CVL

## 1. NIC setup:

#### 1.1 Update driver version(required for CVL)
Get the latest kahawai ice driver: ice-1.7.0_rc67_kahawai_20220110.tgz, then
```bash
cd ice-1.7.0_rc67_kahawai\src
make
make install
rmmod ice
modprobe ice
```
Double check the driver version is right from dmesg.
```bash
ice: Intel(R) Ethernet Connection E800 Series Linux Driver - version 1.7.0_rc67_kahawai_bw_burst_flow_20220110
```

#### 1.2 Update DDP package version(required for CVL)
```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
cp <latest_ddp_dir>/ice-1.3.27.0_mcast_hack_signed.pkg ./
rm ice.pkg
ln -s ice-1.3.27.0_mcast_hack_signed.pkg ice.pkg
rmmod ice
modprobe ice
```
Double check the DDP version is right from dmesg.
```bash
The DDP package was successfully loaded: ICE OS Default Package (mc) version 1.3.27.0
```

#### 1.3 Bind NIC to VF.
Below is the command to bind BDF 0000:af:00.0 to VF mode
```bash
./script/nicctl.sh create_vf 0000:af:00.0
```
Pls check the output to find the VF BDF info, ex 0000:af:01.0 in below example.
```bash
Bind 0000:af:01.0 to vfio success
Bind 0000:af:01.1 to vfio success
Bind 0000:af:01.2 to vfio success
Bind 0000:af:01.3 to vfio success
Bind 0000:af:01.4 to vfio success
Bind 0000:af:01.5 to vfio success
```

#### 1.4 VF PTP setup:
Kahawai supports two PTP mode, one is based on DPDK and process PTP on the fly, and the second is the kernel space PTP.
For VF run, it must use kernel space PTP.
If choosing kernel space PTP procol, here is the steps to set up PTP.
```bash
# Choose a kernel managed interface, and start ptp4l, for example:
ptp4l -m -s  -i enp175s0f0
# Sync up ptp4l with system time
./phc2sys -w -s enp175s0f0 -m -q -r
```

## 2. FAQs:
#### 2.1 vfio_pci not loaded
If you see below error while creating VF, run "modprobe vfio_pci" to install the VFIO_PCI kernel module.
```bash
Error: Driver 'vfio-pci' is not loaded.
```