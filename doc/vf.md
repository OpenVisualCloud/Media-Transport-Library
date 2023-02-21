# VF Guide for CVL

## 1. Update driver version to 1.9.11 with patches:

#### 1.1 Get driver version with 1.9.11
Download CVL 1.9.11 driver from Intel website: https://www.intel.com/content/www/us/en/download/19630/intel-network-adapter-driver-for-e810-series-devices-under-linux.html.

#### 1.2 Patch with Kahawai RL patches
Apply the all patches under [ice_driver](../patches/ice_drv/1.9.11/)
```bash
git am $dpdk_st_kahawai/patches/ice_drv/1.9.11/0001-ice-linux-fix-incorrect-memcpy-size.patch
git am $dpdk_st_kahawai/patches/ice_drv/1.9.11/0002-vf-support-kahawai-runtime-rl-queue.patch
git am $dpdk_st_kahawai/patches/ice_drv/1.9.11/0003-ice-set-ICE_SCHED_DFLT_BURST_SIZE-to-2048.patch
git am $dpdk_st_kahawai/patches/ice_drv/1.9.11/0004-version-update-to-kahawai.patch
```

#### 1.3 Build and install the driver
Pls refer to below command for build and install
```bash
cd src
make
sudo make install
sudo rmmod ice
sudo modprobe ice
```
Double check the driver version is right from dmesg.
```bash
[  241.238174] ice: Intel(R) Ethernet Connection E800 Series Linux Driver - version Kahawai_1.9.11_20220803
```

#### 1.4 Update DDP package version to latest(>=1.3.30)
Double check the DDP version is right from dmesg.
```bash
The DDP package was successfully loaded: ICE OS Default Package (mc) version 1.3.30.0
```
Use below command to update if it's not latest.
```bash
cd /usr/lib/firmware/updates/intel/ice/ddp
cp <latest_ddp_dir>/ice-1.3.30.0.pkg ./
rm ice.pkg
ln -s ice-1.3.30.0.pkg ice.pkg
rmmod ice
modprobe ice
```

## 2. Create VF and bind to PMD.
Below is the command to create VF on 0000:af:00.0, and bind the VFs to DPDK PMD.
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

## 3. FAQs:
#### 2.1 vfio_pci not loaded
If you see below error while creating VF, run "modprobe vfio_pci" to install the VFIO_PCI kernel module.
```bash
Error: Driver 'vfio-pci' is not loaded.
```